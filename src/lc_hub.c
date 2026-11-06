/* lc_hub.c - LocalConnection registry and message queue shared by AVM1 and
 * AVM2 (Ruffle's LocalConnections): connection names are keyed by
 * "superdomain:name" (or "_name"), lower case; messages sent during a
 * frame are delivered once per frame, each one first reporting "status"
 * (or "error" when nobody listens) to its sender. */
#include "lc_hub.h"
#include <stdlib.h>
#include <string.h>

typedef struct { char *key; int vmk; void *obj; } LCEntry;
typedef struct { int vmk; void *src; char *conn; char *method; int ok, nargs; u8 **args; size_t *argn; } LCQ;

static LCEntry *g_e; static int g_ne;
static LCQ *g_q; static int g_nq;
static LCHandler g_h[2];

static char *dupz(const char *s) { size_t l = strlen(s); char *d = (char*)malloc(l + 1); if (d) memcpy(d, s, l + 1); return d; }
static void lower(char *s) { for (; *s; s++) if (*s >= 'A' && *s <= 'Z') *s = (char)(*s + 32); }

void lch_set_handler(int vmk, const LCHandler *h) { if (vmk == 0 || vmk == 1) g_h[vmk] = *h; }

char *lch_key(const char *superdomain, const char *name, int for_send) {
    char *k;
    if (name[0] == '_' || (for_send && strchr(name, ':'))) k = dupz(name);
    else { k = (char*)malloc(strlen(superdomain) + strlen(name) + 2); if (k) { strcpy(k, superdomain); strcat(k, ":"); strcat(k, name); } }
    if (k) lower(k);
    return k;
}

void lch_superdomain(const char *url, char *out, size_t n) {
    const char *p = url ? strstr(url, "://") : NULL, *q, *dot; char dom[256]; size_t l;
    if (!p || !strncmp(url, "file:", 5)) strcpy(dom, "localhost");
    else {
        p += 3; q = p; while (*q && *q != '/' && *q != ':' && *q != '?' && *q != '#') q++;
        l = (size_t)(q - p); if (l >= sizeof(dom)) l = sizeof(dom) - 1; memcpy(dom, p, l); dom[l] = 0;
        if (!l) strcpy(dom, "localhost");
    }
    dot = strrchr(dom, '.');
    strncpy(out, dot ? dot + 1 : dom, n - 1); out[n - 1] = 0;
}

static LCEntry *find(const char *key) { int i; for (i = 0; i < g_ne; i++) if (!strcmp(g_e[i].key, key)) return &g_e[i]; return NULL; }

int lch_connect(const char *key, int vmk, void *obj) {
    LCEntry *n;
    if (find(key)) return 0;
    n = (LCEntry*)realloc(g_e, (size_t)(g_ne + 1) * sizeof(LCEntry));
    if (!n) return 0;
    g_e = n; g_e[g_ne].key = dupz(key); g_e[g_ne].vmk = vmk; g_e[g_ne].obj = obj; g_ne++;
    return 1;
}
void lch_close(const char *key) {
    int i;
    for (i = 0; i < g_ne; i++) if (!strcmp(g_e[i].key, key)) { free(g_e[i].key); memmove(&g_e[i], &g_e[i + 1], (size_t)(g_ne - i - 1) * sizeof(LCEntry)); g_ne--; return; }
}

void lch_send(int vmk, void *src, const char *conn_key, const char *method, int nargs, u8 **args, size_t *argn) {
    LCQ *n = (LCQ*)realloc(g_q, (size_t)(g_nq + 1) * sizeof(LCQ)), *m;
    int i;
    if (!n) { for (i = 0; i < nargs; i++) free(args[i]); free(args); free(argn); return; }
    g_q = n; m = &g_q[g_nq++];
    m->vmk = vmk; m->src = src; m->conn = dupz(conn_key); m->method = dupz(method);
    m->ok = find(conn_key) != NULL; m->nargs = nargs; m->args = args; m->argn = argn;
}

void lch_deliver(void) {
    LCQ *list = g_q; int n = g_nq, k, i;
    if (!n) return;
    g_q = NULL; g_nq = 0;
    for (k = 0; k < n; k++) {
        LCQ *m = &list[k]; LCEntry *r = m->ok ? find(m->conn) : NULL;
        if (!r) { if (g_h[m->vmk].status) g_h[m->vmk].status(g_h[m->vmk].ctx, m->src, "error"); }
        else {
            int rk = r->vmk; void *ro = r->obj;
            if (g_h[m->vmk].status) g_h[m->vmk].status(g_h[m->vmk].ctx, m->src, "status");
            if (g_h[rk].call) g_h[rk].call(g_h[rk].ctx, ro, m->method, m->nargs, m->args, m->argn);
        }
        for (i = 0; i < m->nargs; i++) free(m->args[i]);
        free(m->args); free(m->argn); free(m->conn); free(m->method);
    }
    free(list);
}

void lch_mark(int vmk, void (*mark)(void *ctx, void *obj), void *ctx) {
    int i;
    for (i = 0; i < g_ne; i++) if (g_e[i].vmk == vmk) mark(ctx, g_e[i].obj);
    for (i = 0; i < g_nq; i++) if (g_q[i].vmk == vmk) mark(ctx, g_q[i].src);
}

void lch_forget_vm(int vmk) {
    int i, j, k;
    for (i = j = 0; i < g_ne; i++) { if (g_e[i].vmk == vmk) free(g_e[i].key); else g_e[j++] = g_e[i]; }
    g_ne = j;
    for (i = j = 0; i < g_nq; i++) {
        if (g_q[i].vmk == vmk) { for (k = 0; k < g_q[i].nargs; k++) free(g_q[i].args[k]); free(g_q[i].args); free(g_q[i].argn); free(g_q[i].conn); free(g_q[i].method); }
        else g_q[j++] = g_q[i];
    }
    g_nq = j;
    memset(&g_h[vmk], 0, sizeof(g_h[vmk]));
}
