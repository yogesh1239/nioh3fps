/* P1/P2 stub pages (PLAN section 3): one allocation of 2 adjacent pages.
   Page 1 = stubs + copied host UNWIND_INFO + RUNTIME_FUNCTION table (written, then RX).
   Page 2 = RW data, each value on its own 64-byte line.
   Shared by the DLL and tests/test_unwind.c so the test exercises the exact bytes. */
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <windows.h>

#define STUB_PAGE 0x1000

/* page-2 data layout (offsets from page 2) */
#define DATA_R1      0x000
#define DATA_R2      0x040
#define DATA_C60     0x080
#define DATA_OVL_H1  0x0C0
#define DATA_OVL_H2  0x100

/* page-1 layout (offsets from the allocation base) */
#define STUB_P1_OFF  0x000
#define STUB_P2_OFF  0x040
#define STUB_UW1_OFF 0x100
#define STUB_UW2_OFF 0x140
#define STUB_TBL_OFF 0x200

#define STUB_P1_LEN  36
#define STUB_P2_LEN  42

typedef struct {
    uint8_t       *alloc;              /* 2 pages, page 1 writable while building */
    uintptr_t      p1_return;          /* base + 0x5ad6b2 */
    uintptr_t      p2_return;          /* base + 0x39e537 */
    const uint8_t *p1_host_unwind;     /* host UNWIND_INFO (original bytes) */
    const uint8_t *p2_host_unwind;
} stubs_params;

typedef struct {
    uint8_t           *p1_stub, *p2_stub;
    RUNTIME_FUNCTION  *table;
    DWORD              count;
    volatile float    *r1, *r2, *c60;
    volatile long     *ovl_h1, *ovl_h2;
    /* instruction start offsets inside each stub (for the unwind test) */
    int                p1_insn[32], p1_ninsn;
    int                p2_insn[32], p2_ninsn;
} stubs_layout;

/* Length of a host UNWIND_INFO copy: 4 + 2*((CountOfCodes+1)&~1). */
size_t stubs_unwind_len(const uint8_t *ui);
/* Transform a host UNWIND_INFO for use by a stub that runs inside the host's body:
   Flags = 0, SizeOfProlog = 0, CodeOffset = 0 in the first slot of every code.
   Returns 0 on success, -1 if the info is unsupported (version != 1, chained,
   frame register set, unknown opcode). */
int stubs_transform_unwind(const uint8_t *src, uint8_t *dst, size_t *len, char *err, size_t errlen);
/* Write both stubs, unwind copies and the function table; initialise page 2
   (r1 = r2 = 1.0, c60 = 60.0, counters 0). Does not change protection. */
int stubs_build(const stubs_params *p, stubs_layout *out, char *err, size_t errlen);
void stubs_s2l(const stubs_params *p, stubs_layout *o, uint32_t req, uint32_t eff, DWORD tls);
