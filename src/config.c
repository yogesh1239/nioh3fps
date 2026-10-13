/* nioh3fps.ini: every key validated; an invalid value falls back to its default and logs a line. */
#include "common.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <math.h>

config_t g_cfg;

void config_defaults(config_t *c)
{
    memset(c, 0, sizeof *c);
    c->enabled = 1;
    c->start_enabled = 1;       /* S2L experiment, offline test procedure */
    c->lockstep = 1;
    c->heartbeat_source = 0;    /* gameplay packet take */
    c->toggle_key = (keycombo){ VK_F10, 0, 0, 0 };
    c->dump_key = (keycombo){ VK_F10, 1, 0, 0 };
    c->marker_key = (keycombo){ VK_F9, 0, 0, 0 };
    c->fc.mode = TM_RAW;
    c->fc.avg_frames = 8;
    c->fc.smooth_k = 0.2;
    c->fc.scale_min = 0.5;
    c->fc.scale_max = 2.0;
    c->fc.safe_mode = 0;
    c->log_level = 0;
}

static int get(const wchar_t *path, const wchar_t *key, char *out, size_t n)
{
    wchar_t w[128];
    DWORD k = GetPrivateProfileStringW(L"nioh3fps", key, L"\x01", w, 128, path);
    char *e;
    if (k == 1 && w[0] == 1) return 0;   /* missing */
    if (!n) return 0;
    if (!WideCharToMultiByte(CP_UTF8, 0, w, -1, out, (int)n, NULL, NULL)) {
        out[0] = 0;          /* failed (e.g. too long): treat as an invalid, empty value */
        return 1;
    }
    out[n - 1] = 0;
    /* strip inline comments and whitespace */
    if ((e = strchr(out, ';')) != NULL) *e = 0;
    if ((e = strchr(out, '#')) != NULL) *e = 0;
    e = out + strlen(out);
    while (e > out && (e[-1] == ' ' || e[-1] == '\t')) *--e = 0;
    while (*out == ' ' || *out == '\t') memmove(out, out + 1, strlen(out));
    return 1;
}

static int parse_bool(const char *s, int *v)
{
    if (!_stricmp(s, "1") || !_stricmp(s, "true") || !_stricmp(s, "yes") || !_stricmp(s, "on")) { *v = 1; return 1; }
    if (!_stricmp(s, "0") || !_stricmp(s, "false") || !_stricmp(s, "no") || !_stricmp(s, "off")) { *v = 0; return 1; }
    return 0;
}

static int parse_double(const char *s, double *v)
{
    char *end;
    double d = strtod(s, &end);
    if (end == s || *end || isnan(d) || isinf(d)) return 0;
    *v = d;
    return 1;
}

static int parse_int(const char *s, int *v)
{
    char *end;
    long d = strtol(s, &end, 10);
    if (end == s || *end) return 0;
    *v = (int)d;
    return 1;
}

static int vk_from_name(const char *s)
{
    int n;
    if ((s[0] == 'F' || s[0] == 'f') && s[1] >= '1' && s[1] <= '9') {
        n = atoi(s + 1);
        if (n >= 1 && n <= 24) {
            char tmp[8]; snprintf(tmp, sizeof tmp, "%d", n);
            if (!strcmp(tmp, s + 1)) return VK_F1 + n - 1;
        }
        return 0;
    }
    if (strlen(s) == 1 && ((s[0] >= 'A' && s[0] <= 'Z') || (s[0] >= '0' && s[0] <= '9'))) return s[0];
    if (strlen(s) == 1 && s[0] >= 'a' && s[0] <= 'z') return s[0] - 32;
    if (!_stricmp(s, "Insert")) return VK_INSERT;
    if (!_stricmp(s, "Delete")) return VK_DELETE;
    if (!_stricmp(s, "Home")) return VK_HOME;
    if (!_stricmp(s, "End")) return VK_END;
    if (!_stricmp(s, "PageUp")) return VK_PRIOR;
    if (!_stricmp(s, "PageDown")) return VK_NEXT;
    if (!_stricmp(s, "Pause")) return VK_PAUSE;
    if (!_stricmp(s, "ScrollLock")) return VK_SCROLL;
    return 0;
}

static int parse_key(const char *s, keycombo *k)
{
    char buf[64], *tok, *ctx = NULL;
    keycombo r = { 0, 0, 0, 0 };
    if (!_stricmp(s, "None")) { *k = r; return 1; }
    snprintf(buf, sizeof buf, "%s", s);
    for (tok = strtok_s(buf, "+", &ctx); tok; tok = strtok_s(NULL, "+", &ctx)) {
        while (*tok == ' ') tok++;
        char *e = tok + strlen(tok);
        while (e > tok && e[-1] == ' ') *--e = 0;
        if (!_stricmp(tok, "Ctrl") || !_stricmp(tok, "Control")) r.ctrl = 1;
        else if (!_stricmp(tok, "Shift")) r.shift = 1;
        else if (!_stricmp(tok, "Alt")) r.alt = 1;
        else if (r.vk) return 0;
        else if (!(r.vk = vk_from_name(tok))) return 0;
    }
    if (!r.vk) return 0;
    *k = r;
    return 1;
}

const char *keycombo_str(const keycombo *k, char *buf, size_t n)
{
    char key[16];
    if (!k->vk) { snprintf(buf, n, "None"); return buf; }
    if (k->vk >= VK_F1 && k->vk <= VK_F24) snprintf(key, sizeof key, "F%d", k->vk - VK_F1 + 1);
    else if (k->vk < 128 && k->vk > 32) snprintf(key, sizeof key, "%c", k->vk);
    else snprintf(key, sizeof key, "VK%02X", k->vk);
    snprintf(buf, n, "%s%s%s%s", k->ctrl ? "Ctrl+" : "", k->shift ? "Shift+" : "", k->alt ? "Alt+" : "", key);
    return buf;
}

#define BAD(key, val, why) log_printf("CONFIG: %s=%s invalid (%s); using default", key, val, why)

void config_load(const wchar_t *path, config_t *c)
{
    char v[128];
    double d; int i; keycombo k;
    config_defaults(c);
    c->ini_found = GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES;
    if (!c->ini_found) return;

    if (get(path, L"Enabled", v, sizeof v)) { if (parse_bool(v, &i)) c->enabled = i; else BAD("Enabled", v, "expected 0/1"); }
    if (get(path, L"StartEnabled", v, sizeof v)) { if (parse_bool(v, &i)) c->start_enabled = i; else BAD("StartEnabled", v, "expected 0/1"); }
#ifndef RELEASE
    if (get(path, L"ToggleFile", v, sizeof v)) { if (parse_bool(v, &i)) c->toggle_file = i; else BAD("ToggleFile", v, "expected 0/1"); }
#endif
    if (get(path, L"Lockstep", v, sizeof v)) { if (parse_bool(v, &i)) c->lockstep = i; else BAD("Lockstep", v, "expected 0/1"); }
    if (get(path, L"HeartbeatSource", v, sizeof v)) {
        if (!_stricmp(v,"take")) c->heartbeat_source=0;
        else if (!_stricmp(v,"xell_entry")) c->heartbeat_source=1;
        else if (!_stricmp(v,"xell_exit")) c->heartbeat_source=2;
        else BAD("HeartbeatSource",v,"expected take, xell_entry or xell_exit");
    }
    if (get(path, L"SafeMode", v, sizeof v)) { if (parse_bool(v, &i)) c->fc.safe_mode = i; else BAD("SafeMode", v, "expected 0/1"); }
    if (get(path, L"ToggleKey", v, sizeof v)) { if (parse_key(v, &k)) c->toggle_key = k; else BAD("ToggleKey", v, "unknown key"); }
    if (get(path, L"DumpKey", v, sizeof v)) { if (parse_key(v, &k)) c->dump_key = k; else BAD("DumpKey", v, "unknown key"); }
    if (get(path, L"MarkerKey", v, sizeof v)) { if (parse_key(v, &k)) c->marker_key = k; else BAD("MarkerKey", v, "unknown key"); }
    if (get(path, L"TimingMode", v, sizeof v)) {
        if (!_stricmp(v, "Raw")) c->fc.mode = TM_RAW;
        else if (!_stricmp(v, "Smooth")) c->fc.mode = TM_SMOOTH;
        else BAD("TimingMode", v, "expected Raw or Smooth");
    }
    if (get(path, L"AvgFrames", v, sizeof v)) {
        if (parse_int(v, &i) && i >= 1 && i <= FACTOR_HIST_MAX) c->fc.avg_frames = i; else BAD("AvgFrames", v, "range [1, 32]");
    }
    if (get(path, L"SmoothK", v, sizeof v)) {
        if (parse_double(v, &d) && d > 0.0 && d <= 1.0) c->fc.smooth_k = d; else BAD("SmoothK", v, "range (0, 1]");
    }
    if (get(path, L"ScaleMin", v, sizeof v)) {
        if (parse_double(v, &d) && d >= 0.5 && d <= 1.0) c->fc.scale_min = d; else BAD("ScaleMin", v, "range [0.5, 1.0]");
    }
    if (get(path, L"ScaleMax", v, sizeof v)) {
        if (parse_double(v, &d) && d >= 1.0 && d <= 2.0) c->fc.scale_max = d; else BAD("ScaleMax", v, "range [1.0, 2.0]");
    }
    if (get(path, L"LogLevel", v, sizeof v)) {
        if (!_stricmp(v, "Info")) c->log_level = 0;
        else if (!_stricmp(v, "Debug")) c->log_level = 1;
        else BAD("LogLevel", v, "expected Info or Debug");
    }
    if (c->toggle_key.vk && c->toggle_key.vk == c->dump_key.vk && c->toggle_key.ctrl == c->dump_key.ctrl &&
        c->toggle_key.shift == c->dump_key.shift && c->toggle_key.alt == c->dump_key.alt) {
        log_printf("CONFIG: DumpKey equals ToggleKey; DumpKey disabled");
        c->dump_key.vk = 0;
    }
}

void config_log(const config_t *c)
{
    char a[32], b[32], m[32];
    log_printf("CONFIG: ini %s", c->ini_found ? "found" : "not found (defaults)");
#ifndef RELEASE
    log_printf("CONFIG: ToggleFile=%d (debug-only, background poll)",c->toggle_file);
#endif
    log_printf("CONFIG: Lockstep=%d HeartbeatSource=%s",c->lockstep,
               c->heartbeat_source==0 ? "take" : c->heartbeat_source==1 ? "xell_entry" : "xell_exit");
    log_printf("CONFIG: Enabled=%d StartEnabled=%d SafeMode=%d ToggleKey=%s DumpKey=%s MarkerKey=%s",
               c->enabled, c->start_enabled, c->fc.safe_mode, keycombo_str(&c->toggle_key, a, sizeof a),
               keycombo_str(&c->dump_key, b, sizeof b), keycombo_str(&c->marker_key, m, sizeof m));
    log_printf("CONFIG: TimingMode=%s AvgFrames=%d SmoothK=%.4g ScaleMin=%.4g ScaleMax=%.4g LogLevel=%s",
               c->fc.mode == TM_RAW ? "Raw" : "Smooth", c->fc.avg_frames, c->fc.smooth_k,
               c->fc.scale_min, c->fc.scale_max, c->log_level ? "Debug" : "Info");
#if PROBE_BUILD
    log_printf("CONFIG: verbose diagnostics build; S2L scaling still follows the lockstep runtime decision");
#endif
}
