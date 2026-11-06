/* sock.c - socket connections behind an event queue (see sock.h).
 *
 * With the test navigator (fetch.c g_test_navigator) a connection plays
 * the "socket.json" script next to the movie, the way Ruffle's test
 * harness does; otherwise it is a real TCP connection of the platform. */
#include "sock.h"
#include "fetch.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern int g_test_navigator, g_fetch_log;
extern void avm1_trace_out(const char *s);

enum { SE_SEND = 1, SE_RECEIVE, SE_DISCONNECT, SE_WAIT_DISCONNECT };
typedef struct { int type; u8 *d; size_t n; } ScriptEv;

typedef struct Conn {
    int used, handle, closed_by_client, done;
    int scripted; ScriptEv *ev; int nev, idx, started; int pending_sends;
    int fd, connecting;
} Conn;

typedef struct SockState {
    Conn *c; int nc;
    SockEvent *q; int nq, capq;
    int next;
} SockState;

static SockState *st_of(Player *p) {
    static SockState *g;    /* one player per process */
    (void)p;
    if (!g) g = (SockState*)calloc(1, sizeof(SockState));
    return g;
}

static void push_ev(SockState *s, int type, int h, int ok, const u8 *d, size_t n) {
    SockEvent *e;
    if (s->nq == s->capq) { int nc = s->capq ? s->capq * 2 : 16; SockEvent *nq = (SockEvent*)realloc(s->q, (size_t)nc * sizeof(SockEvent)); if (!nq) return; s->q = nq; s->capq = nc; }
    e = &s->q[s->nq++];
    e->type = type; e->handle = h; e->ok = ok; e->data = NULL; e->n = n;
    if (d && n) { e->data = (u8*)malloc(n); if (e->data) memcpy(e->data, d, n); else e->n = 0; }
}

/* ---- socket.json (an array of {"type": ..., "payload"/"expected": [bytes]}) ---- */

static void load_script(Player *p, Conn *c) {
    char path[1100]; const char *mp = p->movie ? p->movie->path : NULL; size_t pl, n = 0; u8 *d; char *s, *e;
    if (!mp) return;
    pl = strlen(mp); while (pl > 0 && mp[pl - 1] != '/' && mp[pl - 1] != '\\') pl--;
    if (pl + 16 >= sizeof(path)) return;
    memcpy(path, mp, pl); strcpy(path + pl, "socket.json");
    d = fetch_read_file(path, &n);
    if (!d) return;
    d[n] = 0;
    c->scripted = 1;
    s = (char*)d;
    while ((s = strstr(s, "\"type\"")) != NULL) {
        ScriptEv ev; char *q; ScriptEv *ne;
        memset(&ev, 0, sizeof(ev));
        s += 6; q = strchr(s, '"'); if (!q) break; q++;
        if (!strncmp(q, "Send\"", 5)) ev.type = SE_SEND;
        else if (!strncmp(q, "Receive\"", 8)) ev.type = SE_RECEIVE;
        else if (!strncmp(q, "Disconnect\"", 11)) ev.type = SE_DISCONNECT;
        else if (!strncmp(q, "WaitForDisconnect\"", 18)) ev.type = SE_WAIT_DISCONNECT;
        e = strchr(q, '}');
        if (ev.type == SE_SEND) {
            char *b = strchr(q, '['); size_t cap = 64;
            ev.d = (u8*)malloc(cap);
            if (b && (!e || b < e)) {
                b++;
                while (*b && *b != ']') {
                    while (*b == ' ' || *b == ',' || *b == '\n' || *b == '\r' || *b == '\t') b++;
                    if (*b >= '0' && *b <= '9') { if (ev.n == cap) { cap *= 2; ev.d = (u8*)realloc(ev.d, cap); } ev.d[ev.n++] = (u8)strtol(b, &b, 10); }
                    else if (*b != ']') b++;
                }
            }
        }
        ne = (ScriptEv*)realloc(c->ev, (size_t)(c->nev + 1) * sizeof(ScriptEv));
        if (!ne) break;
        c->ev = ne; c->ev[c->nev++] = ev;
        s = q;
    }
    free(d);
}

static void run_script(SockState *s, Conn *c) {
    if (!c->started) { c->started = 1; push_ev(s, SOCK_EV_CONNECT, c->handle, 1, NULL, 0); }
    while (!c->done && c->idx < c->nev) {
        ScriptEv *e = &c->ev[c->idx];
        if (e->type == SE_SEND) { push_ev(s, SOCK_EV_DATA, c->handle, 1, e->d, e->n); c->idx++; }
        else if (e->type == SE_DISCONNECT) { push_ev(s, SOCK_EV_CLOSE, c->handle, 1, NULL, 0); c->idx++; c->done = 1; }
        else if (e->type == SE_WAIT_DISCONNECT) { if (c->closed_by_client) c->done = 1; break; }
        else { if (c->pending_sends > 0) { c->pending_sends--; c->idx++; } else { if (c->closed_by_client) c->done = 1; break; } }
    }
    if (c->idx >= c->nev) c->done = 1;
}

/* ---- API ---- */

int sock_connect(Player *p, const char *host, int port, int timeout_ms) {
    SockState *s = st_of(p); Conn *c = NULL; int i;
    (void)timeout_ms;
    if (!s) return 0;
    if (g_fetch_log) {
        char line[600];
        avm1_trace_out("Navigator::connect_socket");
        sprintf(line, "    Host: %.500s; Port: %d", host, port);
        avm1_trace_out(line);
    }
    for (i = 0; i < s->nc; i++) if (!s->c[i].used) { c = &s->c[i]; break; }
    if (!c) { Conn *nc = (Conn*)realloc(s->c, (size_t)(s->nc + 1) * sizeof(Conn)); if (!nc) return 0; s->c = nc; c = &s->c[s->nc++]; }
    memset(c, 0, sizeof(Conn));
    c->used = 1; c->handle = ++s->next; c->fd = -1;
    if (g_test_navigator) { load_script(p, c); if (!c->scripted) c->done = 1; }
    else {
        c->fd = plat_tcp_connect(host, port);
        if (c->fd < 0) { push_ev(s, SOCK_EV_CONNECT, c->handle, 0, NULL, 0); c->done = 1; }
        else c->connecting = 1;
    }
    return c->handle;
}

static Conn *find(SockState *s, int h) { int i; for (i = 0; s && i < s->nc; i++) if (s->c[i].used && s->c[i].handle == h) return &s->c[i]; return NULL; }

void sock_send(Player *p, int h, const u8 *d, size_t n) {
    Conn *c = find(st_of(p), h);
    if (!c || c->closed_by_client) return;
    if (c->scripted) c->pending_sends++;
    else if (c->fd >= 0 && !c->connecting) plat_tcp_send(c->fd, d, n);
}

static void conn_free(Conn *c) {
    int i;
    for (i = 0; i < c->nev; i++) free(c->ev[i].d);
    free(c->ev);
    if (c->fd >= 0) plat_tcp_close(c->fd);
    memset(c, 0, sizeof(Conn));
}

void sock_close(Player *p, int h) {
    SockState *s = st_of(p); Conn *c = find(s, h); int i, j;
    if (!c) return;
    c->closed_by_client = 1;
    if (c->scripted && !c->done) run_script(s, c);
    /* events of a connection the script dropped are never delivered */
    for (i = j = 0; i < s->nq; i++) { if (s->q[i].handle == h) free(s->q[i].data); else s->q[j++] = s->q[i]; }
    s->nq = j;
    conn_free(c);
}

void sock_pump(Player *p) {
    SockState *s = st_of(p); int i;
    if (!s) return;
    for (i = 0; i < s->nc; i++) {
        Conn *c = &s->c[i];
        if (!c->used || c->done) continue;
        if (c->scripted) { run_script(s, c); continue; }
        if (c->fd >= 0) {
            int conn = 0, rd = 0;
            if (plat_tcp_poll(c->fd, &conn, &rd) < 0) {
                push_ev(s, c->connecting ? SOCK_EV_CONNECT : SOCK_EV_CLOSE, c->handle, 0, NULL, 0);
                c->done = 1; continue;
            }
            if (c->connecting && conn) { c->connecting = 0; push_ev(s, SOCK_EV_CONNECT, c->handle, 1, NULL, 0); }
            while (!c->connecting && rd) {
                u8 buf[4096]; int k = plat_tcp_recv(c->fd, buf, sizeof(buf));
                if (k > 0) push_ev(s, SOCK_EV_DATA, c->handle, 1, buf, (size_t)k);
                else if (k == 0) { push_ev(s, SOCK_EV_CLOSE, c->handle, 1, NULL, 0); c->done = 1; break; }
                else break;
            }
        }
    }
}

int sock_next_event(Player *p, SockEvent *ev) {
    SockState *s = st_of(p); int i;
    if (!s || !s->nq) return 0;
    *ev = s->q[0];
    for (i = 1; i < s->nq; i++) s->q[i - 1] = s->q[i];
    s->nq--;
    return 1;
}

void sock_free_all(Player *p) {
    SockState *s = st_of(p); int i;
    if (!s) return;
    for (i = 0; i < s->nc; i++) if (s->c[i].used) conn_free(&s->c[i]);
    for (i = 0; i < s->nq; i++) free(s->q[i].data);
    free(s->c); free(s->q);
    memset(s, 0, sizeof(SockState));
}
