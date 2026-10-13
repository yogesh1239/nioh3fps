#include "consumer_probe.h"
#include "consumer_fix.h"
#include <string.h>

_Static_assert(sizeof(cf_tick)==16,"one atomic r/seq pair");
static cf_tick published __attribute__((aligned(16)))={1,0,0,0};
static uint32_t fraction(float r)
{
    if (!(r>0) || r>1000) return 0;
    double rate=120.0/r;
    return rate>=1 && rate<=10000 ? (uint32_t)((rate-(int)rate)*2147483648.0) : 0;
}
static int tick_cas(cf_tick *expected,cf_tick desired)
{
    uint64_t lo,hi,newlo,newhi; unsigned char ok;
    memcpy(&lo,expected,8); hi=expected->seq;
    memcpy(&newlo,&desired,8); newhi=desired.seq;
    __asm__ volatile("lock cmpxchg16b %1; sete %0"
        : "=q"(ok), "+m"(published), "+a"(lo), "+d"(hi)
        : "b"(newlo), "c"(newhi) : "cc", "memory");
    memcpy(expected,&lo,8); expected->seq=hi; return ok;
}
cf_tick consumer_fix_snapshot(void)
{
    cf_tick value={0}; tick_cas(&value,value); return value;
}
void consumer_fix_publish(float r,uint64_t seq,int armed)
{
    cf_tick before=consumer_fix_snapshot(),after={r,0,armed!=0,seq};
    do {
        /* Diffuse the previous tick once, regardless of calls/actor count.
           The phase travels in the r/seq pair so a late reader cannot mix it. */
        after.phase=armed && before.armed ?
            (before.phase+(before.seq!=seq ? fraction(before.r) : 0))&0x7fffffffu : 0;
    } while (!tick_cas(&before,after));
}

/* Constant-r sequence helper for arithmetic tests; runtime uses the published
   phase, which also diffuses correctly when r changes from tick to tick. */
int cf_rate(float r,uint64_t seq,unsigned site,unsigned policy)
{
    if (!(r>0) || r>1000) return 120;
    double rate=120.0/r;
    if (rate<1) rate=1;
    if (rate>10000) rate=10000;
    int lo=(int)rate;
    if (policy==CF_ROUND) return (int)(rate+0.5);
    if (policy!=CF_RATE) return 120;
    uint32_t part=fraction(r),phase=(uint32_t)(site*2654435761u)&0x7fffffffu;
    uint32_t remainder=(uint32_t)(seq*part+phase)&0x7fffffffu;
    return lo+((uint64_t)remainder+part>=2147483648ull);
}
int cf_tick_rate(cf_tick tick,unsigned site,unsigned policy)
{
    if (policy!=CF_RATE) return cf_rate(tick.r,tick.seq,site,policy);
    if (!(tick.r>0) || tick.r>1000) return 120;
    double rate=120.0/tick.r;
    if (rate<1) rate=1;
    if (rate>10000) rate=10000;
    uint32_t phase=(tick.phase+(uint32_t)(site*2654435761u))&0x7fffffffu;
    return (int)rate+((uint64_t)phase+fraction(tick.r)>=2147483648ull);
}
float cf_x_speed(float effective,float requested,int armed)
{
    return !armed ? effective : effective==0 ? 0 : requested;
}
int cf_guard(double *hot,uint64_t calls,double seconds,int armed,int known)
{
    if (seconds<=0 || !armed || known || (double)calls/seconds<=10) *hot=0;
    else *hot+=seconds;
    if (*hot<5) return 0;
    *hot=0; return 1;
}
int cf_find(const cf_site *sites,unsigned count,uintptr_t ret)
{
    unsigned low=0,high=count;
    while (low<high) { unsigned mid=low+(high-low)/2;
        if (sites[mid].ret<ret) low=mid+1; else high=mid;
    }
    return low<count && sites[low].ret==ret ? (int)low : -1;
}
int cf_core_ok(const cf_site *sites,const unsigned char *placed,unsigned count)
{
    for (unsigned i=0;i<count;i++) if (sites[i].core && !placed[i]) return 0;
    return 1;
}
double cp_steps_per_second(uint64_t steps, uint64_t calls, uint64_t ticks, double seconds)
{
    return calls && seconds>0 ? (double)steps/calls * ((double)ticks/seconds) : 0;
}
int cp_slot_hit(cp_table *t, uintptr_t ret, uintptr_t outer, unsigned state)
{
    if (!ret || ret==UINTPTR_MAX || state>=CP_STATES) return -1;
    unsigned home=(unsigned)(((ret ^ (outer*0x9e3779b97f4a7c15ull))*0x9e3779b97f4a7c15ull)>>53);
    for (unsigned n=0;n<32;n++) {
        unsigned i=(home+n)&(CP_SLOTS-1); cp_slot *s=&t->slot[i];
        uintptr_t k=__atomic_load_n(&s->ret,__ATOMIC_ACQUIRE);
        if (!k) {
            /* Reserve with sentinel, initialise the second key before publishing. */
            uintptr_t zero=0;
            if (__atomic_compare_exchange_n(&s->ret,&zero,UINTPTR_MAX,0,__ATOMIC_RELAXED,__ATOMIC_RELAXED)) {
                s->outer=outer;
                __atomic_store_n(&s->ret,ret,__ATOMIC_RELEASE); k=ret;
            } else k=zero;
        }
        if (k==ret && s->outer==outer) {
            __atomic_fetch_add(&s->count[state],1,__ATOMIC_RELAXED); return (int)i;
        }
        /* A concurrent claim can produce duplicate slots, merged by the writer. */
    }
    __atomic_fetch_add(&t->overflow,1,__ATOMIC_RELAXED); return -1;
}
int cp_helper_decode(const uint8_t *p,size_t n,uintptr_t at,uintptr_t fps,uintptr_t *ret,unsigned *frame)
{
    int32_t d;
    if (n<9 || memcmp(p,"\x48\x83\xec",3) || p[3]<0x20 || p[3]>=0x80 || (p[3]&15)!=8 || p[4]!=0xe8) return 0;
    memcpy(&d,p+5,4);
    if (at+9+d!=fps) return 0;
    *ret=at+9; *frame=p[3]; return 1;
}
uintptr_t cp_outer_return(const void *stack,uintptr_t helper_ret,unsigned frame)
{
    const uintptr_t *rsp=stack;
    if (!helper_ret || *rsp!=helper_ret) return 0;
    return *(const uintptr_t *)((const uint8_t *)stack+frame+8);
}
