/* fetch.c - resolving and reading the URLs movies load (local files). */
#include "fetch.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const char *fetch_movie_url(SWFMovie *m) { return m && m->url ? m->url : "file:///movie.swf"; }

/* rel against base (only the cases SWFs use: absolute, root-relative, relative with ./ and ../) */
void fetch_url_resolve(const char *base, const char *rel, char *out, size_t n) {
    size_t bl; const char *q; char nb[1100];
    if (!rel) rel = "";
    if (base && (q = strstr(base, "/[[DYNAMIC]]/")) != NULL && (size_t)(q - base) < sizeof(nb)) {   /* loadBytes content: relative to the SWF that loaded it */
        memcpy(nb, base, (size_t)(q - base)); nb[q - base] = 0; base = nb;
    }
    if (strstr(rel, "://") || !strncmp(rel, "file:", 5) || !strncmp(rel, "data:", 5)) { strncpy(out, rel, n - 1); out[n - 1] = 0; return; }
    if (rel[0] == '/' || rel[0] == '\\') {
        const char *s = strstr(base, "://"); size_t hl = s ? (size_t)(s - base) + 3 : 0;
        if (s && strncmp(base, "file:", 5)) { const char *sl = strchr(s + 3, '/'); hl = sl ? (size_t)(sl - base) : strlen(base); }
        if (hl >= n) hl = n - 1;
        memcpy(out, base, hl); out[hl] = 0;
        strncat(out, rel, n - hl - 1);
        return;
    }
    q = strchr(base, '?'); bl = q ? (size_t)(q - base) : strlen(base);
    while (bl > 0 && base[bl - 1] != '/' && base[bl - 1] != '\\') bl--;
    if (bl >= n) bl = n - 1;
    memcpy(out, base, bl); out[bl] = 0;
    while (!strncmp(rel, "./", 2)) rel += 2;
    while (!strncmp(rel, "../", 3)) {
        size_t l = strlen(out);
        if (l > 8 && out[l - 1] == '/') { l--; while (l > 8 && out[l - 1] != '/') l--; out[l] = 0; }
        rel += 3;
    }
    strncat(out, rel, n - strlen(out) - 1);
}
/* file name for a file: URL, through the main movie's url -> path mapping */
int g_test_navigator = 0;   /* test harness: http://host/path is read from <movie dir>/host/path */

int fetch_url_to_path(Player *p, const char *url, char *out, size_t n) {
    const char *mu = fetch_movie_url(p->movie), *mp = p->movie->path; size_t ul, pl, i, o = 0; char tmp[1024];
    if (g_test_navigator && mp && (!strncmp(url, "http://", 7) || !strncmp(url, "https://", 8))) {
        const char *h = strstr(url, "://") + 3, *e = h; size_t hl;
        while (*e && *e != '/' && *e != ':' && *e != '?' && *e != '#') e++;
        hl = (size_t)(e - h);
        while (*e && *e != '/' && *e != '?' && *e != '#') e++;   /* port */
        pl = strlen(mp); while (pl > 0 && mp[pl - 1] != '/' && mp[pl - 1] != '\\') pl--;
        if (pl + hl + strlen(e) + 2 >= sizeof(tmp)) return 0;
        memcpy(tmp, mp, pl); memcpy(tmp + pl, h, hl); tmp[pl + hl] = 0;
        strcat(tmp, *e == '/' ? e : "/");
        goto decode;
    }
    if (strncmp(url, "file:", 5)) return 0;
    ul = strlen(mu); while (ul > 0 && mu[ul - 1] != '/') ul--;
    if (mp) { pl = strlen(mp); while (pl > 0 && mp[pl - 1] != '/' && mp[pl - 1] != '\\') pl--; } else pl = 0;
    if (mp && ul && !strncmp(url, mu, ul)) { if (pl >= sizeof(tmp)) return 0; memcpy(tmp, mp, pl); tmp[pl] = 0; strncat(tmp, url + ul, sizeof(tmp) - pl - 1); }
    else {
        const char *s = url + 5; while (*s == '/') s++;
#ifdef _WIN32
        strncpy(tmp, s, sizeof(tmp) - 1);
#else
        tmp[0] = '/'; strncpy(tmp + 1, s, sizeof(tmp) - 2);
#endif
        tmp[sizeof(tmp) - 1] = 0;
    }
decode:
    for (i = 0; tmp[i] && tmp[i] != '?' && tmp[i] != '#' && o + 1 < n; i++) {
        if (tmp[i] == '%' && tmp[i + 1] && tmp[i + 2]) { char h[3]; h[0] = tmp[i + 1]; h[1] = tmp[i + 2]; h[2] = 0; out[o++] = (char)strtol(h, NULL, 16); i += 2; }
#ifdef _WIN32
        else if (tmp[i] == '/') out[o++] = '\\';
#endif
        else out[o++] = tmp[i];
    }
    out[o] = 0;
    return 1;
}
u8 *fetch_read_file(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb"); long n; u8 *b;
    if (!f) return NULL;
    fseek(f, 0, SEEK_END); n = ftell(f); fseek(f, 0, SEEK_SET);
    if (n < 0) { fclose(f); return NULL; }
    b = (u8*)malloc((size_t)n + 1);
    if (!b) { fclose(f); return NULL; }
    if (n && fread(b, 1, (size_t)n, f) != (size_t)n) { free(b); fclose(f); return NULL; }
    fclose(f); *len = (size_t)n; return b;
}


/* ---------------------------------------------------------------- */
/* local storage                                                    */

const char *g_store_dir = NULL;

typedef struct MemItem { char *key; u8 *d; size_t n; struct MemItem *next; } MemItem;
static MemItem *g_mem;

static MemItem *mem_find(const char *key) { MemItem *m; for (m = g_mem; m; m = m->next) if (!strcmp(m->key, key)) return m; return NULL; }

static int store_path(const char *key, char *out, size_t n, int make_dirs) {
    size_t i, o; const char *k;
    if (!g_store_dir || strlen(g_store_dir) + strlen(key) + 8 > n) return 0;
    strcpy(out, g_store_dir); o = strlen(out);
    out[o++] = '/';
    for (k = key; *k; k++) {
        char c = *k;
        if (c == '/') {
            out[o] = 0;
            if (make_dirs) plat_mkdir(out);
#ifdef _WIN32
            c = '\\';
#endif
        }
        /* characters Windows does not allow in file names */
        if (c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|') c = '_';
        out[o++] = c;
    }
    out[o] = 0;
    strcat(out, ".sol");
    (void)i;
    return 1;
}

int store_get(const char *key, u8 **out, size_t *n) {
    MemItem *m = mem_find(key); char path[1024];
    if (m) { *out = (u8*)malloc(m->n ? m->n : 1); if (!*out) return 0; memcpy(*out, m->d, m->n); *n = m->n; return 1; }
    if (store_path(key, path, sizeof(path), 0)) { *out = fetch_read_file(path, n); return *out != NULL; }
    return 0;
}

int store_put(const char *key, const u8 *d, size_t n) {
    MemItem *m = mem_find(key); char path[1024];
    if (!m) {
        m = (MemItem*)calloc(1, sizeof(MemItem));
        if (!m) return 0;
        m->key = (char*)malloc(strlen(key) + 1);
        if (!m->key) { free(m); return 0; }
        strcpy(m->key, key); m->next = g_mem; g_mem = m;
    }
    free(m->d); m->d = (u8*)malloc(n ? n : 1); m->n = n;
    if (!m->d) { m->n = 0; return 0; }
    memcpy(m->d, d, n);
    if (store_path(key, path, sizeof(path), 1)) {
        FILE *f = fopen(path, "wb");
        if (!f) return 0;
        if (n && fwrite(d, 1, n, f) != n) { fclose(f); return 0; }
        fclose(f);
    }
    return 1;
}

void store_remove(const char *key) {
    MemItem **pp = &g_mem, *m; char path[1024];
    while ((m = *pp) != NULL) { if (!strcmp(m->key, key)) { *pp = m->next; free(m->key); free(m->d); free(m); break; } pp = &m->next; }
    if (store_path(key, path, sizeof(path), 0)) remove(path);
}
