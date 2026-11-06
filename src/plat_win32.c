/* plat_win32.c - Retro Flash Player: the Windows NT 3.1+ / Windows 95+ front end.
 *
 * Uses only API present in NT 3.1 and Win95: RegisterClassA/CreateWindowExA,
 * GDI SetDIBitsToDevice (no DIB sections), waveOut (polled, no callbacks),
 * GetOpenFileNameA, timeGetTime/timeBeginPeriod, classic menus + TrackPopupMenu.  ANSI
 * everywhere.  Builds with the tiny CRT in ntcrt.c so no msvcrt.dll is
 * needed. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmsystem.h>
#include <commdlg.h>
#include <stdio.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "player.h"
#include "version.h"
#include "audio.h"
#include "avm1.h"
#include "raster.h"

static void fpu_init(void);

#define AUDIO_BUFS   16
#define AUDIO_FRAMES 1024      /* per buffer: ~23 ms at 44.1 kHz */
#define AUDIO_BUF_MS (AUDIO_FRAMES * 1000.0 / AUDIO_RATE)

/* menu command ids */
enum {
    IDM_OPEN = 101, IDM_CLOSE, IDM_EXIT, IDM_PRINT,
    IDM_ZOOM_IN = 201, IDM_ZOOM_OUT, IDM_ZOOM_100, IDM_ZOOM_FIT, IDM_FULLSCREEN,
    IDM_Q_LOW, IDM_Q_MEDIUM, IDM_Q_HIGH, IDM_SHOWMENU,
    IDM_PLAY = 301, IDM_STOP, IDM_REWIND, IDM_FORWARD, IDM_BACK, IDM_LOOP, IDM_PAUSE,
    IDM_ABOUT = 401, IDM_HELP
};

/* dialog control ids */
enum { IDC_LOCATION = 1001, IDC_BROWSE, IDC_PROMPT, IDC_ABOUT_ICON, IDC_ABOUT_NAME, IDC_ABOUT_VER, IDC_ABOUT_TEXT };

static Player   *g_player;
static SWFMovie *g_movie;
static HWND      g_hwnd;
static HMENU     g_menubar, g_popup;
static int       g_client_w, g_client_h;
static HWAVEOUT  g_wave;
static WAVEHDR   g_hdr[AUDIO_BUFS];
static s16      *g_abuf[AUDIO_BUFS];
static int       g_audio_ok;
static int       g_paused;
static int       g_fullscreen;
static RECT      g_saved_rect;
static DWORD     g_saved_style;
static double    g_zoom_fixed;   /* 0 = fit to window */
static char      g_title[300];
static char      g_path[520];
static HINSTANCE g_hinst;

/* ---- audio ---- */

/* Audio runs on its own thread so a slow frame can never starve it.  The
   thread owns the waveOut queue; `g_audio_cs` guards the mixer against the
   main thread's player_tick (both touch the player).  The buffers queued
   ahead (g_audio_target) adapt to the slowest recent frame: low latency
   when drawing is fast, no gaps when it is slow. */
static int              g_audio_next;         /* next ring buffer to queue */
static int              g_audio_target = 8;   /* buffers kept queued */
static double           g_loop_max_ms;        /* slowest recent frame, decaying */
static CRITICAL_SECTION g_audio_cs;
static int              g_audio_cs_ok;
static HANDLE           g_audio_thread;
static volatile int     g_audio_quit;
static volatile long    g_audio_underruns;    /* buffers that drained with none queued */

void audio_lock(void)   { if (g_audio_cs_ok) EnterCriticalSection(&g_audio_cs); }
void audio_unlock(void) { if (g_audio_cs_ok) LeaveCriticalSection(&g_audio_cs); }

/* queue buffers up to the target; called only by the audio thread (or, as a
   fallback, the main loop when the thread could not start) */
static void audio_pump(void) {
    int queued = 0, i;
    if (!g_audio_ok) return;
    for (i = 0; i < AUDIO_BUFS; i++) if (!(g_hdr[i].dwFlags & WHDR_DONE)) queued++;
    if (queued == 0) g_audio_underruns++;
    while (queued < g_audio_target && (g_hdr[g_audio_next].dwFlags & WHDR_DONE)) {
        WAVEHDR *h = &g_hdr[g_audio_next];
        /* g_player is read and used only inside the lock, so a File > Close on
           the main thread cannot free it while this mixes */
        audio_lock();
        if (g_paused || !g_player) memset(g_abuf[g_audio_next], 0, AUDIO_FRAMES * 4);
        else audio_mix(g_player, g_abuf[g_audio_next], AUDIO_FRAMES);
        audio_unlock();
        h->dwFlags &= ~WHDR_DONE;
        waveOutWrite(g_wave, h, sizeof(WAVEHDR));
        g_audio_next = (g_audio_next + 1) % AUDIO_BUFS; queued++;
    }
}

static DWORD WINAPI audio_thread(LPVOID arg) {
    (void)arg;
    while (!g_audio_quit) { audio_pump(); Sleep(2); }
    return 0;
}

static void audio_open(void) {
    WAVEFORMATEX wf; int i;
    memset(&wf, 0, sizeof(wf));
    wf.wFormatTag = WAVE_FORMAT_PCM; wf.nChannels = 2; wf.nSamplesPerSec = AUDIO_RATE;
    wf.wBitsPerSample = 16; wf.nBlockAlign = 4; wf.nAvgBytesPerSec = AUDIO_RATE * 4;
    if (waveOutOpen(&g_wave, WAVE_MAPPER, &wf, 0, 0, CALLBACK_NULL) != MMSYSERR_NOERROR) { g_audio_ok = 0; return; }
    for (i = 0; i < AUDIO_BUFS; i++) {
        g_abuf[i] = (s16*)calloc(AUDIO_FRAMES * 2, sizeof(s16));
        memset(&g_hdr[i], 0, sizeof(WAVEHDR));
        g_hdr[i].lpData = (LPSTR)g_abuf[i];
        g_hdr[i].dwBufferLength = AUDIO_FRAMES * 4;
        waveOutPrepareHeader(g_wave, &g_hdr[i], sizeof(WAVEHDR));
        g_hdr[i].dwFlags |= WHDR_DONE;          /* start as free */
    }
    g_audio_ok = 1;
    InitializeCriticalSection(&g_audio_cs); g_audio_cs_ok = 1;
    { DWORD id; g_audio_thread = CreateThread(NULL, 0, audio_thread, NULL, 0, &id); }
    if (g_audio_thread) SetThreadPriority(g_audio_thread, THREAD_PRIORITY_ABOVE_NORMAL);
}

/* the target and the stream lead track the slowest recent frame */
static void audio_retarget(double loop_ms, double frame_ms) {
    int t;
    g_loop_max_ms = loop_ms > g_loop_max_ms ? loop_ms : g_loop_max_ms * 0.98 + loop_ms * 0.02;
    t = (int)ceil((g_loop_max_ms * 1.3 + 40.0) / AUDIO_BUF_MS);
    if (t < 5) t = 5; if (t > AUDIO_BUFS) t = AUDIO_BUFS;
    g_audio_target = t;
    g_stream_lead_ms = t * AUDIO_BUF_MS + frame_ms;
}

/* optional diagnostics to retrofp.log (set RETROFP_DIAG=1): one line a second
   with the measured frame rate, the slowest frame, dropped frames and the
   audio queue, so a stutter can be diagnosed from the user's own machine */
static int g_diag;
double g_diag_tick_ms, g_diag_render_ms, g_diag_blit_ms;
static void audio_diag(int ticks, double loop_ms, double frame_ms) {
    static DWORD win; static int frames, draws, drops; static double mx, mxt, mxr, mxb; static long u0;
    DWORD now;
    if (!g_diag) { g_diag = GetEnvironmentVariableA("RETROFP_DIAG", NULL, 0) ? 1 : -1; if (g_diag < 0) return; win = timeGetTime(); u0 = g_audio_underruns; }
    if (g_diag < 0) return;
    now = timeGetTime();
    frames += ticks; if (ticks) draws++; if (ticks > 1) drops += ticks - 1;
    if (loop_ms > mx) mx = loop_ms;
    if (g_diag_tick_ms > mxt) mxt = g_diag_tick_ms;
    if (g_diag_render_ms > mxr) mxr = g_diag_render_ms;
    if (g_diag_blit_ms > mxb) mxb = g_diag_blit_ms;
    if (now - win >= 1000) {
        int q = 0, i; for (i = 0; i < AUDIO_BUFS; i++) if (!(g_hdr[i].dwFlags & WHDR_DONE)) q++;
        DWORD el = now - win == 0 ? 1 : now - win;
        fprintf(stderr, "diag: %.1f fps drawn (%d frames advanced, %d dropped) slowest=%.0f [tick=%.0f render=%.0f blit=%.0f] /%.0f budget, audio q=%d/%d underruns=%ld\n",
                draws * 1000.0 / el, frames, drops, mx, mxt, mxr, mxb, frame_ms,
                q, g_audio_target, g_audio_underruns - u0);
        win = now; frames = draws = drops = 0; mx = mxt = mxr = mxb = 0; u0 = g_audio_underruns;
    }
}

static void audio_close(void) {
    int i;
    if (!g_audio_ok) return;
    g_audio_quit = 1;
    if (g_audio_thread) { WaitForSingleObject(g_audio_thread, 2000); CloseHandle(g_audio_thread); g_audio_thread = NULL; }
    waveOutReset(g_wave);
    for (i = 0; i < AUDIO_BUFS; i++) { waveOutUnprepareHeader(g_wave, &g_hdr[i], sizeof(WAVEHDR)); free(g_abuf[i]); }
    waveOutClose(g_wave);
    if (g_audio_cs_ok) { DeleteCriticalSection(&g_audio_cs); g_audio_cs_ok = 0; }
    g_audio_ok = 0;
}

/* ---- video ---- */

/* the stage is drawn at g_render_scale (adapted to the frame budget so the
   software rasterizer stays smooth) and the result is stretched to the display
   rectangle (g_disp_*), the stage fit into the client at g_zf, centred */
static double g_zf = 1.0, g_render_scale;    /* display fit zoom; current render scale (<= g_zf) */
static int    g_disp_x, g_disp_y, g_disp_w, g_disp_h, g_rscale_fixed;

static void blit(HDC dc) {
    BITMAPINFO bi;
    Surface *s;
    if (!g_player) return;
    s = g_player->surf;
    memset(&bi, 0, sizeof(bi));
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = s->w;
    bi.bmiHeader.biHeight = -s->h;              /* top-down */
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    if (s->w == g_disp_w && s->h == g_disp_h) {
        SetDIBitsToDevice(dc, g_disp_x, g_disp_y, s->w, s->h, 0, 0, 0, s->h, s->px, &bi, DIB_RGB_COLORS);
    } else {
        SetStretchBltMode(dc, COLORONCOLOR);    /* fast nearest-ish scale (NT 3.1 safe) */
        StretchDIBits(dc, g_disp_x, g_disp_y, g_disp_w, g_disp_h, 0, 0, s->w, s->h, s->px, &bi, DIB_RGB_COLORS, SRCCOPY);
    }
    /* letterbox: Flash Player fills the area around the stage with the movie's background colour */
    if (g_disp_x > 0 || g_disp_y > 0 || g_disp_x + g_disp_w < g_client_w || g_disp_y + g_disp_h < g_client_h) {
        HBRUSH b = CreateSolidBrush(RGB(g_player->bg.r, g_player->bg.g, g_player->bg.b)); RECT r;
        if (g_disp_y > 0) { SetRect(&r, 0, 0, g_client_w, g_disp_y); FillRect(dc, &r, b); }
        if (g_disp_y + g_disp_h < g_client_h) { SetRect(&r, 0, g_disp_y + g_disp_h, g_client_w, g_client_h); FillRect(dc, &r, b); }
        if (g_disp_x > 0) { SetRect(&r, 0, g_disp_y, g_disp_x, g_disp_y + g_disp_h); FillRect(dc, &r, b); }
        if (g_disp_x + g_disp_w < g_client_w) { SetRect(&r, g_disp_x + g_disp_w, g_disp_y, g_client_w, g_disp_y + g_disp_h); FillRect(dc, &r, b); }
        DeleteObject(b);
    }
}

static double stage_w(void) { return (g_movie->stage.xmax - g_movie->stage.xmin) / 20.0; }
static double stage_h(void) { return (g_movie->stage.ymax - g_movie->stage.ymin) / 20.0; }

static void resize_surface(void) {
    double sw = stage_w(), sh = stage_h(), zf, zr;
    int w, h;
    if (!g_player) return;
    if (g_zoom_fixed > 0) zf = g_zoom_fixed;
    else { double zx = g_client_w / sw, zy = g_client_h / sh; zf = zx < zy ? zx : zy; }
    if (zf < 0.05) zf = 0.05;
    g_zf = zf;
    g_disp_w = (int)(sw * zf + 0.5); g_disp_h = (int)(sh * zf + 0.5);
    if (g_disp_w < 1) g_disp_w = 1; if (g_disp_h < 1) g_disp_h = 1;
    g_disp_x = (g_client_w - g_disp_w) / 2; g_disp_y = (g_client_h - g_disp_h) / 2;
    /* render scale: 0 (reset) -> full; otherwise clamped to the display zoom.
       RETROFP_RSCALE pins the render scale as a fraction of the display zoom
       (0.1..1), disabling the adaptive change (manual quality/speed control) */
    {
        static double g_rscale_env = -2.0; char e[16];
        if (g_rscale_env < -1.0) g_rscale_env = GetEnvironmentVariableA("RETROFP_RSCALE", e, sizeof(e)) ? atof(e) : 0.0;
        if (g_rscale_env > 0.0) { double f = g_rscale_env > 1.0 ? 1.0 : g_rscale_env < 0.1 ? 0.1 : g_rscale_env; g_render_scale = zf * f; g_rscale_fixed = 1; }
    }
    if (g_render_scale <= 0 || g_render_scale > zf) g_render_scale = zf;
    zr = g_render_scale; if (zr < 0.1) zr = 0.1; if (zr > zf) zr = zf;
    w = (int)(sw * zr + 0.5); h = (int)(sh * zr + 0.5);
    if (w < 1) w = 1; if (h < 1) h = 1;
    if (w > 4096) w = 4096; if (h > 4096) h = 4096;
    if (g_player->surf && g_player->surf->w == w && g_player->surf->h == h) { g_player->zoom = zr; return; }
    surf_free(g_player->surf);
    g_player->surf = surf_create(w, h);
    g_player->zoom = zr;
}

/* lower the render resolution when drawing overruns the frame budget, raise it
   when there is headroom, so playback stays smooth at the best resolution the
   machine can sustain (fullscreen included) */
static void adapt_render_scale(double render_ms, double frame_ms) {
    /* never render below the native stage resolution: at or under native window
       size the image stays crisp; only an enlarged window (g_zf > 1) may drop
       scale, and only down to native (1.0), which is then stretched up */
    double ns = g_render_scale, lo = g_zf < 1.0 ? g_zf : 1.0;
    if (g_render_scale <= 0 || g_rscale_fixed) return;
    if (render_ms > frame_ms * 0.95 && ns > lo) ns *= 0.85;               /* overrun: drop fast */
    else if (render_ms > 0 && render_ms < frame_ms * 0.55 && ns < g_zf) ns *= 1.06;   /* headroom: raise gently */
    else return;
    if (ns < lo) ns = lo; if (ns > g_zf) ns = g_zf;
    if (ns < g_render_scale * 0.98 || ns > g_render_scale * 1.02) { g_render_scale = ns; resize_surface(); }
}

static void repaint(void) {
    if (!g_player) return;
    player_render(g_player);
    InvalidateRect(g_hwnd, NULL, TRUE);
}

/* window point -> stage-surface pixel (through the stretched display rectangle) */
static int to_stage_x(int x) { return g_disp_w > 0 && g_player ? (int)((double)(x - g_disp_x) * g_player->surf->w / g_disp_w) : x; }
static int to_stage_y(int y) { return g_disp_h > 0 && g_player ? (int)((double)(y - g_disp_y) * g_player->surf->h / g_disp_h) : y; }

/* ---- menus ---- */

static HMENU build_control_items(HMENU m) {
    AppendMenuA(m, MF_STRING, IDM_PLAY, "&Play\tCtrl+Enter");
    AppendMenuA(m, MF_STRING, IDM_REWIND, "&Rewind\tCtrl+R");
    AppendMenuA(m, MF_STRING, IDM_LOOP, "&Loop\tCtrl+L");
    AppendMenuA(m, MF_SEPARATOR, 0, NULL);
    AppendMenuA(m, MF_STRING, IDM_FORWARD, "Step &Forward\t.");
    AppendMenuA(m, MF_STRING, IDM_BACK, "Step &Backward\t,");
    return m;
}

static HMENU build_quality_menu(void) {
    HMENU q = CreatePopupMenu();
    AppendMenuA(q, MF_STRING, IDM_Q_LOW, "&Low");
    AppendMenuA(q, MF_STRING, IDM_Q_MEDIUM, "&Medium");
    AppendMenuA(q, MF_STRING, IDM_Q_HIGH, "&High");
    return q;
}

static void build_menus(void) {
    HMENU file = CreatePopupMenu(), view = CreatePopupMenu(), ctrl = CreatePopupMenu(), help = CreatePopupMenu(), mag = CreatePopupMenu();
    AppendMenuA(file, MF_STRING, IDM_OPEN, "&Open...\tCtrl+O");
    AppendMenuA(file, MF_STRING, IDM_CLOSE, "&Close\tCtrl+W");
    AppendMenuA(file, MF_SEPARATOR, 0, NULL);
    AppendMenuA(file, MF_STRING, IDM_EXIT, "E&xit\tCtrl+Q");
    AppendMenuA(mag, MF_STRING, IDM_ZOOM_IN, "Zoom &In\tCtrl+=");
    AppendMenuA(mag, MF_STRING, IDM_ZOOM_OUT, "Zoom &Out\tCtrl+-");
    AppendMenuA(mag, MF_SEPARATOR, 0, NULL);
    AppendMenuA(mag, MF_STRING, IDM_ZOOM_100, "&100%\tCtrl+1");
    AppendMenuA(mag, MF_STRING, IDM_ZOOM_FIT, "Show &All\tCtrl+3");
    AppendMenuA(view, MF_POPUP, (UINT)mag, "&Magnification");
    AppendMenuA(view, MF_POPUP, (UINT)build_quality_menu(), "&Quality");
    AppendMenuA(view, MF_SEPARATOR, 0, NULL);
    AppendMenuA(view, MF_STRING, IDM_FULLSCREEN, "&Full Screen\tCtrl+F");
    build_control_items(ctrl);
    AppendMenuA(help, MF_STRING, IDM_HELP, "Retro Flash Player &Help\tF1");
    AppendMenuA(help, MF_SEPARATOR, 0, NULL);
    AppendMenuA(help, MF_STRING, IDM_ABOUT, "&About Retro Flash Player");
    g_menubar = CreateMenu();
    AppendMenuA(g_menubar, MF_POPUP, (UINT)file, "&File");
    AppendMenuA(g_menubar, MF_POPUP, (UINT)view, "&View");
    AppendMenuA(g_menubar, MF_POPUP, (UINT)ctrl, "&Control");
    AppendMenuA(g_menubar, MF_POPUP, (UINT)help, "&Help");

    /* Flash Player style right-click menu */
    g_popup = CreatePopupMenu();
    AppendMenuA(g_popup, MF_STRING, IDM_ZOOM_IN, "Zoom In");
    AppendMenuA(g_popup, MF_STRING, IDM_ZOOM_OUT, "Zoom Out");
    AppendMenuA(g_popup, MF_STRING, IDM_ZOOM_100, "100%");
    AppendMenuA(g_popup, MF_STRING, IDM_ZOOM_FIT, "Show All");
    AppendMenuA(g_popup, MF_SEPARATOR, 0, NULL);
    AppendMenuA(g_popup, MF_POPUP, (UINT)build_quality_menu(), "Quality");
    AppendMenuA(g_popup, MF_SEPARATOR, 0, NULL);
    AppendMenuA(g_popup, MF_STRING, IDM_PLAY, "Play");
    AppendMenuA(g_popup, MF_STRING, IDM_LOOP, "Loop");
    AppendMenuA(g_popup, MF_STRING, IDM_REWIND, "Rewind");
    AppendMenuA(g_popup, MF_STRING, IDM_FORWARD, "Forward");
    AppendMenuA(g_popup, MF_STRING, IDM_BACK, "Back");
    AppendMenuA(g_popup, MF_SEPARATOR, 0, NULL);
    AppendMenuA(g_popup, MF_STRING, IDM_ABOUT, "About Retro Flash Player...");
}

static void update_checks(HMENU m) {
    int playing = g_player && g_player->root->playing && !g_paused;
    CheckMenuItem(m, IDM_PLAY, MF_BYCOMMAND | (playing ? MF_CHECKED : MF_UNCHECKED));
    CheckMenuItem(m, IDM_LOOP, MF_BYCOMMAND | ((g_player && g_player->loop) ? MF_CHECKED : MF_UNCHECKED));
    CheckMenuItem(m, IDM_PAUSE, MF_BYCOMMAND | (g_paused ? MF_CHECKED : MF_UNCHECKED));
    CheckMenuItem(m, IDM_FULLSCREEN, MF_BYCOMMAND | (g_fullscreen ? MF_CHECKED : MF_UNCHECKED));
    CheckMenuItem(m, IDM_Q_LOW, MF_BYCOMMAND | (g_stage_quality == 0 ? MF_CHECKED : MF_UNCHECKED));
    CheckMenuItem(m, IDM_Q_MEDIUM, MF_BYCOMMAND | (g_stage_quality == 1 ? MF_CHECKED : MF_UNCHECKED));
    CheckMenuItem(m, IDM_Q_HIGH, MF_BYCOMMAND | (g_stage_quality >= 2 ? MF_CHECKED : MF_UNCHECKED));
    CheckMenuItem(m, IDM_ZOOM_FIT, MF_BYCOMMAND | (g_zoom_fixed == 0 ? MF_CHECKED : MF_UNCHECKED));
    CheckMenuItem(m, IDM_ZOOM_100, MF_BYCOMMAND | (g_zoom_fixed == 1.0 ? MF_CHECKED : MF_UNCHECKED));
}

/* ---- movie load / unload ---- */

static void close_movie(void) {
    /* the audio thread mixes g_player; swap it out under the lock */
    audio_lock();
    { Player *p = g_player; g_player = NULL; if (p) player_free(p); }
    audio_unlock();
    if (g_movie) { swf_free(g_movie); g_movie = NULL; }
    strcpy(g_title, APP_NAME);
    if (g_hwnd) { SetWindowTextA(g_hwnd, g_title); InvalidateRect(g_hwnd, NULL, TRUE); }
}

static int open_movie(const char *path) {
    int err;
    SWFMovie *m = swf_load_file(path, &err);
    if (!m) { MessageBoxA(g_hwnd, "Cannot load this SWF (unsupported or damaged).", APP_NAME, MB_OK | MB_ICONSTOP); return 0; }
    close_movie();
    g_movie = m;
    strncpy(g_path, path, sizeof(g_path) - 1);
    sprintf(g_title, APP_NAME " - %.120s", strrchr(path, '\\') ? strrchr(path, '\\') + 1 : path);
    if (g_hwnd) SetWindowTextA(g_hwnd, g_title);
    {   /* publish the new player to the audio thread atomically */
        Player *np = player_create(m, 1.0);
        audio_lock(); g_player = np; audio_unlock();
    }
    if (!g_player) return 0;
    resize_surface();
    repaint();
    return 1;
}


/* size the window so the client area matches the stage (Flash Player does this on open) */
static void fit_window_to_stage(void) {
    RECT r;
    if (g_fullscreen || !g_player) return;
    r.left = 0; r.top = 0; r.right = (LONG)stage_w(); r.bottom = (LONG)stage_h();
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, TRUE);
    SetWindowPos(g_hwnd, NULL, 0, 0, r.right - r.left, r.bottom - r.top, SWP_NOMOVE | SWP_NOZORDER);
}

/* ---- dialogs (in-memory templates: no resource compiler, NT 3.1 safe) ---- */

typedef struct { WORD *buf; int n, cap; } DlgT;
static void dt_w(DlgT *t, WORD w) { if (t->n == t->cap) { t->cap = t->cap ? t->cap * 2 : 256; t->buf = (WORD*)realloc(t->buf, (size_t)t->cap * sizeof(WORD)); } t->buf[t->n++] = w; }
static void dt_str(DlgT *t, const char *s) { while (*s) dt_w(t, (WORD)(unsigned char)*s++); dt_w(t, 0); }
static void dt_align(DlgT *t) { if (t->n & 1) dt_w(t, 0); }      /* DWORD alignment (n counts WORDs) */
static void dt_begin(DlgT *t, DWORD style, int nitems, short x, short y, short cx, short cy, const char *title) {
    t->n = 0;
    dt_w(t, (WORD)style); dt_w(t, (WORD)(style >> 16)); dt_w(t, 0); dt_w(t, 0);   /* style, exstyle */
    dt_w(t, (WORD)nitems); dt_w(t, (WORD)x); dt_w(t, (WORD)y); dt_w(t, (WORD)cx); dt_w(t, (WORD)cy);
    dt_w(t, 0); dt_w(t, 0); dt_str(t, title);                                     /* no menu, default class */
    dt_w(t, 8); dt_str(t, "MS Sans Serif");                                       /* DS_SETFONT */
}
static void dt_item(DlgT *t, DWORD style, short x, short y, short cx, short cy, WORD id, WORD cls, const char *text) {
    dt_align(t);
    style |= WS_CHILD | WS_VISIBLE;
    dt_w(t, (WORD)style); dt_w(t, (WORD)(style >> 16)); dt_w(t, 0); dt_w(t, 0);
    dt_w(t, (WORD)x); dt_w(t, (WORD)y); dt_w(t, (WORD)cx); dt_w(t, (WORD)cy); dt_w(t, id);
    dt_w(t, 0xFFFF); dt_w(t, cls);                                                 /* 0x80 button 0x81 edit 0x82 static 0x85 combobox */
    if (text) dt_str(t, text); else dt_w(t, 0);
    dt_w(t, 0);                                                                    /* no creation data */
}

/* recently opened locations (the Open dialog's drop-down) */
static char g_mru[6][520]; static int g_nmru;
static void mru_add(const char *p) {
    int i, j;
    for (i = 0; i < g_nmru; i++) if (!strcmp(g_mru[i], p)) break;
    if (i == g_nmru) { if (g_nmru < 6) g_nmru++; i = g_nmru - 1; }
    for (j = i; j > 0; j--) strcpy(g_mru[j], g_mru[j - 1]);
    strncpy(g_mru[0], p, 519); g_mru[0][519] = 0;
}

static int pick_file(char *path, int cap) {
    OPENFILENAMEA ofn;
    memset(&ofn, 0, sizeof(ofn));
    path[0] = 0;
    ofn.lStructSize = 76;                 /* OPENFILENAME_SIZE_VERSION_400: what NT 3.x / Win95 accept */
    ofn.hwndOwner = GetActiveWindow();
    ofn.lpstrFilter = "Flash Movies (*.swf)\0*.swf\0All Files (*.*)\0*.*\0\0";
    ofn.lpstrFile = path; ofn.nMaxFile = (DWORD)cap;
    ofn.lpstrTitle = "Open";
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_HIDEREADONLY | OFN_PATHMUSTEXIST;
    if ((GetVersion() & 0xFF) >= 4) ofn.Flags |= 0x00080000;   /* OFN_EXPLORER on Win95 / NT4+ */
    return GetOpenFileNameA(&ofn) ? 1 : 0;
}

/* DS_CENTER is ignored before Windows 95: centre over the player window by hand */
static void center_dlg(HWND d) {
    RECT pr, dr; int x, y;
    GetWindowRect(g_hwnd ? g_hwnd : GetDesktopWindow(), &pr); GetWindowRect(d, &dr);
    x = pr.left + ((pr.right - pr.left) - (dr.right - dr.left)) / 2;
    y = pr.top + ((pr.bottom - pr.top) - (dr.bottom - dr.top)) / 3;
    if (x < 0) x = 0; if (y < 0) y = 0;
    SetWindowPos(d, NULL, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER);
}

static char *g_open_result;
static BOOL CALLBACK open_dlgproc(HWND d, UINT m, WPARAM wp, LPARAM lp) {
    (void)lp;
    switch (m) {
    case WM_INITDIALOG: {
        int i; HWND cb = GetDlgItem(d, IDC_LOCATION);
        center_dlg(d);
        for (i = 0; i < g_nmru; i++) SendMessageA(cb, CB_ADDSTRING, 0, (LPARAM)g_mru[i]);
        if (g_nmru) SetWindowTextA(cb, g_mru[0]);
        EnableWindow(GetDlgItem(d, IDOK), g_nmru > 0);
        SetFocus(cb);
        return FALSE; }
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDC_LOCATION:
            if (HIWORD(wp) == CBN_EDITCHANGE || HIWORD(wp) == CBN_SELCHANGE) {
                char t[8]; HWND cb = GetDlgItem(d, IDC_LOCATION);
                if (HIWORD(wp) == CBN_SELCHANGE) { int sel = (int)SendMessageA(cb, CB_GETCURSEL, 0, 0); EnableWindow(GetDlgItem(d, IDOK), sel >= 0); }
                else { GetWindowTextA(cb, t, sizeof(t)); EnableWindow(GetDlgItem(d, IDOK), t[0] != 0); }
            }
            return TRUE;
        case IDC_BROWSE: {
            char p[520];
            if (pick_file(p, sizeof(p))) { SetWindowTextA(GetDlgItem(d, IDC_LOCATION), p); EnableWindow(GetDlgItem(d, IDOK), TRUE); SetFocus(GetDlgItem(d, IDOK)); }
            return TRUE; }
        case IDOK: {
            HWND cb = GetDlgItem(d, IDC_LOCATION); int sel = (int)SendMessageA(cb, CB_GETCURSEL, 0, 0);
            g_open_result[0] = 0;
            GetWindowTextA(cb, g_open_result, 519);
            if (!g_open_result[0] && sel >= 0) SendMessageA(cb, CB_GETLBTEXT, (WPARAM)sel, (LPARAM)g_open_result);
            EndDialog(d, g_open_result[0] ? 1 : 0); return TRUE; }
        case IDCANCEL: EndDialog(d, 0); return TRUE;
        }
        break;
    }
    return FALSE;
}
/* File > Open: the Flash Player 10 "Open" dialog (location box + Browse...) */
static int open_dialog(char *out) {
    DlgT t; int r;
    memset(&t, 0, sizeof(t));
    dt_begin(&t, DS_MODALFRAME | DS_SETFONT | 0x0004L /* DS_3DLOOK */ | WS_POPUP | WS_CAPTION | WS_SYSMENU | DS_CENTER, 6, 0, 0, 262, 82, "Open");
    dt_item(&t, SS_LEFT, 8, 8, 246, 24, IDC_PROMPT, 0x82, "Enter the World Wide Web location (URL) of the file you would like to open, or click Browse to select a file from your computer.");
    dt_item(&t, SS_LEFT, 8, 39, 32, 9, (WORD)-1, 0x82, "&Location:");
    dt_item(&t, CBS_DROPDOWN | CBS_AUTOHSCROLL | WS_VSCROLL | WS_TABSTOP, 44, 37, 210, 80, IDC_LOCATION, 0x85, NULL);
    dt_item(&t, BS_PUSHBUTTON | WS_TABSTOP, 8, 60, 58, 14, IDC_BROWSE, 0x80, "&Browse...");
    dt_item(&t, BS_DEFPUSHBUTTON | WS_TABSTOP, 134, 60, 58, 14, IDOK, 0x80, "OK");
    dt_item(&t, BS_PUSHBUTTON | WS_TABSTOP, 196, 60, 58, 14, IDCANCEL, 0x80, "Cancel");
    g_open_result = out;
    r = (int)DialogBoxIndirectParamA(g_hinst, (LPCDLGTEMPLATEA)t.buf, g_hwnd, (DLGPROC)open_dlgproc, 0);
    free(t.buf);
    return r == 1;
}

static BOOL CALLBACK about_dlgproc(HWND d, UINT m, WPARAM wp, LPARAM lp) {
    (void)lp;
    switch (m) {
    case WM_INITDIALOG: {
        HICON ic = LoadIconA(g_hinst, MAKEINTRESOURCEA(1));
        if (!ic) ic = LoadIconA(NULL, IDI_APPLICATION);
        SendDlgItemMessageA(d, IDC_ABOUT_ICON, STM_SETICON, (WPARAM)ic, 0);
        center_dlg(d);
        return TRUE; }
    case WM_COMMAND: if (LOWORD(wp) == IDOK || LOWORD(wp) == IDCANCEL) { EndDialog(d, 0); return TRUE; } break;
    }
    return FALSE;
}
/* Help > About: same layout as Flash Player 10's About box (product, version, copyright, OK) */
static const char *g_sse_state;
static void about_dialog(void) {
    DlgT t; char ver[160];
    memset(&t, 0, sizeof(t));
    sprintf(ver, "Version " APP_VERSION ", %s", g_sse_state ? g_sse_state : "x87");
    dt_begin(&t, DS_MODALFRAME | DS_SETFONT | 0x0004L /* DS_3DLOOK */ | WS_POPUP | WS_CAPTION | WS_SYSMENU | DS_CENTER, 5, 0, 0, 250, 140, "About " APP_NAME);
    dt_item(&t, SS_ICON, 12, 12, 20, 20, IDC_ABOUT_ICON, 0x82, NULL);
    dt_item(&t, SS_LEFT, 44, 12, 194, 10, IDC_ABOUT_NAME, 0x82, APP_NAME " " APP_VERSION " by " APP_AUTHOR);
    dt_item(&t, SS_LEFT, 44, 24, 194, 10, IDC_ABOUT_VER, 0x82, ver);
    dt_item(&t, SS_LEFT, 44, 40, 198, 74, IDC_ABOUT_TEXT, 0x82,
        "A from-scratch SWF player for Windows NT 3.1/3.5x/4.0, 2000/XP and later, and Windows 95/98/ME. Based on Flash Player 10.\n"
        "SWF 1-10, ActionScript 1/2/3, vector and bitmap rendering, text, sound and video.\n\n"
        "Flash is a trademark of Adobe. Retro Flash Player is not affiliated with Adobe.");
    dt_item(&t, BS_DEFPUSHBUTTON | WS_TABSTOP, 184, 120, 58, 14, IDOK, 0x80, "OK");
    DialogBoxIndirectParamA(g_hinst, (LPCDLGTEMPLATEA)t.buf, g_hwnd, (DLGPROC)about_dlgproc, 0);
    free(t.buf);
}

static void toggle_fullscreen(void) {
    if (!g_fullscreen) {
        GetWindowRect(g_hwnd, &g_saved_rect);
        g_saved_style = GetWindowLongA(g_hwnd, GWL_STYLE);
        SetMenu(g_hwnd, NULL);
        SetWindowLongA(g_hwnd, GWL_STYLE, WS_POPUP | WS_VISIBLE);
        SetWindowPos(g_hwnd, HWND_TOP, 0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN), SWP_FRAMECHANGED | SWP_SHOWWINDOW);
        g_fullscreen = 1;
    } else {
        SetWindowLongA(g_hwnd, GWL_STYLE, g_saved_style);
        SetMenu(g_hwnd, g_menubar);
        SetWindowPos(g_hwnd, HWND_NOTOPMOST, g_saved_rect.left, g_saved_rect.top,
                     g_saved_rect.right - g_saved_rect.left, g_saved_rect.bottom - g_saved_rect.top, SWP_FRAMECHANGED | SWP_SHOWWINDOW);
        g_fullscreen = 0;
    }
    if (g_player) g_player->fullscreen = g_fullscreen;
}

/* Stage.displayState from scripts */
int plat_set_fullscreen(Player *p, int on) {
    (void)p;
    if ((on != 0) != (g_fullscreen != 0)) toggle_fullscreen();
    return 1;
}

static void step(int dir) {
    if (!g_player) return;
    if (dir > 0) { g_paused = 1; g_player->root->playing = 1; player_tick(g_player); }
    else { int f = clip_current_frame(g_player->root) - 1; g_paused = 1; if (f >= 0) clip_goto_frame(g_player->root, f, 0); }
    repaint();
}

static void on_command(int id) {
    char path[520];
    switch (id) {
    case IDM_OPEN:
        if (open_dialog(path)) {
            char *p = path;
            if (!strncmp(p, "file:///", 8)) p += 8; else if (!strncmp(p, "file://", 7)) p += 7;
            if (!strncmp(p, "http://", 7) || !strncmp(p, "https://", 8)) { MessageBoxA(g_hwnd, "Opening network locations is not supported yet.", APP_NAME, MB_OK | MB_ICONEXCLAMATION); break; }
            if (open_movie(p)) { mru_add(path); fit_window_to_stage(); }
        }
        break;
    case IDM_CLOSE: close_movie(); break;
    case IDM_EXIT: DestroyWindow(g_hwnd); break;
    case IDM_ZOOM_IN: g_zoom_fixed = (g_zoom_fixed > 0 ? g_zoom_fixed : g_zf) * 1.25; g_render_scale = 0; resize_surface(); repaint(); break;
    case IDM_ZOOM_OUT: g_zoom_fixed = (g_zoom_fixed > 0 ? g_zoom_fixed : g_zf) / 1.25; g_render_scale = 0; resize_surface(); repaint(); break;
    case IDM_ZOOM_100: g_zoom_fixed = 1.0; g_render_scale = 0; resize_surface(); repaint(); break;
    case IDM_ZOOM_FIT: g_zoom_fixed = 0; g_render_scale = 0; resize_surface(); repaint(); break;
    case IDM_FULLSCREEN: toggle_fullscreen(); break;
    case IDM_Q_LOW: g_stage_quality = 0; repaint(); break;
    case IDM_Q_MEDIUM: g_stage_quality = 1; repaint(); break;
    case IDM_Q_HIGH: g_stage_quality = 2; repaint(); break;
    case IDM_PLAY: if (g_player) { if (g_player->root->playing && !g_paused) g_player->root->playing = 0; else { g_paused = 0; g_player->root->playing = 1; } } break;
    case IDM_STOP: if (g_player) g_player->root->playing = 0; break;
    case IDM_PAUSE: g_paused = !g_paused; break;
    case IDM_REWIND: if (g_player) { clip_goto_frame(g_player->root, 0, g_player->root->playing); repaint(); } break;
    case IDM_FORWARD: step(1); break;
    case IDM_BACK: step(-1); break;
    case IDM_LOOP: if (g_player) g_player->loop = !g_player->loop; break;
    case IDM_ABOUT: about_dialog(); break;
    case IDM_HELP:
        MessageBoxA(g_hwnd,
            "Keyboard shortcuts:\n\n"
            "Ctrl+O  Open          Ctrl+W  Close        Ctrl+Q  Exit\n"
            "Enter   Play/Stop     Ctrl+R  Rewind       Ctrl+L  Loop\n"
            ".  ,    Step forward / backward\n"
            "Ctrl+=  Zoom in       Ctrl+-  Zoom out\n"
            "Ctrl+1  100%          Ctrl+3  Show all\n"
            "Ctrl+F  Full screen   Esc     Leave full screen\n\n"
            "Right-click the movie for the context menu.",
            APP_NAME " Help", MB_OK | MB_ICONINFORMATION);
        break;
    default: break;
    }
}

/* KeyboardEvent.keyLocation of a key message: 1 left, 2 right, 3 numpad */
static int key_location(WPARAM vk, LPARAM lp) {
    int ext = (int)((lp >> 24) & 1), sc = (int)((lp >> 16) & 0xFF);
    if (vk == VK_SHIFT) return sc == 0x36 ? 2 : 1;
    if (vk == VK_CONTROL || vk == VK_MENU) return ext ? 2 : 1;
    if ((vk >= VK_NUMPAD0 && vk <= VK_DIVIDE)) return 3;
    if (!ext && ((vk >= VK_PRIOR && vk <= VK_DOWN) || vk == VK_INSERT || vk == VK_DELETE || vk == VK_CLEAR)) return 3;   /* the keypad without NumLock */
    if (ext && vk == VK_RETURN) return 3;
    return 0;
}
/* KeyboardEvent.charCode: the character the key types; at a key down Windows has
   already queued its WM_CHAR (TranslateMessage), so peek at it */
static int g_key_char[256];
static int key_char_down(HWND h, WPARAM vk) {
    MSG msg; int c = 0;
    if (vk == VK_DELETE) c = 127;
    else if (PeekMessage(&msg, h, WM_CHAR, WM_CHAR, PM_NOREMOVE)) c = (int)msg.wParam;
    if (c < 32 && c != 8 && c != 9 && c != 13 && c != 27 && vk >= 'A' && vk <= 'Z') c = (int)vk + 32;   /* Ctrl+letter */
    if (c < 32 && c != 8 && c != 9 && c != 13 && c != 27) c = 0;
    if (vk < 256) g_key_char[vk] = c;
    return c;
}
static int key_char_up(WPARAM vk) {
    int c = vk < 256 ? g_key_char[vk] : 0;
    if (vk >= 'A' && vk <= 'Z' && c && (c | 32) == (int)vk + 32) {   /* a letter follows the shift state at release */
        int up = ((GetKeyState(VK_SHIFT) & 0x8000) != 0) != ((GetKeyState(VK_CAPITAL) & 1) != 0);
        c = up ? (int)vk : (int)vk + 32;
    }
    return c;
}
static LRESULT CALLBACK wndproc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    switch (m) {
    case WM_PAINT: {
        PAINTSTRUCT ps; HDC dc = BeginPaint(h, &ps);
        blit(dc);
        EndPaint(h, &ps);
        return 0; }
    case WM_ERASEBKGND:
        if (!g_player) { RECT r; GetClientRect(h, &r); FillRect((HDC)wp, &r, (HBRUSH)GetStockObject(WHITE_BRUSH)); }
        return 1;   /* with a movie, blit() paints every pixel (stage + letterbox) */
    case WM_SIZE:
        g_client_w = LOWORD(lp); g_client_h = HIWORD(lp);
        /* new window size: render at full resolution again, then adapt down */
        if (g_player && g_client_w > 0 && g_client_h > 0) { g_render_scale = 0; resize_surface(); repaint(); }
        return 0;
    case WM_COMMAND:
        on_command(LOWORD(wp));
        return 0;
    case WM_INITMENUPOPUP:
        update_checks((HMENU)wp);
        return 0;
    case WM_RBUTTONDOWN: {
        POINT pt; pt.x = (short)LOWORD(lp); pt.y = (short)HIWORD(lp);
        if (g_player) player_mouse_button(g_player, to_stage_x(pt.x), to_stage_y(pt.y), 2, 1);
        ClientToScreen(h, &pt);
        update_checks(g_popup);
        TrackPopupMenu(g_popup, TPM_LEFTALIGN | TPM_RIGHTBUTTON, pt.x, pt.y, 0, h, NULL);
        if (g_player) player_mouse_button(g_player, to_stage_x((short)LOWORD(lp)), to_stage_y((short)HIWORD(lp)), 2, 0);
        return 0; }
    case WM_MBUTTONDOWN:
        if (g_player) player_mouse_button(g_player, to_stage_x((short)LOWORD(lp)), to_stage_y((short)HIWORD(lp)), 1, 1);
        return 0;
    case WM_MBUTTONUP:
        if (g_player) player_mouse_button(g_player, to_stage_x((short)LOWORD(lp)), to_stage_y((short)HIWORD(lp)), 1, 0);
        return 0;
    case WM_KEYDOWN:
        if (wp == VK_F1) { on_command(IDM_HELP); return 0; }
        if (wp == VK_ESCAPE && g_fullscreen) { toggle_fullscreen(); return 0; }
        if (GetKeyState(VK_CONTROL) & 0x8000) {
            switch (wp) {
            case VK_RETURN: on_command(IDM_PLAY); return 0;
            case 'O': on_command(IDM_OPEN); return 0;
            case 'W': on_command(IDM_CLOSE); return 0;
            case 'Q': on_command(IDM_EXIT); return 0;
            case 'R': on_command(IDM_REWIND); return 0;
            case 'L': on_command(IDM_LOOP); return 0;
            case 'F': on_command(IDM_FULLSCREEN); return 0;
            case '1': on_command(IDM_ZOOM_100); return 0;
            case '3': case '0': on_command(IDM_ZOOM_FIT); return 0;
            case VK_ADD: case 0xBB: on_command(IDM_ZOOM_IN); return 0;
            case VK_SUBTRACT: case 0xBD: on_command(IDM_ZOOM_OUT); return 0;
            default: break;
            }
        }
        if (g_player && !(lp & (1 << 30))) player_key_ex(g_player, (int)wp, 1, key_location(wp, lp), key_char_down(h, wp));
        return 0;
    case WM_KEYUP:
        if (g_player) player_key_ex(g_player, (int)wp, 0, key_location(wp, lp), key_char_up(wp));
        return 0;
    case WM_CHAR:
        if (g_player) player_char(g_player, (int)wp);
        return 0;
    case 0x020A:   /* WM_MOUSEWHEEL (NT 4 / Windows 98 and IntelliPoint on 95); not in old headers */
        if (g_player) { static int acc; acc += (short)HIWORD(wp); while (acc >= 120 || acc <= -120) { int d = acc > 0 ? 3 : -3; player_mouse_wheel(g_player, d); acc -= acc > 0 ? 120 : -120; } }
        return 0;
    case WM_MOUSEMOVE:
        if (g_player) player_mouse(g_player, to_stage_x((short)LOWORD(lp)), to_stage_y((short)HIWORD(lp)), (wp & MK_LBUTTON) ? 1 : 0);
        return 0;
    case WM_LBUTTONDOWN:
        SetCapture(h);
        if (g_player) player_mouse(g_player, to_stage_x((short)LOWORD(lp)), to_stage_y((short)HIWORD(lp)), 1);
        return 0;
    case WM_KILLFOCUS:
        if (g_player) player_focus_lost(g_player);
        break;
    case WM_LBUTTONUP:
        ReleaseCapture();
        if (g_player) player_mouse(g_player, to_stage_x((short)LOWORD(lp)), to_stage_y((short)HIWORD(lp)), 0);
        return 0;
    case WM_CLOSE:
        DestroyWindow(h);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    default: break;
    }
    return DefWindowProcA(h, m, wp, lp);
}

static void strip_quotes(char *s) {
    size_t n = strlen(s);
    while (n && (s[n - 1] == ' ' || s[n - 1] == '"')) s[--n] = 0;
    if (s[0] == '"') memmove(s, s + 1, n);
}

/* ByteArray.readMultiByte / writeMultiByte code pages */
/* System.setClipboard: UTF-8 -> UTF-16 by hand (CP_UTF8 is missing on NT 3.1), then
   CF_UNICODETEXT where the system has it plus CF_TEXT in the ANSI code page. */
static void win_set_clipboard(const char *u) {
    size_t n = strlen(u), i = 0; int k = 0, an; WCHAR *w = (WCHAR*)malloc((n + 1) * sizeof(WCHAR)); HGLOBAL h; char *a;
    if (!w) return;
    while (i < n) {
        unsigned c = (unsigned char)u[i];
        if (c < 0x80) { w[k++] = (WCHAR)c; i++; }
        else if ((c >> 5) == 6 && i + 1 < n) { w[k++] = (WCHAR)(((c & 31) << 6) | (u[i + 1] & 63)); i += 2; }
        else if ((c >> 4) == 14 && i + 2 < n) { w[k++] = (WCHAR)(((c & 15) << 12) | ((u[i + 1] & 63) << 6) | (u[i + 2] & 63)); i += 3; }
        else if ((c >> 3) == 30 && i + 3 < n) { unsigned cp = ((c & 7) << 18) | ((u[i + 1] & 63) << 12) | ((u[i + 2] & 63) << 6) | (u[i + 3] & 63); cp -= 0x10000; w[k++] = (WCHAR)(0xD800 + (cp >> 10)); w[k++] = (WCHAR)(0xDC00 + (cp & 1023)); i += 4; }
        else { w[k++] = (WCHAR)'?'; i++; }
    }
    w[k] = 0;
    if (!OpenClipboard(g_hwnd)) { free(w); return; }
    EmptyClipboard();
    an = k ? WideCharToMultiByte(CP_ACP, 0, w, k, NULL, 0, NULL, NULL) : 0; if (an < 0) an = 0;
    if ((h = GlobalAlloc(GMEM_MOVEABLE | GMEM_DDESHARE, (DWORD)an + 1)) != NULL) {
        a = (char*)GlobalLock(h); if (an) WideCharToMultiByte(CP_ACP, 0, w, k, a, an, NULL, NULL); a[an] = 0; GlobalUnlock(h);
        SetClipboardData(CF_TEXT, h);
    }
    if ((GetVersion() & 0x80000000u) == 0 && (h = GlobalAlloc(GMEM_MOVEABLE | GMEM_DDESHARE, (DWORD)(k + 1) * sizeof(WCHAR))) != NULL) {   /* NT: Unicode too */
        WCHAR *d = (WCHAR*)GlobalLock(h); memcpy(d, w, (size_t)(k + 1) * sizeof(WCHAR)); GlobalUnlock(h);
        SetClipboardData(CF_UNICODETEXT, h);
    }
    CloseClipboard();
    free(w);
}

/* TextField paste: clipboard text as UTF-8 (CF_UNICODETEXT on NT, CF_TEXT elsewhere) */
static char *win_get_clipboard(void) {
    HANDLE h; char *out = NULL; int nt = (GetVersion() & 0x80000000u) == 0;
    if (!OpenClipboard(g_hwnd)) return NULL;
    if (nt && (h = GetClipboardData(CF_UNICODETEXT)) != NULL) {
        const WCHAR *w = (const WCHAR*)GlobalLock(h);
        if (w) { size_t n = 0, i, k = 0; while (w[n]) n++; out = (char*)malloc(n * 3 + 1);
            if (out) for (i = 0; i < n; i++) { unsigned c = w[i];
                if (c >= 0xD800 && c < 0xDC00 && i + 1 < n) { unsigned c2 = w[i + 1]; unsigned cp = 0x10000 + ((c - 0xD800) << 10) + (c2 - 0xDC00); out = (char*)realloc(out, n * 3 + 8); out[k++] = (char)(0xF0 | (cp >> 18)); out[k++] = (char)(0x80 | ((cp >> 12) & 63)); out[k++] = (char)(0x80 | ((cp >> 6) & 63)); out[k++] = (char)(0x80 | (cp & 63)); i++; continue; }
                if (c < 0x80) out[k++] = (char)c; else if (c < 0x800) { out[k++] = (char)(0xC0 | (c >> 6)); out[k++] = (char)(0x80 | (c & 63)); } else { out[k++] = (char)(0xE0 | (c >> 12)); out[k++] = (char)(0x80 | ((c >> 6) & 63)); out[k++] = (char)(0x80 | (c & 63)); } }
            if (out) out[k] = 0; GlobalUnlock(h); }
    } else if ((h = GetClipboardData(CF_TEXT)) != NULL) {
        const char *a = (const char*)GlobalLock(h);
        if (a) { int n = (int)strlen(a), wn; WCHAR *w = (WCHAR*)malloc(((size_t)n + 1) * sizeof(WCHAR));
            if (w) { wn = MultiByteToWideChar(CP_ACP, 0, a, n, w, n + 1); if (wn < 0) wn = 0;
                out = (char*)malloc((size_t)wn * 3 + 1);
                if (out) { int i, k = 0; for (i = 0; i < wn; i++) { unsigned c = w[i]; if (c < 0x80) out[k++] = (char)c; else if (c < 0x800) { out[k++] = (char)(0xC0 | (c >> 6)); out[k++] = (char)(0x80 | (c & 63)); } else { out[k++] = (char)(0xE0 | (c >> 12)); out[k++] = (char)(0x80 | ((c >> 6) & 63)); out[k++] = (char)(0x80 | (c & 63)); } } out[k] = 0; }
                free(w); }
            GlobalUnlock(h); }
    }
    CloseClipboard();
    return out;
}

static int win_codepage(int dir, u32 cp, const void *src, int n, void *dst, int cap) {
    int r;
    if (n == 0) return 0;
    if (dir) r = MultiByteToWideChar(cp ? cp : CP_ACP, 0, (LPCSTR)src, n, (LPWSTR)dst, cap);
    else r = WideCharToMultiByte(cp ? cp : CP_ACP, 0, (LPCWSTR)src, n, (LPSTR)dst, cap, NULL, NULL);
    return r > 0 ? r : -1;
}
/* SharedObject storage: a folder next to the executable */
/* navigateToURL / getURL to a web page: the default browser (ShellExecute, loaded at run time) */
void plat_open_url(const char *url) {
    typedef HINSTANCE (WINAPI *ShellExecFn)(HWND, LPCSTR, LPCSTR, LPCSTR, LPCSTR, INT);
    static ShellExecFn fn; static int tried;
    if (!tried) { HMODULE h = LoadLibraryA("SHELL32.DLL"); tried = 1; if (h) fn = (ShellExecFn)GetProcAddress(h, "ShellExecuteA"); }
    if (fn) fn(NULL, "open", url, NULL, NULL, SW_SHOWNORMAL);
}
int plat_mkdir(const char *path) { return CreateDirectoryA(path, NULL) || GetLastError() == ERROR_ALREADY_EXISTS; }
static char g_store_path[MAX_PATH + 32];
static void init_store_dir(void) {
    extern const char *g_store_dir; char *sl;
    if (!GetModuleFileNameA(NULL, g_store_path, MAX_PATH)) return;
    sl = strrchr(g_store_path, '\\');
    if (!sl) return;
    strcpy(sl + 1, "SharedObjects");
    if (plat_mkdir(g_store_path)) g_store_dir = g_store_path;
}

int WINAPI WinMain(HINSTANCE hinst, HINSTANCE prev, LPSTR cmdline, int show) {
    WNDCLASSA wc;
    char path[520];
    MSG msg;
    DWORD last; double acc;
    RECT r;
    extern int (*a2_codepage_hook)(int dir, u32 cp, const void *src, int n, void *dst, int cap);
    (void)prev; (void)show;
    a2_codepage_hook = win_codepage;
    { extern int a2_screen_w, a2_screen_h; a2_screen_w = GetSystemMetrics(SM_CXSCREEN); a2_screen_h = GetSystemMetrics(SM_CYSCREEN); }
    { extern void (*a2_set_clipboard_hook)(const char *utf8); a2_set_clipboard_hook = win_set_clipboard; }
    { extern char *(*a2_get_clipboard_hook)(void); a2_get_clipboard_hook = win_get_clipboard; }
    g_hinst = hinst;
    init_store_dir();

    strncpy(path, cmdline ? cmdline : "", sizeof(path) - 1); path[sizeof(path) - 1] = 0;
    strip_quotes(path);

    memset(&wc, 0, sizeof(wc));
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = wndproc;
    wc.hInstance = hinst;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hIcon = LoadIconA(hinst, MAKEINTRESOURCEA(1));
    if (!wc.hIcon) wc.hIcon = LoadIcon(NULL, IDI_APPLICATION);
    wc.hbrBackground = (HBRUSH)GetStockObject(WHITE_BRUSH);   /* empty player window is white, as in Flash Player */
    wc.lpszClassName = "RetroFPWindow";
    RegisterClassA(&wc);
    build_menus();

    strcpy(g_title, APP_NAME);
    r.left = 0; r.top = 0; r.right = 550; r.bottom = 400;
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, TRUE);
    g_hwnd = CreateWindowExA(0, "RetroFPWindow", g_title, WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                             CW_USEDEFAULT, CW_USEDEFAULT, r.right - r.left, r.bottom - r.top,
                             NULL, g_menubar, hinst, NULL);
    if (!g_hwnd) return 1;
    GetClientRect(g_hwnd, &r);
    g_client_w = r.right; g_client_h = r.bottom;
    audio_open();

    if (path[0]) {
        if (open_movie(path)) {
            mru_add(path);
            /* size the window to the stage */
            r.left = 0; r.top = 0; r.right = (LONG)stage_w(); r.bottom = (LONG)stage_h();
            AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, TRUE);
            SetWindowPos(g_hwnd, NULL, 0, 0, r.right - r.left, r.bottom - r.top, SWP_NOMOVE | SWP_NOZORDER);
        }
    }   /* like Flash Player 10: no movie given -> empty window, use File > Open */

    timeBeginPeriod(1);   /* 1 ms timer and Sleep granularity (the default is ~15.6 ms on NT 6+) */
    last = timeGetTime(); acc = 0;
    for (;;) {
        DWORD now, elapsed; int ticks = 0, stream;
        double frame_ms;
        while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) goto done;
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
        }
        if (!g_player) { WaitMessage(); last = timeGetTime(); continue; }
        fpu_init();
        frame_ms = 1000.0 / (g_movie->framerate > 0 ? g_movie->framerate : 12.0);
        now = timeGetTime();
        elapsed = now - last; last = now;
        if (elapsed > 500) elapsed = 500;
        acc += elapsed;
        /* a timeline stream sound sets the pace: late frames are run without
           being drawn (Flash Player's frame dropping) so the sound stays whole */
        stream = g_audio_ok && audio_stream_active(g_player);
        { DWORD t0 = timeGetTime();
        /* catch up on skipped frames, but render often: stop ticking once this
           has used more than ~1.5 frames of wall time, so a run of slow ticks
           can't delay the next draw by hundreds of ms (visible smoothness) */
        double tick_budget = frame_ms * 1.5; if (tick_budget < 24) tick_budget = 24;
        while (acc >= frame_ms && ticks < (stream ? 12 : 4)) {
            if (!g_paused) {
                audio_lock();        /* the audio thread mixes from the same player */
                if (!g_audio_ok) {
                    /* no wave device: still advance sounds in virtual time so
                     * Sound.position / onSoundComplete keep scripts moving */
                    static s16 scratch[2048 * 2];
                    int n = (int)(AUDIO_RATE / (g_movie->framerate > 0 ? g_movie->framerate : 12.0));
                    while (n > 0) { int k = n > 2048 ? 2048 : n; audio_mix(g_player, scratch, k); n -= k; }
                }
                player_tick(g_player);
                audio_unlock();
            }
            acc -= frame_ms; ticks++;
            if (ticks >= 2 && (double)(timeGetTime() - t0) >= tick_budget) break;
        }
        g_diag_tick_ms = (double)(timeGetTime() - t0); }
        if (!stream && ticks >= 4) acc = 0;           /* too slow: drop the backlog */
        if (stream && acc > 1000.0) acc = frame_ms;   /* far behind (a stall): start again */
        g_diag_render_ms = g_diag_blit_ms = 0;
        if (ticks && !g_paused) {
            HDC dc; DWORD t1 = timeGetTime();
            player_render(g_player);   /* not locked: audio keeps flowing while we draw */
            g_diag_render_ms = (double)(timeGetTime() - t1); t1 = timeGetTime();
            dc = GetDC(g_hwnd); blit(dc); ReleaseDC(g_hwnd, dc);
            g_diag_blit_ms = (double)(timeGetTime() - t1);
        }
        if (!g_audio_thread) audio_pump();   /* fallback: no audio thread */
        if (ticks && !g_paused) adapt_render_scale(g_diag_render_ms, frame_ms);   /* keep drawing within budget */
        audio_retarget((double)(timeGetTime() - now), frame_ms);
        audio_diag(ticks, (double)(timeGetTime() - now), frame_ms);
        Sleep(1);
    }
done:
    timeEndPeriod(1);
    audio_close();
    close_movie();
    return 0;
}

/* x87: 53-bit precision, round to nearest, exceptions masked (0x27F, NT's
   default for new threads).  The NT runtime's math (ntmath*.c) relies on
   it; set at startup and again before each frame in case a DLL running on
   this thread (common dialogs, printer drivers, ...) changed it. */
static void fpu_init(void) {
#if defined(__GNUC__) && defined(__i386__)
    extern void nt_fpu_init(void);
    nt_fpu_init();
#endif
}

/* ---- SSE2 for the renderer's hot loops (raster_sse2.c) ----
   Used when the CPU has SSE2 and the OS lets programs run SSE instructions.
   Windows 98 and NT 5+ save the XMM registers on task switches.  NT 3.x/4.0
   and Windows 95 do not; they only run SSE when a driver such as WINSIMD has
   enabled it (CR4.OSFXSR).  There the process is kept on one CPU, so its XMM
   registers do not get left behind on another processor. */
#if defined(__GNUC__) && defined(__i386__)
static int cpu_has_sse2(void) {
    unsigned long f0, f1, a, b, c, d;
    __asm__ volatile ("pushfl; popl %0; movl %0, %1; xorl $0x200000, %0; pushl %0; popfl; pushfl; popl %0; pushl %1; popfl"
                      : "=&r" (f0), "=&r" (f1));
    if (!((f0 ^ f1) & 0x200000)) return 0;   /* no CPUID (386, early 486) */
    __asm__ volatile ("cpuid" : "=a" (a), "=b" (b), "=c" (c), "=d" (d) : "a" (0));
    if (a < 1) return 0;
    __asm__ volatile ("cpuid" : "=a" (a), "=b" (b), "=c" (c), "=d" (d) : "a" (1));
    return (d >> 26) & 1;
}
static volatile int g_sse_fault;
/* Frame-based SEH probe: SetUnhandledExceptionFilter is a last-resort filter
   and on NT 3.x/4.0 returning EXCEPTION_CONTINUE_EXECUTION from it does not
   reliably resume, so a CPU that has SSE2 but whose OS has not enabled it
   (no WINSIMD) crashes the process instead of falling back to x87.  A real
   frame-based handler (the fs:[0] chain) catches the #UD first-chance and
   resumes past it on every NT version. */
#ifndef EXCEPTION_ILLEGAL_INSTRUCTION
#define EXCEPTION_ILLEGAL_INSTRUCTION 0xC000001DL
#endif
static EXCEPTION_DISPOSITION __cdecl sse_seh(struct _EXCEPTION_RECORD *rec,
        void *frame, struct _CONTEXT *ctx, void *disp) {
    (void)frame; (void)disp;
    if (rec->ExceptionCode == (DWORD)EXCEPTION_ILLEGAL_INSTRUCTION ||
        rec->ExceptionCode == (DWORD)EXCEPTION_PRIV_INSTRUCTION) {
        g_sse_fault = 1; ctx->Eip += 4;              /* skip the 4-byte PXOR */
        return ExceptionContinueExecution;
    }
    return ExceptionContinueSearch;
}
static int os_runs_sse(void) {
    struct { void *prev; void *handler; } f;
    void *prev;
    g_sse_fault = 0;
    __asm__ volatile ("movl %%fs:0, %0" : "=r" (prev));    /* save current head */
    f.prev = prev; f.handler = (void*)sse_seh;
    __asm__ volatile ("movl %0, %%fs:0" :: "r" (&f) : "memory");   /* install */
    __asm__ volatile (".byte 0x66, 0x0f, 0xef, 0xc0" ::: "memory");/* pxor %xmm0,%xmm0 */
    __asm__ volatile ("movl %0, %%fs:0" :: "r" (prev) : "memory"); /* uninstall */
    return !g_sse_fault;
}
#endif
static const char *g_sse_state = "x87";
static void sse2_setup(void) {
#if defined(__GNUC__) && defined(__i386__)
    extern int g_cpu_sse2;
    DWORD v = GetVersion(); int nt = !(v & 0x80000000u), major = (int)(v & 0xFF), minor = (int)((v >> 8) & 0xFF);
    char e[8];
    if (GetEnvironmentVariableA("RETROFP_NOSSE", e, sizeof(e))) { g_sse_state = "x87"; return; }
    if (!cpu_has_sse2()) { g_sse_state = "x87"; return; }
    if (!os_runs_sse()) { g_sse_state = "x87"; return; }
    if ((nt && major >= 5) || (!nt && (major > 4 || (major == 4 && minor >= 10)))) g_sse_state = "SSE2";
    else {
        /* the OS does not save XMM: stay on the first CPU */
        typedef BOOL (WINAPI *SPAM)(HANDLE, DWORD);
        typedef BOOL (WINAPI *GPAM)(HANDLE, LPDWORD, LPDWORD);
        HMODULE k = GetModuleHandleA("KERNEL32.DLL");
        GPAM gp = k ? (GPAM)GetProcAddress(k, "GetProcessAffinityMask") : NULL;
        SPAM sp = k ? (SPAM)GetProcAddress(k, "SetProcessAffinityMask") : NULL;
        DWORD pm = 0, sm = 0;
        if (gp && gp(GetCurrentProcess(), &pm, &sm) && (pm & (pm - 1))) {
            if (!sp || !sp(GetCurrentProcess(), pm & (~pm + 1))) { g_sse_state = "x87"; return; }
        }
        g_sse_state = "SSE2";
    }
    g_cpu_sse2 = 1;
#endif
}

/* -nostdlib entry point */
void WinMainCRTStartup(void) {
    char *cmd = GetCommandLineA();
    fpu_init();
    sse2_setup();   /* before the crash log's exception filter goes in */
    { extern void nt_install_crash_log(void); nt_install_crash_log(); }
    {   /* the EXE reserves 32 MB of stack (build/mingw-nt31.sh, nt31.mak); AS3 recursion may use 24 MB of it */
        extern char *g_a2_cstack_base; extern size_t g_a2_stack_limit;
        g_a2_cstack_base = (char*)&cmd; g_a2_stack_limit = 24u << 20;
    }
    /* skip program name (possibly quoted) */
    if (*cmd == '"') { cmd++; while (*cmd && *cmd != '"') cmd++; if (*cmd) cmd++; }
    else while (*cmd && *cmd != ' ') cmd++;
    while (*cmd == ' ') cmd++;
    ExitProcess((UINT)WinMain(GetModuleHandleA(NULL), NULL, cmd, SW_SHOWNORMAL));
}

/* ---- time for Date ---- */
/* System.useCodepage: the system ANSI code page, to UTF-8 (CP_UTF8 is not on NT 3.1) */
char *plat_codepage_to_utf8(const u8 *d, size_t n, size_t *outlen) {
    int wn = MultiByteToWideChar(CP_ACP, 0, (const char*)d, (int)n, NULL, 0), i; WCHAR *w; char *out, *o;
    if (n == 0) { out = (char*)malloc(1); if (out) *out = 0; *outlen = 0; return out; }
    if (wn <= 0) return NULL;
    w = (WCHAR*)malloc((size_t)wn * sizeof(WCHAR));
    if (!w) return NULL;
    MultiByteToWideChar(CP_ACP, 0, (const char*)d, (int)n, w, wn);
    out = (char*)malloc((size_t)wn * 3 + 1);
    if (!out) { free(w); return NULL; }
    o = out;
    for (i = 0; i < wn; i++) {
        unsigned c = w[i];
        if (c >= 0xD800 && c <= 0xDBFF && i + 1 < wn && w[i + 1] >= 0xDC00 && w[i + 1] <= 0xDFFF) { c = 0x10000 + ((c - 0xD800) << 10) + (w[i + 1] - 0xDC00); i++; }
        if (c < 0x80) *o++ = (char)c;
        else if (c < 0x800) { *o++ = (char)(0xC0 | (c >> 6)); *o++ = (char)(0x80 | (c & 63)); }
        else if (c < 0x10000) { *o++ = (char)(0xE0 | (c >> 12)); *o++ = (char)(0x80 | ((c >> 6) & 63)); *o++ = (char)(0x80 | (c & 63)); }
        else { *o++ = (char)(0xF0 | (c >> 18)); *o++ = (char)(0x80 | ((c >> 12) & 63)); *o++ = (char)(0x80 | ((c >> 6) & 63)); *o++ = (char)(0x80 | (c & 63)); }
    }
    free(w);
    *o = 0; *outlen = (size_t)(o - out);
    return out;
}

double plat_now_ms(void) {
    SYSTEMTIME st; double days; long y, m;
    GetSystemTime(&st);
    y = st.wYear; m = st.wMonth;
    if (m <= 2) { y--; m += 12; }
    /* days since 1970-01-01 (proleptic Gregorian) */
    days = 365.0 * y + y / 4 - y / 100 + y / 400 + (153 * (m - 3) + 2) / 5 + st.wDay - 719469.0;
    return days * 86400000.0 + st.wHour * 3600000.0 + st.wMinute * 60000.0 + st.wSecond * 1000.0 + st.wMilliseconds;
}
int plat_tz_minutes(void) {
    TIME_ZONE_INFORMATION tz; DWORD r = GetTimeZoneInformation(&tz); long bias = tz.Bias;
    if (r == 2) bias += tz.DaylightBias; else if (r == 1) bias += tz.StandardBias;
    return (int)-bias;
}

/* ---- TCP (XMLSocket) through Windows Sockets 1.1 (NT 3.1 and Win95 ship WSOCK32) ---- */
#include <winsock.h>
#include "sock.h"
static int g_wsa_ok = -1;
static int wsa_init(void) { if (g_wsa_ok < 0) { WSADATA wd; g_wsa_ok = WSAStartup(MAKEWORD(1, 1), &wd) == 0; } return g_wsa_ok; }
int plat_tcp_connect(const char *host, int port) {
    struct sockaddr_in sa; struct hostent *he; SOCKET s; u_long nb = 1; unsigned long ip;
    if (!wsa_init() || port <= 0) return -1;
    memset(&sa, 0, sizeof(sa)); sa.sin_family = AF_INET; sa.sin_port = htons((u_short)port);
    ip = inet_addr(host);
    if (ip != INADDR_NONE) sa.sin_addr.s_addr = ip;
    else { he = gethostbyname(host); if (!he) return -1; memcpy(&sa.sin_addr, he->h_addr_list[0], 4); }
    s = socket(AF_INET, SOCK_STREAM, 0);
    if (s == INVALID_SOCKET) return -1;
    ioctlsocket(s, FIONBIO, &nb);
    if (connect(s, (struct sockaddr*)&sa, sizeof(sa)) == SOCKET_ERROR && WSAGetLastError() != WSAEWOULDBLOCK) { closesocket(s); return -1; }
    return (int)s;
}
int plat_tcp_poll(int fd, int *connected, int *readable) {
    fd_set w, r, e; struct timeval tv; tv.tv_sec = 0; tv.tv_usec = 0;
    FD_ZERO(&w); FD_ZERO(&r); FD_ZERO(&e); FD_SET((SOCKET)fd, &w); FD_SET((SOCKET)fd, &r); FD_SET((SOCKET)fd, &e);
    if (select(0, &r, &w, &e, &tv) == SOCKET_ERROR) return -1;
    if (FD_ISSET((SOCKET)fd, &e)) return -1;
    if (FD_ISSET((SOCKET)fd, &w) || FD_ISSET((SOCKET)fd, &r)) *connected = 1;
    *readable = FD_ISSET((SOCKET)fd, &r) ? 1 : 0;
    return 0;
}
int plat_tcp_send(int fd, const u8 *d, size_t n) { return send((SOCKET)fd, (const char*)d, (int)n, 0); }
int plat_tcp_recv(int fd, u8 *d, size_t n) { int k = recv((SOCKET)fd, (char*)d, (int)n, 0); if (k == SOCKET_ERROR) return -1; return k; }
void plat_tcp_close(int fd) { closesocket((SOCKET)fd); }

/* FileReference dialogs; filter is "description\0*.ext;*.ext2\0...\0" or NULL */
int plat_file_dialog(int save, const char *filter, const char *name, char *out, size_t cap) {
    OPENFILENAMEA ofn;
    memset(&ofn, 0, sizeof(ofn));
    strncpy(out, name ? name : "", cap - 1); out[cap - 1] = 0;
    ofn.lStructSize = 76;
    ofn.hwndOwner = g_hwnd;
    ofn.lpstrFilter = filter ? filter : "All Files (*.*)\0*.*\0\0";
    ofn.lpstrFile = out; ofn.nMaxFile = (DWORD)cap;
    ofn.Flags = OFN_HIDEREADONLY | OFN_PATHMUSTEXIST | (save ? OFN_OVERWRITEPROMPT : OFN_FILEMUSTEXIST);
    if ((GetVersion() & 0xFF) >= 4) ofn.Flags |= 0x00080000;
    return (save ? GetSaveFileNameA(&ofn) : GetOpenFileNameA(&ofn)) ? 1 : 0;
}
