/* plat_host.c - headless host driver: load a SWF, advance frames, dump
 * PNGs.  Used to validate the engine on a modern machine; the NT 3.1
 * build uses plat_win32.c instead.  Pure C89, no deps (own PNG writer). */
void host_advance_clock(double ms);
int host_codepage(int dir, unsigned int cp, const void *src, int n, void *dst, int cap);
#include "player.h"
#include "devfont.h"
#include "avm1.h"
#include "audio.h"
#include <stdio.h>
#include <time.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

/* ---- minimal PNG writer (stored deflate blocks + crc32 + adler32) ---- */
static u32 crc_table[256];
static void crc_init(void) {
    u32 c; int n, k;
    for (n = 0; n < 256; n++) {
        c = (u32)n;
        for (k = 0; k < 8; k++) c = (c & 1) ? 0xedb88320u ^ (c >> 1) : c >> 1;
        crc_table[n] = c;
    }
}
static u32 crc32b(u32 crc, const u8 *buf, size_t len) {
    size_t i; crc ^= 0xffffffffu;
    for (i = 0; i < len; i++) crc = crc_table[(crc ^ buf[i]) & 0xff] ^ (crc >> 8);
    return crc ^ 0xffffffffu;
}
static void put32(FILE *f, u32 v) { fputc(v >> 24, f); fputc(v >> 16, f); fputc(v >> 8, f); fputc(v, f); }
static void chunk(FILE *f, const char *type, const u8 *data, size_t len) {
    u32 crc;
    put32(f, (u32)len);
    fwrite(type, 1, 4, f);
    if (len) fwrite(data, 1, len, f);
    crc = crc32b(0, (const u8*)type, 4);
    crc = crc32b(crc, data, len);
    put32(f, crc);
}

int write_png(const char *path, const Surface *s) {
    FILE *f = fopen(path, "wb");
    size_t rowlen = (size_t)s->w * 3 + 1, rawlen = rowlen * s->h, i;
    u8 *raw, *z; size_t zlen, zpos = 0, pos = 0;
    u32 a = 1, b = 0;
    u8 hdr[13];
    static int inited = 0;
    if (!f) return -1;
    if (!inited) { crc_init(); inited = 1; }
    raw = (u8*)malloc(rawlen);
    for (i = 0; i < (size_t)s->h; i++) {
        int x; u8 *row = raw + i * rowlen;
        row[0] = 0;
        for (x = 0; x < s->w; x++) {
            u32 p = s->px[i * s->w + x];
            row[1 + x * 3] = (u8)(p >> 16); row[2 + x * 3] = (u8)(p >> 8); row[3 + x * 3] = (u8)p;
        }
    }
    /* zlib stream of stored blocks */
    zlen = 2 + rawlen + (rawlen / 65535 + 1) * 5 + 4;
    z = (u8*)malloc(zlen);
    z[zpos++] = 0x78; z[zpos++] = 0x01;
    while (pos < rawlen) {
        size_t n = rawlen - pos; if (n > 65535) n = 65535;
        z[zpos++] = (pos + n == rawlen) ? 1 : 0;
        z[zpos++] = (u8)n; z[zpos++] = (u8)(n >> 8);
        z[zpos++] = (u8)~n; z[zpos++] = (u8)(~n >> 8);
        memcpy(z + zpos, raw + pos, n); zpos += n; pos += n;
    }
    for (i = 0; i < rawlen; i++) { a = (a + raw[i]) % 65521; b = (b + a) % 65521; }
    z[zpos++] = (u8)(b >> 8); z[zpos++] = (u8)b; z[zpos++] = (u8)(a >> 8); z[zpos++] = (u8)a;

    fwrite("\x89PNG\r\n\x1a\n", 1, 8, f);
    hdr[0] = (u8)(s->w >> 24); hdr[1] = (u8)(s->w >> 16); hdr[2] = (u8)(s->w >> 8); hdr[3] = (u8)s->w;
    hdr[4] = (u8)(s->h >> 24); hdr[5] = (u8)(s->h >> 16); hdr[6] = (u8)(s->h >> 8); hdr[7] = (u8)s->h;
    hdr[8] = 8; hdr[9] = 2; hdr[10] = 0; hdr[11] = 0; hdr[12] = 0;
    chunk(f, "IHDR", hdr, 13);
    chunk(f, "IDAT", z, zpos);
    chunk(f, "IEND", NULL, 0);
    fclose(f); free(raw); free(z);
    return 0;
}

#include <sys/stat.h>
#include <sys/resource.h>
#include <errno.h>
void plat_open_url(const char *url) { if (getenv("FLASHNT_DEBUG")) fprintf(stderr, "[open url] %s\n", url); }
int plat_mkdir(const char *path) { return mkdir(path, 0755) == 0 || errno == EEXIST; }

/* fscommand("captureImage", name): the Ruffle test harness image capture.
   Names are queued and written as <outdir>/capture_<name>.png after the tick. */
static char *g_cap_names[64]; static int g_ncap;
static void host_fscommand(Player *p, const char *cmd, const char *args) {
    (void)p;
    if (!strcmp(cmd, "captureImage") && g_ncap < 64) {
        g_cap_names[g_ncap] = (char*)malloc(strlen(args) + 1);
        if (g_cap_names[g_ncap]) strcpy(g_cap_names[g_ncap++], args);
    }
}
static void host_flush_captures(Player *p, const char *outdir) {
    int i; char path[600];
    for (i = 0; i < g_ncap; i++) {
        player_render(p);
        sprintf(path, "%s/capture_%.500s.png", outdir, g_cap_names[i]);
        write_png(path, p->surf);
        free(g_cap_names[i]);
    }
    g_ncap = 0;
}
/* -F list: frames to write (comma separated); every frame is rendered like the test harness does */
static int frame_listed(const char *list, int t) {
    const char *q = list;
    while (q && *q) { if (atoi(q) == t) return 1; q = strchr(q, ','); if (q) q++; }
    return 0;
}

static void usage(void) {
    fprintf(stderr,
      "flashnt-host <file.swf> [-n frames] [-z zoom] [-o outdir] [-e every] [-k keycode@tick]\n"
      "  renders frame 1..n to outdir/frame_NNNN.png (every `every`-th frame)\n");
}

/* in-memory clipboard for the test harness */
static char *g_host_clip;
extern int g_view_w, g_view_h; extern double g_view_sf;
extern void (*g_fscommand_hook)(Player *p, const char *cmd, const char *args);
int plat_set_fullscreen(Player *p, int on) { (void)p; (void)on; return 1; }
static void host_clip_set(const char *u) { free(g_host_clip); g_host_clip = (char*)malloc(strlen(u) + 1); if (g_host_clip) strcpy(g_host_clip, u); }
static void host_clip_set_escaped(const char *u) {   /* test input: \n \r \\ escapes */
    char *b = (char*)malloc(strlen(u) + 1); size_t k = 0;
    if (!b) return;
    while (*u) { if (*u == '\\' && u[1]) { u++; b[k++] = *u == 'n' ? '\n' : *u == 'r' ? '\r' : *u; u++; } else b[k++] = *u++; }
    b[k] = 0; host_clip_set(b); free(b);
}
static char *host_clip_get(void) { char *r; if (!g_host_clip) return NULL; r = (char*)malloc(strlen(g_host_clip) + 1); if (r) strcpy(r, g_host_clip); return r; }

#include "extval.h"
/* ExternalInterface test container (the Ruffle test harness provider) */
static void host_trace(const char *s) { extern void avm1_trace_out(const char *s); avm1_trace_out(s); }
static int host_ei_provider(Player *p, const char *name, int argc, const ExtVal *args, ExtVal *out) {
    if (!strcmp(name, "trace")) {
        char *buf; size_t cap = 64, n; int i;
        buf = (char*)malloc(cap); strcpy(buf, "[ExternalInterface] trace: [");
        for (i = 0; i < argc; i++) {
            char *d = xv_debug(&args[i]); n = strlen(buf) + strlen(d) + 8;
            if (n > cap) { cap = n * 2; buf = (char*)realloc(buf, cap); }
            if (i) strcat(buf, ", ");
            strcat(buf, d); free(d);
        }
        strcat(buf, "]"); host_trace(buf); free(buf);
        *out = xv_str("Traced!"); return 1;
    }
    if (!strcmp(name, "ping")) { host_trace("[ExternalInterface] ping"); *out = xv_str("Pong!"); return 1; }
    if (!strcmp(name, "reentry")) {
        ExtVal a[2];
        host_trace("[ExternalInterface] starting reentry");
        a[0] = xv_str("trace"); a[1] = xv_str("successful reentry!");
        player_call_internal(p, "callWith", 2, a, out);
        xv_free(&a[0]); xv_free(&a[1]);
        return 1;
    }
    *out = xv_null();
    return 1;
}
static void host_ei_after_first_tick(Player *p, const char *mode) {
    ExtVal a[2], r; char *d, line[4096];
    if (!strcmp(mode, "avm1")) {
        ExtVal root, nested, list;
        a[0] = xv_str("Hello World!");
        player_call_internal(p, "parrot", 1, a, &r); xv_free(&a[0]);
        d = xv_debug(&r); sprintf(line, "After calling `parrot` with a string: %.3900s", d); host_trace(line); free(d); xv_free(&r);
        list = xv_undef(); list.t = XV_LIST;
        xv_list_push(&list, xv_str("string")); xv_list_push(&list, xv_num(100)); xv_list_push(&list, xv_bool(0));
        { ExtVal e = xv_undef(); e.t = XV_OBJ; xv_list_push(&list, e); }
        nested = xv_undef(); nested.t = XV_OBJ; xv_obj_put(&nested, "list", list);
        root = xv_undef(); root.t = XV_OBJ;
        xv_obj_put(&root, "number", xv_num(-500.1)); xv_obj_put(&root, "string", xv_str("A string!"));
        xv_obj_put(&root, "true", xv_bool(1)); xv_obj_put(&root, "false", xv_bool(0));
        xv_obj_put(&root, "null", xv_null()); xv_obj_put(&root, "nested", nested);
        a[0] = xv_str("trace"); a[1] = root;
        player_call_internal(p, "callWith", 2, a, &r); xv_free(&a[0]); xv_free(&a[1]);
        d = xv_debug(&r); sprintf(line, "After calling `callWith` with a complex payload: %.3900s", d); host_trace(line); free(d); xv_free(&r);
    } else if (!strcmp(mode, "avm2")) {
        ExtVal map, list;
        a[0] = xv_str("Hello World!");
        player_call_internal(p, "parrot", 1, a, &r); xv_free(&a[0]);
        d = xv_debug(&r); sprintf(line, "After calling `parrot` with a string: %.3900s", d); host_trace(line); free(d); xv_free(&r);
        map = xv_undef(); map.t = XV_OBJ; xv_obj_put(&map, "a", xv_num(100)); xv_obj_put(&map, "b", xv_str("string"));
        player_call_internal(p, "parrot", 1, &map, &r); xv_free(&map);
        d = xv_debug(&r); sprintf(line, "After calling `parrot` with an object: %.3900s", d); host_trace(line); free(d); xv_free(&r);
        a[0] = xv_str("Hello World!");
        player_call_internal(p, "freestanding", 1, a, &r); xv_free(&a[0]); xv_free(&r);
        host_trace("Before calling `removeMe`");
        player_call_internal(p, "removeMe", 0, a, &r);
        d = xv_debug(&r); sprintf(line, "After calling `removeMe`: %.3900s", d); host_trace(line); free(d); xv_free(&r);
        list = xv_undef(); list.t = XV_LIST;
        xv_list_push(&list, xv_str("string")); xv_list_push(&list, xv_num(100)); xv_list_push(&list, xv_null()); xv_list_push(&list, xv_bool(0));
        a[0] = xv_str("trace"); a[1] = list;
        player_call_internal(p, "callWith", 2, a, &r); xv_free(&a[0]); xv_free(&a[1]);
        d = xv_debug(&r); sprintf(line, "After calling `callWith` with a complex payload: %.3900s", d); host_trace(line); free(d); xv_free(&r);
    }
}

/* one frame of audio in virtual time; AVM2 players mix inside their frame (after timers and streams) */
static int g_mix_frames;
static void hwav_write(const s16 *buf, int frames);
static void host_mix(Player *p) { static s16 scratch[8192 * 2]; if (g_mix_frames > 0) { audio_mix(p, scratch, g_mix_frames); hwav_write(scratch, g_mix_frames); } g_mix_frames = 0; }
static FILE *g_hwav; static unsigned long g_hwav_bytes;
static void hwav_u32(FILE *f, unsigned long v){ fputc(v&255,f); fputc((v>>8)&255,f); fputc((v>>16)&255,f); fputc((v>>24)&255,f); }
static void hwav_open(const char *path){ g_hwav=fopen(path,"wb"); if(!g_hwav) return; fwrite("RIFF",1,4,g_hwav); hwav_u32(g_hwav,0); fwrite("WAVEfmt ",1,8,g_hwav); hwav_u32(g_hwav,16); fputc(1,g_hwav);fputc(0,g_hwav); fputc(2,g_hwav);fputc(0,g_hwav); hwav_u32(g_hwav,AUDIO_RATE); hwav_u32(g_hwav,AUDIO_RATE*4); fputc(4,g_hwav);fputc(0,g_hwav); fputc(16,g_hwav);fputc(0,g_hwav); fwrite("data",1,4,g_hwav); hwav_u32(g_hwav,0); }
static void hwav_write(const s16 *buf, int frames){ if(g_hwav && frames>0){ fwrite(buf,4,frames,g_hwav); g_hwav_bytes += (unsigned long)frames*4; } }
static void hwav_close(void){ if(!g_hwav) return; fseek(g_hwav,4,SEEK_SET); hwav_u32(g_hwav,36+g_hwav_bytes); fseek(g_hwav,40,SEEK_SET); hwav_u32(g_hwav,g_hwav_bytes); fclose(g_hwav); g_hwav=NULL; }

/* the test harness's device fonts: FLASHNT_FONTS="family|bold|italic|path;..." and
   FLASHNT_DEFAULT_FONTS="sans=A,B;serif=...;typewriter=..." (Ruffle's [fonts] and [default_fonts]) */
static void host_register_fonts(void) {
    const char *fl = getenv("FLASHNT_FONTS"), *df = getenv("FLASHNT_DEFAULT_FONTS");
    if (!getenv("FLASHNT_RUFFLE_FONTS")) return;   /* the image runner's font handling (Ruffle's registry) */
    if (!fl && !df && !getenv("FLASHNT_TEST_FONT")) {
        /* FLASHNT_NO_DEVICE_FONTS: the harness without with_default_font has no device fonts at all */
        if (getenv("FLASHNT_NO_DEVICE_FONTS")) g_devfont_registry = 1;
        return;
    }
    if (getenv("FLASHNT_TEST_FONT")) {
        /* with_default_font: Noto Sans, the default for every family */
        DevFace *nf = devfont_get(DEVFONT_SANS, 0);
        if (nf) devfont_register("Noto Sans", 0, 0, nf);
        devfont_set_default(0, "Noto Sans"); devfont_set_default(1, "Noto Sans"); devfont_set_default(2, "Noto Sans");
    } else g_devfont_registry = 1;
    while (fl && *fl) {
        char fam[128], path[1024]; int bold = 0, italic = 0, k = 0; const char *e = strchr(fl, ';'); size_t len = e ? (size_t)(e - fl) : strlen(fl);
        char item[1300];
        if (len >= sizeof(item)) len = sizeof(item) - 1;
        memcpy(item, fl, len); item[len] = 0;
        fam[0] = path[0] = 0;
        {
            char *p1 = strchr(item, '|'), *p2 = p1 ? strchr(p1 + 1, '|') : NULL, *p3 = p2 ? strchr(p2 + 1, '|') : NULL;
            if (p1 && p2 && p3) {
                *p1 = *p2 = *p3 = 0;
                strncpy(fam, item, sizeof(fam) - 1); fam[sizeof(fam) - 1] = 0;
                bold = atoi(p1 + 1); italic = atoi(p2 + 1);
                strncpy(path, p3 + 1, sizeof(path) - 1); path[sizeof(path) - 1] = 0;
                k = 1;
            }
        }
        if (k) {
            FILE *fp = fopen(path, "rb");
            if (fp) {
                long sz; u8 *data;
                fseek(fp, 0, SEEK_END); sz = ftell(fp); fseek(fp, 0, SEEK_SET);
                data = sz > 0 ? (u8*)malloc((size_t)sz) : NULL;
                if (data && fread(data, 1, (size_t)sz, fp) == (size_t)sz) { DevFace *f = devfont_load_ttf(data, (u32)sz); if (f) devfont_register(fam, bold, italic, f); }
                fclose(fp);
            }
        }
        fl = e ? e + 1 : NULL;
    }
    while (df && *df) {
        const char *e = strchr(df, ';'); size_t len = e ? (size_t)(e - df) : strlen(df); char item[600]; char *eq;
        if (len >= sizeof(item)) len = sizeof(item) - 1;
        memcpy(item, df, len); item[len] = 0;
        if ((eq = strchr(item, '=')) != NULL) {
            *eq = 0;
            devfont_set_default(!strcmp(item, "sans") ? 0 : !strcmp(item, "serif") ? 1 : !strcmp(item, "typewriter") ? 2 : -1, eq + 1);
        }
        df = e ? e + 1 : NULL;
    }
}

int main(int argc, char **argv) {
    int err, n = 1, every = 1, i, t;
    double zoom = 1.0;
    const char *outdir = ".";
    int dump = 0, bench = 0, bench_from = 0; clock_t bench_tick = 0, bench_render = 0;
    SWFMovie *m; Player *p;
    char stack_base;
    struct rlimit rl;
    char path[512];
    int keyat[32], keycode[32], nkeys = 0;
    int held[64][2], nheld = 0;
    int clickat[32], clickx[32], clicky[32], nclicks = 0;
    const char *typetext[16]; int typeat[16], ntype = 0;
    FILE *script = NULL; char line[512]; const char *flist = NULL;

    if (argc < 2) { usage(); return 1; }
    {   /* AS3 recursion stops at 3/4 of the thread's stack (#1023) instead of crashing */
        extern char *g_a2_cstack_base; extern size_t g_a2_stack_limit;
        size_t lim = 8u << 20;
        if (!getrlimit(RLIMIT_STACK, &rl) && rl.rlim_cur != RLIM_INFINITY && rl.rlim_cur > 0) lim = (size_t)rl.rlim_cur;
        g_a2_cstack_base = &stack_base; g_a2_stack_limit = lim / 4 * 3;
    }
    if (getenv("FLASHNT_EI_TEST")) g_ext_provider = host_ei_provider;
    if (getenv("FLASHNT_TEST_FONT")) {
        extern const unsigned long test_fonts_raw_size, test_fonts_z_size; extern const u8 test_fonts_z[];
        extern const unsigned long test_fonts_r_raw_size, test_fonts_r_z_size; extern const u8 test_fonts_r_z[]; extern const int test_fonts_r_em;
        extern const u8 *g_devfont_alt_z; extern unsigned long g_devfont_alt_z_size, g_devfont_alt_raw_size; extern int g_devfont_alt_em;
        if (getenv("FLASHNT_RUFFLE_FONTS")) {   /* the image runner: the font in its own EM, as Ruffle reads it */
            g_devfont_alt_em = test_fonts_r_em;
            g_devfont_alt_z = test_fonts_r_z; g_devfont_alt_z_size = test_fonts_r_z_size; g_devfont_alt_raw_size = test_fonts_r_raw_size;
        } else { g_devfont_alt_z = test_fonts_z; g_devfont_alt_z_size = test_fonts_z_size; g_devfont_alt_raw_size = test_fonts_raw_size; }
    }
    host_register_fonts();
    for (i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "-n") && i + 1 < argc) n = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-z") && i + 1 < argc) zoom = atof(argv[++i]);
        else if (!strcmp(argv[i], "-o") && i + 1 < argc) outdir = argv[++i];
        else if (!strcmp(argv[i], "-e") && i + 1 < argc) every = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-d")) dump = 1;
        else if (!strcmp(argv[i], "-F") && i + 1 < argc) { flist = argv[++i]; g_fscommand_hook = host_fscommand; }
        else if (!strcmp(argv[i], "-B")) bench = 1;
        else if (!strcmp(argv[i], "-Bs") && i + 1 < argc) { bench = 1; bench_from = atoi(argv[++i]); }
        else if (!strcmp(argv[i], "-L")) { extern int g_fetch_log; g_fetch_log = 1; }
        else if (!strcmp(argv[i], "-T")) { extern int g_test_navigator; g_test_navigator = 1; }
        else if (!strcmp(argv[i], "-wav") && i + 1 < argc) hwav_open(argv[++i]);
        else if (!strcmp(argv[i], "-i") && i + 1 < argc) script = fopen(argv[++i], "r");
        else if (!strcmp(argv[i], "-t") && i + 1 < argc && ntype < 16) {
            char *a = argv[++i]; char *at = strrchr(a, '@');
            if (at) { *at = 0; typeat[ntype] = atoi(at + 1); } else typeat[ntype] = 0;
            typetext[ntype++] = a;
        }
        else if (!strcmp(argv[i], "-c") && i + 1 < argc && nclicks < 32) {
            const char *a = argv[++i]; const char *at = strchr(a, '@'); const char *cm = strchr(a, ',');
            clickx[nclicks] = atoi(a); clicky[nclicks] = cm ? atoi(cm + 1) : 0; clickat[nclicks] = at ? atoi(at + 1) : 0; nclicks++;
        }
        else if (!strcmp(argv[i], "-k") && i + 1 < argc && nkeys < 32) {
            const char *a = argv[++i]; const char *at = strchr(a, '@');
            keycode[nkeys] = atoi(a); keyat[nkeys] = at ? atoi(at + 1) : 0; nkeys++;
        }
    }
    { extern int (*a2_codepage_hook)(int dir, u32 cp, const void *src, int n, void *dst, int cap); a2_codepage_hook = host_codepage; }
    if (getenv("FLASHNT_SO_DIR")) { extern const char *g_store_dir; g_store_dir = getenv("FLASHNT_SO_DIR"); }
    if (getenv("FLASHNT_VIEWPORT")) {   /* "w,h,scale_factor": the test harness viewport */
        int w = 0, h = 0; double sf = 1;
        if (sscanf(getenv("FLASHNT_VIEWPORT"), "%d,%d,%lf", &w, &h, &sf) >= 2 && w > 0 && h > 0) {
            extern int a2_screen_w, a2_screen_h;
            g_view_w = w; g_view_h = h; g_view_sf = sf > 0 ? sf : 1;
            a2_screen_w = (int)floor(w / g_view_sf + 0.5); a2_screen_h = (int)floor(h / g_view_sf + 0.5);
        }
    }
    if (getenv("FLASHNT_QUALITY")) {   /* the test harness stage quality (low medium high best 8x8 8x8linear 16x16 16x16linear) */
        static const char *qn[8] = { "low", "medium", "high", "best", "8x8", "8x8linear", "16x16", "16x16linear" }; int q;
        extern int g_stage_quality;
        for (q = 0; q < 8; q++) if (!strcmp(getenv("FLASHNT_QUALITY"), qn[q])) g_stage_quality = q;
    }
    if (getenv("FLASHNT_DRIVER_INFO")) { extern const char *g_driver_info; g_driver_info = getenv("FLASHNT_DRIVER_INFO"); }
    if (getenv("FLASHNT_RELEASE")) { extern int g_player_release; g_player_release = 1; }   /* the harness's release player mode */
    if (getenv("FLASHNT_VERSION")) { extern const char *g_flash_version; g_flash_version = getenv("FLASHNT_VERSION"); }
    m = swf_load_file(argv[1], &err);
    if (!m) { fprintf(stderr, "load failed (%d)\n", err); return 1; }
    fprintf(stderr, "SWF v%d %gx%g @%g fps, %d frames, %d chars\n", m->version,
            (m->stage.xmax - m->stage.xmin) / 20.0, (m->stage.ymax - m->stage.ymin) / 20.0,
            m->framerate, m->framecount, m->maxid);
    if (m->has_abc) fprintf(stderr, "ActionScript 3 movie (AVM2)\n");
    /* an input script with editing commands drives text editing itself */
    if (script) { char l2[512]; extern int a2_external_text_control; while (fgets(l2, sizeof(l2), script)) if (l2[0] == 'X') { a2_external_text_control = 1; break; } rewind(script); }
    p = player_create(m, zoom);
    if (!p) { fprintf(stderr, "player create failed\n"); return 1; }
    if (flist) host_flush_captures(p, outdir);
    if (dump && !p->debug) p->debug = 1;
    { extern void (*a2_set_clipboard_hook)(const char *utf8); extern char *(*a2_get_clipboard_hook)(void); a2_set_clipboard_hook = host_clip_set; a2_get_clipboard_hook = host_clip_get; }
    for (t = 1; t <= n; t++) {
        for (i = 0; i < nkeys; i++) {
            if (keyat[i] == t) player_key(p, keycode[i], 1);
            if (keyat[i] + 3 == t) player_key(p, keycode[i], 0);
        }
        for (i = 0; i < nclicks; i++) {
            if (clickat[i] - 10 == t) player_mouse(p, clickx[i], clicky[i], 0);   /* hover first */
            if (clickat[i] == t) player_mouse(p, clickx[i], clicky[i], 1);
            if (clickat[i] + 1 == t) player_mouse(p, clickx[i], clicky[i], 0);
        }
        for (i = 0; i < ntype; i++) if (typeat[i] == t) { const char *q; for (q = typetext[i]; *q; q++) player_char(p, (u8)*q); }
        /* input script: events until a W line belong to this frame */
        if (script) {
            while (fgets(line, sizeof(line), script)) {
                int a = 0, b = 0, c = 0;
                if (line[0] == 'W') break;
                if (line[0] == 'K') {
                    int loc = 0, ch = -1, k, n = sscanf(line + 1, "%d %d %d %d", &a, &b, &loc, &ch);
                    if (n < 4) { player_key(p, a, b); continue; }
                    /* a key up without its key down (IME, dead keys) is dropped like Ruffle's InputManager */
                    for (k = 0; k < nheld; k++) if (held[k][0] == a && held[k][1] == loc) break;
                    if (b) { if (k == nheld && nheld < 64) { held[nheld][0] = a; held[nheld][1] = loc; nheld++; } }
                    else { if (k == nheld) continue; held[k][0] = held[nheld - 1][0]; held[k][1] = held[nheld - 1][1]; nheld--; }
                    player_key_ex(p, a, b, loc, ch);
                }
                else if (line[0] == 'T') { sscanf(line + 1, "%d", &a); player_char(p, a); }
                else if (line[0] == 'M') { int ci = 0; double fa = 0, fb = 0; if (sscanf(line + 1, "%lf %lf %d %d", &fa, &fb, &c, &ci) < 4) ci = -1; a = (int)floor(fa); b = (int)floor(fb); p->mouse_fx = fa - a; p->mouse_fy = fb - b; if (c) player_click_index(p, ci); player_mouse(p, (int)(a * zoom), (int)(b * zoom), c); }
                else if (line[0] == 'C') { size_t l = strlen(line); while (l && (line[l - 1] == '\n' || line[l - 1] == '\r')) line[--l] = 0; host_clip_set_escaped(line + 2); }
                else if (line[0] == 'X') { size_t l = strlen(line); while (l && (line[l - 1] == '\n' || line[l - 1] == '\r')) line[--l] = 0; player_text_control(p, line + 2); }
                else if (line[0] == 'I' || line[0] == 'J') {
                    size_t l = strlen(line); int fr = -1, to = -1, k = 0; const char *tx;
                    while (l && (line[l - 1] == '\n' || line[l - 1] == '\r')) line[--l] = 0;
                    if (line[0] == 'I') { sscanf(line + 1, "%d %d%n", &fr, &to, &k); tx = line + 1 + k; if (*tx == ' ') tx++; player_ime_preedit(p, tx, fr, to); }
                    else player_ime_commit(p, l > 2 ? line + 2 : "");
                }
                else if (line[0] == 'F') { sscanf(line + 1, "%d", &a); if (!a) player_focus_lost(p); }
                else if (line[0] == 'H') { sscanf(line + 1, "%d", &a); player_mouse_wheel(p, a); }
                else if (line[0] == 'B') { int btn = 0; sscanf(line + 1, "%d %d %d %d", &a, &b, &btn, &c); player_mouse_button(p, (int)(a * zoom), (int)(b * zoom), btn, c); }
            }
        }
        if (bench && t >= bench_from) { clock_t c0 = clock(); player_render(p); bench_render += clock() - c0; }
        if (flist) {
            player_render(p);
            if (frame_listed(flist, t) || t == n) {
                sprintf(path, "%s/frame_%04d.png", outdir, t);
                write_png(path, p->surf);
            }
        }
        else if (t == 1 || (t % every) == 0 || t == n) {
            player_render(p);
            sprintf(path, "%s/frame_%04d.png", outdir, t);
            write_png(path, p->surf);
            fprintf(stderr, "frame %d (root frame %d) -> %s\n", t, clip_current_frame(p->root) + 1, path);
        }
        if (t < n) {
            /* consume audio in virtual time so Sound.position / onSoundComplete behave */
            static s16 scratch[8192 * 2];
            int frames = (int)(AUDIO_RATE / (m->framerate > 0 ? m->framerate : 12.0));
            if (frames > 8192) frames = 8192;
            p->mix_hook = host_mix;
            if (p->avm2) g_mix_frames = frames; else { audio_mix(p, scratch, frames); hwav_write(scratch, frames); }
            host_advance_clock(1000.0 / (m->framerate > 0 ? m->framerate : 12.0));
            { clock_t c0 = clock(); player_tick(p); bench_tick += clock() - c0; }
            if (t == 1 && getenv("FLASHNT_EI_TEST")) host_ei_after_first_tick(p, getenv("FLASHNT_EI_TEST"));
            if (flist) host_flush_captures(p, outdir);
        }
    }
    if (p->avm2) { extern void avm2_final_timers(Player *p); avm2_final_timers(p); }
    if (bench) fprintf(stderr, "bench: %d frames, tick %.2f ms/frame, render %.2f ms/frame\n", n, bench_tick * 1000.0 / CLOCKS_PER_SEC / n, bench_render * 1000.0 / CLOCKS_PER_SEC / n);
    if (dump) { extern void avm1_debug_dump(Player *p, void *fout); avm1_debug_dump(p, stderr); }
    if (getenv("FLASHNT_TREE")) { extern void avm2_dump_tree(Player *p, void *fout); avm2_dump_tree(p, stderr); }
    hwav_close();
    player_free(p);
    swf_free(m);
    return 0;
}

/* ---- System.useCodepage: Flash decodes with the system code page; this host has
   none, so it guesses like Ruffle: UTF-8, Shift-JIS, else Windows-1252 ---- */
#include <iconv.h>
static int valid_utf8(const u8 *d, size_t n) {
    size_t i = 0;
    while (i < n) {
        u8 c = d[i]; int k, len = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 0;
        if (!len || i + len > n) return 0;
        for (k = 1; k < len; k++) if ((d[i + k] & 0xC0) != 0x80) return 0;
        i += len;
    }
    return 1;
}
static int looks_sjis(const u8 *d, size_t n) {
    size_t i = 0; int pairs = 0;
    while (i < n) {
        u8 c = d[i];
        if (c < 0x80 || (c >= 0xA1 && c <= 0xDF)) { i++; continue; }
        if (((c >= 0x81 && c <= 0x9F) || (c >= 0xE0 && c <= 0xFC)) && i + 1 < n && d[i + 1] >= 0x40 && d[i + 1] <= 0xFC && d[i + 1] != 0x7F) { pairs++; i += 2; continue; }
        return 0;
    }
    return pairs > 0;
}
/* ByteArray.readMultiByte / writeMultiByte code pages (Win32 does this with MultiByteToWideChar) */
static const char *cp_name(u32 cp, char *buf) {
    switch (cp) {
    case 0: return "WINDOWS-1252"; case 932: return "CP932"; case 936: return "GBK"; case 949: return "CP949"; case 950: return "BIG5";
    case 20932: return "EUC-JP"; case 54936: return "GB18030"; case 50220: return "ISO-2022-JP"; case 50225: return "ISO-2022-KR";
    case 20866: return "KOI8-R"; case 21866: return "KOI8-U"; case 10000: return "MACINTOSH"; case 10001: return "SHIFT_JIS";
    }
    if (cp >= 28591 && cp <= 28605) { sprintf(buf, "ISO-8859-%u", (unsigned)(cp - 28590)); return buf; }
    sprintf(buf, "CP%u", (unsigned)cp); return buf;
}
int host_codepage(int dir, unsigned int cp, const void *src, int n, void *dst, int cap) {
    char nb[32]; const char *name = cp_name(cp, nb); iconv_t cd; char *ip, *op; size_t il, ol;
    cd = dir ? iconv_open("UTF-16LE", name) : iconv_open(name, "UTF-16LE");
    if (cd == (iconv_t)-1) return -1;
    ip = (char*)src; il = dir ? (size_t)n : (size_t)n * 2; op = (char*)dst; ol = dir ? (size_t)cap * 2 : (size_t)cap;
    while (il > 0) {
        if (iconv(cd, &ip, &il, &op, &ol) == (size_t)-1) {
            if (ol < 2) break;
            if (dir) { ip++; il--; *op++ = (char)0xFD; *op++ = (char)0xFF; ol -= 2; }   /* U+FFFD */
            else { ip += 2; il = il >= 2 ? il - 2 : 0; *op++ = '?'; ol--; }
        }
    }
    iconv_close(cd);
    return dir ? (int)((op - (char*)dst) / 2) : (int)(op - (char*)dst);
}
char *plat_codepage_to_utf8(const u8 *d, size_t n, size_t *outlen) {
    const char *from = valid_utf8(d, n) ? "UTF-8" : looks_sjis(d, n) ? "SHIFT_JIS" : "WINDOWS-1252";
    iconv_t cd = iconv_open("UTF-8", from); char *out, *ip, *op; size_t il = n, ol = n * 4 + 4, cap = ol;
    if (cd == (iconv_t)-1) return NULL;
    out = (char*)malloc(cap + 1);
    if (!out) { iconv_close(cd); return NULL; }
    ip = (char*)d; op = out;
    while (il > 0) {
        if (iconv(cd, &ip, &il, &op, &ol) == (size_t)-1) { if (il) { ip++; il--; } else break; }
    }
    iconv_close(cd);
    *op = 0; *outlen = (size_t)(op - out);
    return out;
}

/* ---- time for Date (FLASHNT_NOW_MS / FLASHNT_TZ_MINUTES pin them for tests) ---- */
#include <time.h>
/* the headless host runs faster than real time: Date follows the frames played
   (start time + frames / frame rate), like getTimer() */
static double g_virtual_ms;
void host_advance_clock(double ms) { g_virtual_ms += ms; }
double plat_now_ms(void) {
    const char *e = getenv("FLASHNT_NOW_MS"); static double start;
    if (e && *e) return atof(e);
    if (!start) start = (double)time(NULL) * 1000.0;
    return start + g_virtual_ms;
}
int plat_tz_minutes(void) {
    const char *e = getenv("FLASHNT_TZ_MINUTES"); time_t now; struct tm lt, gt; long d;
    if (e && *e) return atoi(e);
    now = time(NULL); lt = *localtime(&now); gt = *gmtime(&now);
    d = (lt.tm_hour - gt.tm_hour) * 60L + (lt.tm_min - gt.tm_min);
    if (lt.tm_yday != gt.tm_yday) d += (lt.tm_yday > gt.tm_yday || (lt.tm_yday == 0 && gt.tm_yday > 300)) ? 1440 : -1440;
    return (int)d;
}

/* ---- TCP (XMLSocket) ---- */
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/select.h>
#include "sock.h"
int plat_tcp_connect(const char *host, int port) {
    struct hostent *he = gethostbyname(host); struct sockaddr_in sa; int fd;
    if (!he || port <= 0) return -1;
    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
    memset(&sa, 0, sizeof(sa)); sa.sin_family = AF_INET; sa.sin_port = htons((unsigned short)port);
    memcpy(&sa.sin_addr, he->h_addr_list[0], 4);
    if (connect(fd, (struct sockaddr*)&sa, sizeof(sa)) < 0 && errno != EINPROGRESS) { close(fd); return -1; }
    return fd;
}
int plat_tcp_poll(int fd, int *connected, int *readable) {
    fd_set w, r, e; struct timeval tv = { 0, 0 }; int err = 0; socklen_t el = sizeof(err);
    FD_ZERO(&w); FD_ZERO(&r); FD_ZERO(&e); FD_SET(fd, &w); FD_SET(fd, &r); FD_SET(fd, &e);
    if (select(fd + 1, &r, &w, &e, &tv) < 0) return -1;
    if (FD_ISSET(fd, &w) || FD_ISSET(fd, &r)) { getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &el); if (err) return -1; *connected = 1; }
    *readable = FD_ISSET(fd, &r) ? 1 : 0;
    return 0;
}
int plat_tcp_send(int fd, const u8 *d, size_t n) { return (int)send(fd, d, n, 0); }
int plat_tcp_recv(int fd, u8 *d, size_t n) { ssize_t k = recv(fd, d, n, 0); if (k < 0) return -1; return (int)k; }
void plat_tcp_close(int fd) { close(fd); }

/* no dialogs on the command line host: always cancelled */
int plat_file_dialog(int save, const char *filter, const char *name, char *out, size_t cap) { (void)save; (void)filter; (void)name; (void)out; (void)cap; return 0; }
