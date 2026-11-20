/* printf and scanf over guest arguments (Darwin i386 layout: int/long/pointer 4 bytes, long long 8, double 8,
 * long double 16; wchar_t 4 bytes, like the host's).
 *
 * One engine serves the narrow and wide families: format and output are sequences of code units (bytes, or 32-bit
 * wchar_t). Each conversion is formatted by the host's snprintf with the same flags, width and precision. */
#include "mac.h"
#include <wchar.h>
#include <stdarg.h>
#include <ctype.h>
#include <errno.h>

typedef struct Out { uint32_t *u; size_t n, cap; } Out;
static void out_unit(Out *o, uint32_t v)
{
    if (o->n + 1 >= o->cap) { o->cap = o->cap ? o->cap * 2 : 256; o->u = realloc(o->u, o->cap * 4); }
    o->u[o->n++] = v;
}
static void out_bytes(Out *o, const char *s, size_t n) { for (size_t k = 0; k < n; k++) out_unit(o, (uint8_t)s[k]); }

/* fmt: guest address of the format; wide: 1 = wchar_t units; ap: guest address of the first variable argument.
 * Returns the formatted units (caller frees), *len their count. */
static uint32_t *format(uint32_t fmt, int wide, uint32_t ap, size_t *len)
{
    Out o = {0};
    const uint8_t *M = G_MEM;
#define U(k) (wide ? rt_r32(M, fmt + 4u * (k)) : (uint32_t)M[fmt + (k)])
    size_t i = 0;
    for (;;) {
        uint32_t ch = U(i);
        if (!ch) break;
        if (ch != '%') { out_unit(&o, ch); i++; continue; }
        char spec[64]; int sn = 0; spec[sn++] = '%'; i++;
        if (U(i) == '%') { out_unit(&o, '%'); i++; continue; }
        while (strchr("-+ #0'", (int)U(i)) && U(i) && sn < 40) spec[sn++] = (char)U(i++);
        if (U(i) == '*') { int w = (int)rt_r32(M, ap); ap += 4; sn += snprintf(spec + sn, sizeof spec - (size_t)sn, "%d", w); i++; }
        else while (U(i) >= '0' && U(i) <= '9' && sn < 50) spec[sn++] = (char)U(i++);
        if (U(i) == '.') {
            spec[sn++] = '.'; i++;
            if (U(i) == '*') { int p = (int)rt_r32(M, ap); ap += 4; sn += snprintf(spec + sn, sizeof spec - (size_t)sn, "%d", p); i++; }
            else while (U(i) >= '0' && U(i) <= '9' && sn < 58) spec[sn++] = (char)U(i++);
        }
        int lng = 0, sh = 0, ld = 0;                       /* l count, h count, L */
        for (;;) {
            uint32_t m = U(i);
            if (m == 'l') { lng++; i++; } else if (m == 'h') { sh++; i++; } else if (m == 'L') { ld = 1; i++; }
            else if (m == 'q') { lng = 2; i++; } else if (m == 'j') { lng = 2; i++; } else if (m == 'z' || m == 't') { i++; }
            else break;
        }
        uint32_t conv = U(i); if (!conv) break; i++;
        char buf[512]; int n = 0;
        switch (conv) {
        case 'd': case 'i': case 'u': case 'x': case 'X': case 'o': {
            spec[sn++] = 'l'; spec[sn++] = 'l'; spec[sn++] = (char)conv; spec[sn] = 0;
            long long v;
            if (lng >= 2) { v = (long long)rt_r64(M, ap); ap += 8; }
            else {
                uint32_t r = rt_r32(M, ap); ap += 4;
                int sgn = conv == 'd' || conv == 'i';
                if (sh == 1) v = sgn ? (long long)(int16_t)r : (long long)(uint16_t)r;
                else if (sh >= 2) v = sgn ? (long long)(int8_t)r : (long long)(uint8_t)r;
                else v = sgn ? (long long)(int32_t)r : (long long)r;
            }
            n = snprintf(buf, sizeof buf, spec, v); out_bytes(&o, buf, n < (int)sizeof buf ? (size_t)n : sizeof buf - 1); break;
        }
        case 'f': case 'F': case 'e': case 'E': case 'g': case 'G': case 'a': case 'A': {
            spec[sn++] = (char)conv; spec[sn] = 0;
            double v;
            if (ld) { v = rt_f80_load(M, ap); ap += 16; } else { v = rt_rf64(M, ap); ap += 8; }
            n = snprintf(buf, sizeof buf, spec, v); out_bytes(&o, buf, n < (int)sizeof buf ? (size_t)n : sizeof buf - 1); break;
        }
        case 'c': case 'C': {
            uint32_t v = rt_r32(M, ap); ap += 4;
            if (conv == 'C' || lng || wide) { if (conv == 'c' && !lng && wide) v &= 0xff; }
            else v &= 0xff;
            /* width handling: pad with spaces */
            spec[sn] = 0; int w = atoi(spec + 1 + (spec[1] == '-')); int left = strchr(spec, '-') != 0;
            if (!left) for (int k = 1; k < w; k++) out_unit(&o, ' ');
            out_unit(&o, v);
            if (left) for (int k = 1; k < w; k++) out_unit(&o, ' ');
            break;
        }
        case 's': case 'S': {
            uint32_t p = rt_r32(M, ap); ap += 4;
            int wstr = conv == 'S' || lng;                 /* %ls / %S: wchar_t string (in both families) */
            spec[sn] = 0;
            int prec = -1; char *dot = strchr(spec, '.'); if (dot) prec = atoi(dot + 1);
            int w = atoi(spec + 1 + strspn(spec + 1, "-+ #0'")); int left = strchr(spec, '-') != 0;
            uint32_t tmp[1]; (void)tmp;
            size_t L = 0;
            if (!p) { const char *nul = "(null)"; uint32_t base = 0; (void)base;
                size_t nl = strlen(nul); if (prec >= 0 && (size_t)prec < nl) nl = (size_t)prec;
                if (!left) for (size_t k = nl; (int)k < w; k++) out_unit(&o, ' ');
                out_bytes(&o, nul, nl);
                if (left) for (size_t k = nl; (int)k < w; k++) out_unit(&o, ' ');
                break; }
            if (wstr) { while (rt_r32(M, p + 4u * L) && (prec < 0 || (int)L < prec)) L++; }
            else { while (M[p + L] && (prec < 0 || (int)L < prec)) L++; }
            if (!left) for (size_t k = L; (int)k < w; k++) out_unit(&o, ' ');
            for (size_t k = 0; k < L; k++) out_unit(&o, wstr ? rt_r32(M, p + 4u * k) : M[p + k]);
            if (left) for (size_t k = L; (int)k < w; k++) out_unit(&o, ' ');
            break;
        }
        case 'p': { uint32_t v = rt_r32(M, ap); ap += 4; n = snprintf(buf, sizeof buf, "0x%x", v); out_bytes(&o, buf, (size_t)n); break; }
        case 'n': { uint32_t p = rt_r32(M, ap); ap += 4; if (p) rt_w32(G_MEM, p, (uint32_t)o.n); break; }
        default: out_unit(&o, conv); break;
        }
    }
#undef U
    out_unit(&o, 0); *len = o.n - 1;
    return o.u;
}

/* narrow result -> guest buffer (snprintf semantics); returns the full length */
static uint32_t put_narrow(uint32_t dst, uint32_t cap, const uint32_t *u, size_t n)
{
    if (dst && cap) {
        size_t m = n < cap - 1 ? n : cap - 1;
        for (size_t k = 0; k < m; k++) G_MEM[dst + k] = (uint8_t)u[k];
        G_MEM[dst + m] = 0;
    }
    return (uint32_t)n;
}
static int32_t put_wide(uint32_t dst, uint32_t cap, const uint32_t *u, size_t n)
{
    if (!cap) return -1;
    size_t m = n < cap - 1 ? n : cap - 1;
    for (size_t k = 0; k < m; k++) rt_w32(G_MEM, dst + 4u * k, u[k]);
    rt_w32(G_MEM, dst + 4u * m, 0);
    return n < cap ? (int32_t)n : -1;                 /* swprintf: -1 when it does not fit */
}
char *g_format(uint32_t fmt, uint32_t ap)               /* host string (UTF-8 for wide characters) */
{
    size_t n; uint32_t *u = format(fmt, 0, ap, &n);
    char *s = malloc(n * 4 + 1); size_t j = 0;
    for (size_t k = 0; k < n; k++) {
        uint32_t v = u[k];
        if (v < 0x100) s[j++] = (char)v;           /* bytes pass through (the game's strings are UTF-8 or Latin-1) */
        else if (v < 0x800) { s[j++] = (char)(0xc0 | v >> 6); s[j++] = (char)(0x80 | (v & 63)); }
        else if (v < 0x10000) { s[j++] = (char)(0xe0 | v >> 12); s[j++] = (char)(0x80 | ((v >> 6) & 63)); s[j++] = (char)(0x80 | (v & 63)); }
        else { s[j++] = (char)(0xf0 | v >> 18); s[j++] = (char)(0x80 | ((v >> 12) & 63)); s[j++] = (char)(0x80 | ((v >> 6) & 63)); s[j++] = (char)(0x80 | (v & 63)); }
    }
    s[j] = 0; free(u); return s;
}

IMP(_sprintf)   { size_t n; uint32_t *u = format(ARG(1), 0, c->esp + 12, &n); uint32_t r = put_narrow(ARG(0), 0xffffffffu, u, n); free(u); RET(r); }
IMP(_snprintf)  { size_t n; uint32_t *u = format(ARG(2), 0, c->esp + 16, &n); uint32_t r = put_narrow(ARG(0), ARG(1), u, n); free(u); RET(r); }
IMP(_vsprintf)  { size_t n; uint32_t *u = format(ARG(1), 0, ARG(2), &n); uint32_t r = put_narrow(ARG(0), 0xffffffffu, u, n); free(u); RET(r); }
IMP(_vsnprintf) { size_t n; uint32_t *u = format(ARG(2), 0, ARG(3), &n); uint32_t r = put_narrow(ARG(0), ARG(1), u, n); free(u); RET(r); }
/* _chk variants: (dst, flag, dstlen, fmt, ...) and (dst, maxlen, flag, dstlen, fmt, ...) */
IMP(___sprintf_chk)   { size_t n; uint32_t *u = format(ARG(3), 0, c->esp + 20, &n); uint32_t r = put_narrow(ARG(0), ARG(2), u, n); free(u); RET(r); }
IMP(___snprintf_chk)  { size_t n; uint32_t *u = format(ARG(4), 0, c->esp + 24, &n); uint32_t r = put_narrow(ARG(0), ARG(1), u, n); free(u); RET(r); }
IMP(___vsprintf_chk)  { size_t n; uint32_t *u = format(ARG(3), 0, ARG(4), &n); uint32_t r = put_narrow(ARG(0), ARG(2), u, n); free(u); RET(r); }
IMP(___vsnprintf_chk) { size_t n; uint32_t *u = format(ARG(4), 0, ARG(5), &n); uint32_t r = put_narrow(ARG(0), ARG(1), u, n); free(u); RET(r); }
IMP(_swprintf)  { size_t n; uint32_t *u = format(ARG(2), 1, c->esp + 16, &n); int32_t r = put_wide(ARG(0), ARG(1), u, n); free(u); RET(r); }
IMP(_vswprintf) { size_t n; uint32_t *u = format(ARG(2), 1, ARG(3), &n); int32_t r = put_wide(ARG(0), ARG(1), u, n); free(u); RET(r); }

void sys_write_stream(uint32_t file, const char *s, size_t n);   /* libsystem.c */
IMP(_printf)  { char *s = g_format(ARG(0), c->esp + 8); size_t n = strlen(s); sys_write_stream(0, s, n); free(s); RET(n); }
IMP(_fprintf) { char *s = g_format(ARG(1), c->esp + 12); size_t n = strlen(s); sys_write_stream(ARG(0), s, n); free(s); RET(n); }

/* ---- scanf ---- */
typedef struct In { uint32_t base; int wide; size_t pos; } In;
static uint32_t in_peek(In *in) { return in->wide ? rt_r32(G_MEM, in->base + 4u * in->pos) : G_MEM[in->base + in->pos]; }
static int is_space(uint32_t v) { return v == ' ' || (v >= 9 && v <= 13); }
static int scan(In *in, uint32_t fmt, int wide, uint32_t ap)
{
    const uint8_t *M = G_MEM;
#define F(k) (wide ? rt_r32(M, fmt + 4u * (k)) : (uint32_t)M[fmt + (k)])
    size_t i = 0; int count = 0;
    if (!in_peek(in)) return -1;
    for (;;) {
        uint32_t ch = F(i);
        if (!ch) break;
        if (is_space(ch)) { while (is_space(in_peek(in))) in->pos++; i++; continue; }
        if (ch != '%') { if (in_peek(in) != ch) break; in->pos++; i++; continue; }
        i++;
        if (F(i) == '%') { if (in_peek(in) != '%') break; in->pos++; i++; continue; }
        int skip = 0; if (F(i) == '*') { skip = 1; i++; }
        int width = 0; while (F(i) >= '0' && F(i) <= '9') width = width * 10 + (int)(F(i++) - '0');
        int lng = 0, sh = 0, ld = 0;
        for (;;) { uint32_t m = F(i); if (m == 'l') { lng++; i++; } else if (m == 'h') { sh++; i++; } else if (m == 'L' || m == 'q') { ld = 1; lng = 2; i++; } else break; }
        uint32_t conv = F(i++);
        if (conv != 'c' && conv != '[' && conv != 'n') while (is_space(in_peek(in))) in->pos++;
        if (conv == 'n') { if (!skip) { rt_w32(G_MEM, rt_r32(M, ap), (uint32_t)in->pos); ap += 4; } continue; }
        if (!in_peek(in)) { if (!count) count = -1; break; }
        char tok[512]; int tn = 0; int maxw = width ? width : (int)sizeof tok - 1;
        if (conv == 'd' || conv == 'i' || conv == 'u' || conv == 'x' || conv == 'X' || conv == 'o') {
            int base = conv == 'x' || conv == 'X' ? 16 : conv == 'o' ? 8 : conv == 'i' ? 0 : 10;
            while (tn < maxw) {
                uint32_t v = in_peek(in);
                int ok = (v == '-' || v == '+') ? tn == 0 : (base == 16 || base == 0) ? isxdigit((int)v) || ((v == 'x' || v == 'X') && tn > 0) : isdigit((int)v);
                if (!ok || v > 127) break;
                tok[tn++] = (char)v; in->pos++;
            }
            tok[tn] = 0; if (!tn || (tn == 1 && (tok[0] == '-' || tok[0] == '+'))) break;
            if (!skip) {
                uint32_t dst = rt_r32(M, ap); ap += 4;
                long long v = (conv == 'd' || conv == 'i') ? strtoll(tok, 0, base) : (long long)strtoull(tok, 0, base);
                if (lng >= 2) rt_w64(G_MEM, dst, (uint64_t)v);
                else if (sh == 1) rt_w16(G_MEM, dst, (uint32_t)v);
                else if (sh >= 2) rt_w8(G_MEM, dst, (uint32_t)v);
                else rt_w32(G_MEM, dst, (uint32_t)v);
                count++;
            }
        } else if (strchr("fFeEgGaA", (int)conv)) {
            while (tn < maxw) {
                uint32_t v = in_peek(in);
                if (v > 127 || !(isdigit((int)v) || v == '.' || v == 'e' || v == 'E' || v == '-' || v == '+' || v == 'x' || v == 'X' ||
                                 (isxdigit((int)v) && strchr(tok, 'x')) || v == 'i' || v == 'n' || v == 'f' || v == 'a' || v == 'N' || v == 'I' || v == 'F' || v == 'A')) break;
                if ((v == '-' || v == '+') && tn && tok[tn - 1] != 'e' && tok[tn - 1] != 'E') break;
                tok[tn++] = (char)v; tok[tn] = 0; in->pos++;
            }
            tok[tn] = 0; char *e; double d = strtod(tok, &e);
            if (e == tok) break;
            in->pos -= (size_t)(tn - (e - tok));
            if (!skip) {
                uint32_t dst = rt_r32(M, ap); ap += 4;
                if (ld) rt_f80_store(G_MEM, dst, d); else if (lng) rt_wf64(G_MEM, dst, d); else rt_wf32(G_MEM, dst, (float)d);
                count++;
            }
        } else if (conv == 's' || conv == 'S') {
            int wdst = wide ? !(sh) : (lng || conv == 'S');  /* %s in swscanf stores wchar_t */
            uint32_t dst = skip ? 0 : rt_r32(M, ap); if (!skip) ap += 4;
            int k = 0;
            while (in_peek(in) && !is_space(in_peek(in)) && (!width || k < width)) {
                uint32_t v = in_peek(in); in->pos++;
                if (dst) { if (wdst) rt_w32(G_MEM, dst + 4u * (uint32_t)k, v); else G_MEM[dst + (uint32_t)k] = (uint8_t)v; }
                k++;
            }
            if (!k) break;
            if (dst) { if (wdst) rt_w32(G_MEM, dst + 4u * (uint32_t)k, 0); else G_MEM[dst + (uint32_t)k] = 0; count++; }
        } else if (conv == 'c') {
            int n = width ? width : 1; int wdst = wide ? !sh : lng;
            uint32_t dst = skip ? 0 : rt_r32(M, ap); if (!skip) ap += 4;
            for (int k = 0; k < n; k++) {
                uint32_t v = in_peek(in); if (!v) break; in->pos++;
                if (dst) { if (wdst) rt_w32(G_MEM, dst + 4u * (uint32_t)k, v); else G_MEM[dst + (uint32_t)k] = (uint8_t)v; }
            }
            if (dst) count++;
        } else if (conv == '[') {
            int neg = 0; if (F(i) == '^') { neg = 1; i++; }
            uint32_t set[256]; int ns = 0;
            if (F(i) == ']') set[ns++] = F(i++);
            while (F(i) && F(i) != ']' && ns < 250) set[ns++] = F(i++);
            if (F(i) == ']') i++;
            int wdst = wide ? !sh : lng;
            uint32_t dst = skip ? 0 : rt_r32(M, ap); if (!skip) ap += 4;
            int k = 0;
            for (;;) {
                uint32_t v = in_peek(in); if (!v || (width && k >= width)) break;
                int in_set = 0;
                for (int s = 0; s < ns; s++) {
                    if (s + 2 < ns && set[s + 1] == '-') { if (v >= set[s] && v <= set[s + 2]) in_set = 1; s += 2; }
                    else if (v == set[s]) in_set = 1;
                }
                if (in_set == neg) break;
                if (dst) { if (wdst) rt_w32(G_MEM, dst + 4u * (uint32_t)k, v); else G_MEM[dst + (uint32_t)k] = (uint8_t)v; }
                k++; in->pos++;
            }
            if (!k) break;
            if (dst) { if (wdst) rt_w32(G_MEM, dst + 4u * (uint32_t)k, 0); else G_MEM[dst + (uint32_t)k] = 0; count++; }
        } else if (conv == 'p') {
            while (tn < maxw && in_peek(in) < 128 && (isxdigit((int)in_peek(in)) || in_peek(in) == 'x')) { tok[tn++] = (char)in_peek(in); in->pos++; }
            tok[tn] = 0; if (!skip) { rt_w32(G_MEM, rt_r32(M, ap), (uint32_t)strtoul(tok, 0, 16)); ap += 4; count++; }
        } else break;
    }
#undef F
    return count;
}
IMP(_sscanf)   { In in = {ARG(0), 0, 0}; RET(scan(&in, ARG(1), 0, c->esp + 12)); }
IMP(_vsscanf)  { In in = {ARG(0), 0, 0}; RET(scan(&in, ARG(1), 0, ARG(2))); }
IMP(_swscanf)  { In in = {ARG(0), 1, 0}; RET(scan(&in, ARG(1), 1, c->esp + 12)); }
IMP(_vswscanf) { In in = {ARG(0), 1, 0}; RET(scan(&in, ARG(1), 1, ARG(2))); }
int sys_scan_file(uint32_t file, uint32_t fmt, uint32_t ap);    /* libsystem.c: reads a line, scans it */
int g_scan_string(const char *s, uint32_t fmt, uint32_t ap)
{
    uint32_t n = (uint32_t)strlen(s) + 1, b = g_malloc(n); memcpy(G_MEM + b, s, n);
    In in = {b, 0, 0}; int r = scan(&in, fmt, 0, ap); g_free(b); return r;
}
