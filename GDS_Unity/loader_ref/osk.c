/* osk.c -- gamepad on-screen keyboard for the kairo FEP text entry.
 *
 * Verbatim port of the approved Terraria NextOS controller keyboard
 * (ter_vkbd_* in terraria-nextos/src/main.c, itself ported from the
 * Prizefighters 2 port's controller keyboard): same QWERTY layout in a
 * 1280x720 design space, same move/activate/repeat logic, same latch that
 * swallows the pad until everything is released once after open, same
 * 5x7 run-length glyph renderer.  Only the platform seam differs:
 *   - Terraria stores text in its jni_softinput buffer; ours is owned here
 *     and returned through kairo's getResultInputPanel JNI (see jni.c).
 *   - Terraria draws with glScissor+glClear rects; ours reuses the
 *     game-proven cursor-overlay GL (gds_egl_overlay_*, egl_shim.c).
 * Controls (Terraria's, Y=space added; banner drawn at the bottom):
 *   dpad  move      A/R3  press key      B  backspace
 *   X     shift     Y     space          SELECT cancel    START done
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "gds.h"

/* egl_shim export: save-state begin, axis-aligned pixel rect, restore. */
extern int  gds_egl_overlay_begin(int *sw, int *sh);
extern void gds_egl_overlay_rect(float x, float y, float w, float h,
                                 float r, float g, float b);
extern void gds_egl_overlay_end(void);
extern int  egl_shim_screen_w(void);
extern int  egl_shim_screen_h(void);

enum { VK_CHARACTER, VK_BACKSPACE, VK_SHIFT, VK_SPACE, VK_DONE };

struct vk_key {
    int x, y, w, h;             /* design space: 1280x720 */
    const char *label;
    char lower, upper;
    int action;
};

/* QWERTY layout and interaction model ported from the approved
 * Prizefighters 2 controller keyboard (identical to Terraria's). */
static const struct vk_key vk_keys[] = {
  { 171,382, 86,52,"Q",'q','Q',VK_CHARACTER },
  { 265,382, 86,52,"W",'w','W',VK_CHARACTER },
  { 359,382, 86,52,"E",'e','E',VK_CHARACTER },
  { 453,382, 86,52,"R",'r','R',VK_CHARACTER },
  { 547,382, 86,52,"T",'t','T',VK_CHARACTER },
  { 641,382, 86,52,"Y",'y','Y',VK_CHARACTER },
  { 735,382, 86,52,"U",'u','U',VK_CHARACTER },
  { 829,382, 86,52,"I",'i','I',VK_CHARACTER },
  { 923,382, 86,52,"O",'o','O',VK_CHARACTER },
  {1017,382, 86,52,"P",'p','P',VK_CHARACTER },
  { 218,444, 86,52,"A",'a','A',VK_CHARACTER },
  { 312,444, 86,52,"S",'s','S',VK_CHARACTER },
  { 406,444, 86,52,"D",'d','D',VK_CHARACTER },
  { 500,444, 86,52,"F",'f','F',VK_CHARACTER },
  { 594,444, 86,52,"G",'g','G',VK_CHARACTER },
  { 688,444, 86,52,"H",'h','H',VK_CHARACTER },
  { 782,444, 86,52,"J",'j','J',VK_CHARACTER },
  { 876,444, 86,52,"K",'k','K',VK_CHARACTER },
  { 970,444, 86,52,"L",'l','L',VK_CHARACTER },
  { 265,506, 86,52,"Z",'z','Z',VK_CHARACTER },
  { 359,506, 86,52,"X",'x','X',VK_CHARACTER },
  { 453,506, 86,52,"C",'c','C',VK_CHARACTER },
  { 547,506, 86,52,"V",'v','V',VK_CHARACTER },
  { 641,506, 86,52,"B",'b','B',VK_CHARACTER },
  { 735,506, 86,52,"N",'n','N',VK_CHARACTER },
  { 829,506, 86,52,"M",'m','M',VK_CHARACTER },
  { 923,506,187,52,"DEL",0,0,VK_BACKSPACE },
  { 171,568,188,58,"SHIFT",0,0,VK_SHIFT },
  { 371,568,458,58,"SPACE",0,0,VK_SPACE },
  { 841,568,269,58,"DONE",0,0,VK_DONE },
};
#define VK_NKEYS ((int)(sizeof(vk_keys) / sizeof(vk_keys[0])))

static int vk_glyph(char c, unsigned char r[7]) {
    memset(r, 0, 7);
#define VG(a,b,c,d,e,f,g) do { \
        unsigned char v[7] = {a,b,c,d,e,f,g}; memcpy(r, v, 7); return 1; \
    } while (0)
    switch (c) {
    case 'A': VG(14,17,17,31,17,17,17); case 'B': VG(30,17,17,30,17,17,30);
    case 'C': VG(14,17,16,16,16,17,14); case 'D': VG(30,17,17,17,17,17,30);
    case 'E': VG(31,16,16,30,16,16,31); case 'F': VG(31,16,16,30,16,16,16);
    case 'G': VG(14,17,16,23,17,17,14); case 'H': VG(17,17,17,31,17,17,17);
    case 'I': VG(14,4,4,4,4,4,14);      case 'J': VG(7,2,2,2,18,18,12);
    case 'K': VG(17,18,20,24,20,18,17); case 'L': VG(16,16,16,16,16,16,31);
    case 'M': VG(17,27,21,21,17,17,17); case 'N': VG(17,25,21,19,17,17,17);
    case 'O': VG(14,17,17,17,17,17,14); case 'P': VG(30,17,17,30,16,16,16);
    case 'Q': VG(14,17,17,17,21,18,13); case 'R': VG(30,17,17,30,20,18,17);
    case 'S': VG(15,16,16,14,1,1,30);   case 'T': VG(31,4,4,4,4,4,4);
    case 'U': VG(17,17,17,17,17,17,14); case 'V': VG(17,17,17,17,17,10,4);
    case 'W': VG(17,17,17,21,21,21,10); case 'X': VG(17,17,10,4,10,17,17);
    case 'Y': VG(17,17,10,4,4,4,4);     case 'Z': VG(31,1,2,4,8,16,31);
    case 'a': VG(0,0,14,1,15,17,15);    case 'b': VG(16,16,30,17,17,17,30);
    case 'c': VG(0,0,14,17,16,17,14);   case 'd': VG(1,1,15,17,17,17,15);
    case 'e': VG(0,0,14,17,31,16,14);   case 'f': VG(6,9,8,28,8,8,8);
    case 'g': VG(0,0,15,17,15,1,14);    case 'h': VG(16,16,30,17,17,17,17);
    case 'i': VG(4,0,12,4,4,4,14);      case 'j': VG(2,0,6,2,2,18,12);
    case 'k': VG(16,16,18,20,24,20,18); case 'l': VG(12,4,4,4,4,4,14);
    case 'm': VG(0,0,26,21,21,17,17);   case 'n': VG(0,0,30,17,17,17,17);
    case 'o': VG(0,0,14,17,17,17,14);   case 'p': VG(0,0,30,17,30,16,16);
    case 'q': VG(0,0,15,17,15,1,1);     case 'r': VG(0,0,22,25,16,16,16);
    case 's': VG(0,0,15,16,14,1,30);    case 't': VG(8,8,28,8,8,9,6);
    case 'u': VG(0,0,17,17,17,19,13);   case 'v': VG(0,0,17,17,17,10,4);
    case 'w': VG(0,0,17,17,21,21,10);   case 'x': VG(0,0,17,10,4,10,17);
    case 'y': VG(0,0,17,17,15,1,14);    case 'z': VG(0,0,31,2,4,8,31);
    case '0': VG(14,17,19,21,25,17,14); case '1': VG(4,12,4,4,4,4,14);
    case '2': VG(14,17,1,2,4,8,31);     case '3': VG(30,1,1,14,1,1,30);
    case '4': VG(2,6,10,18,31,2,2);     case '5': VG(31,16,16,30,1,1,30);
    case '6': VG(14,16,16,30,17,17,14); case '7': VG(31,1,2,4,8,8,8);
    case '8': VG(14,17,17,14,17,17,14); case '9': VG(14,17,17,15,1,1,14);
    case '-': VG(0,0,0,31,0,0,0);       case '_': VG(0,0,0,0,0,0,31);
    case ':': VG(0,4,4,0,4,4,0);        case ' ': return 1;
    /* kairo dialog titles carry punctuation the Prizefighters font lacks. */
    case '.': VG(0,0,0,0,0,6,6);        case ',': VG(0,0,0,0,12,4,8);
    case '!': VG(4,4,4,4,4,0,4);        case '?': VG(14,17,1,6,4,0,4);
    case '\'': VG(4,4,4,0,0,0,0);       case '/': VG(1,1,2,4,8,16,16);
    }
    return 0;
#undef VG
}

/* ----------------------------------------------------------------- state */
static int  g_open, g_done, g_ok;
static char g_text[128];
static char g_title[96];
static int  g_maxlen = 16;
static int  g_sel, g_upper, g_latch;

/* 0.95.3: control bytes in user/game text can redraw a printed log line
 * over itself -- that is exactly how "Sunny Studios\r" kept turning our
 * own diagnostics into scrambled fragments.  Escape control bytes in any
 * string we print ("\x0d"). */
const char *gds_vis(const char *s, char *buf, size_t cap)
{
    size_t o = 0;
    for (const unsigned char *p = (const unsigned char *)s; *p && o + 6 < cap; p++) {
        if (*p >= 0x20) { buf[o++] = (char)*p; }
        else o += (size_t)snprintf(buf + o, cap - o, "\\x%02x", *p);
    }
    buf[o] = 0;
    return buf;
}

static void text_copy(char *dst, size_t cap, const char *src)
{
    if (!src)
        src = "";
    size_t n = strlen(src);
    if (n >= cap)
        n = cap - 1;
    memcpy(dst, src, n);
    dst[n] = 0;
}

/* ------------------------------------------------------------------- api */
void gds_osk_open(const char *title, const char *initial, int maxlen)
{
    if (maxlen > 0 && maxlen < (int)sizeof g_text)
        g_maxlen = maxlen;
    else
        g_maxlen = 16;
    if (!g_open) {
        g_open = 1;
        g_sel = 0;
        g_latch = 1;            /* swallow pad until full release once */
        if (title && title[0]) text_copy(g_title, sizeof g_title, title);
        else text_copy(g_title, sizeof g_title, "ENTER TEXT");
    }
    if (initial)
        text_copy(g_text, sizeof g_text, initial);
    size_t n = strlen(g_text);
    if (n > (size_t)g_maxlen) g_text[g_maxlen] = 0;
    /* 0.95.0 user report: a trailing blank at the end of the prefill was
     * STILL there after the ASCII-space trim shipped, and the interleaved
     * boot log (two threads racing stderr) couldn't prove what byte it is.
     * 0.95.1 (a) dumps the raw tail BYTES so the char is named outright,
     * and (b) extends the trim past ASCII space to the usual Unicode
     * blanks a Japanese game font would use -- the OSK bitmap font draws
     * every one of them as an empty gap + caret, exactly matching the
     * report.  PREFILL ONLY, never typed text. */
    n = strlen(g_text);
    const size_t rawlen = n;
    char tailhex[3 * 8 + 1];
    {
        const unsigned char *p = (const unsigned char *)g_text;
        size_t tn = n < 8 ? n : 8, ho = 0;
        for (size_t i = n - tn; i < n; i++)
            ho += (size_t)snprintf(tailhex + ho, sizeof tailhex - ho,
                                   "%02x ", p[i]);
        if (ho) tailhex[ho - 1] = 0;
        else tailhex[0] = 0;
    }
    size_t trimmed = 0;
    for (;;) {
        n = strlen(g_text);
        if (n == 0) break;
        unsigned char *u = (unsigned char *)g_text;
        /* 0.95.3: DEVICE-NAMED the "extra space" -- the rawtail bytes in
         * the 0.95.2 boot log were 53 74 75 64 69 6f 73 0d = "Studios" +
         * CR (0x0d).  Not a space: no amount of "space trim" could remove
         * it, the bitmap font drew it as an empty cell ("blank, then
         * cursor"), and printing it raw made the log line overwrite itself
         * (the real cause of every "interleave garble"). */
        if (u[n - 1] == ' ' || u[n - 1] < 0x20) {          /* blank/ctrl */
            u[--n] = 0; trimmed++; continue;
        }
        if (n >= 2 && u[n - 2] == 0xC2 && u[n - 1] == 0xA0) /* U+00A0 NBSP */
            { u[n -= 2] = 0; trimmed += 2; continue; }
        if (n >= 3 && u[n - 3] == 0xE3 && u[n - 2] == 0x80 &&
            u[n - 1] == 0x80)                                /* U+3000 ideo. */
            { u[n -= 3] = 0; trimmed += 3; continue; }
        if (n >= 3 && u[n - 3] == 0xE2 && u[n - 2] == 0x80 &&
            (u[n - 1] == 0x87 || u[n - 1] == 0xAF ||
             u[n - 1] == 0x8B))       /* figure / narrow-NBSP / zero-width */
            { u[n -= 3] = 0; trimmed += 3; continue; }
        break;
    }
    n = strlen(g_text);
    g_upper = n == 0 || g_text[n - 1] == ' ';
    g_done = 0;
    g_ok = 0;
    /* ONE flockfile'd line, text escaped so a control byte can no longer
     * redraw the log over itself (see the CR note above). */
    char vis_title[128], vis_text[160];
    flockfile(stderr);
    fprintf(stderr, "[osk] open title=\"%s\" rawlen=%zu rawtail=[%s] -> "
                    "text(%zu)=\"%s\" maxlen=%d%s\n",
            gds_vis(g_title, vis_title, sizeof vis_title),
            rawlen, tailhex, n,
            gds_vis(g_text, vis_text, sizeof vis_text), g_maxlen,
            trimmed ? " (trailing junk trimmed)" : "");
    fflush(stderr);
    funlockfile(stderr);
}

void gds_osk_set_text(const char *text)
{
    if (!g_open)
        return;
    text_copy(g_text, sizeof g_text, text);
    size_t n = strlen(g_text);
    if (n > (size_t)g_maxlen) g_text[g_maxlen] = 0;
    g_upper = n == 0 || g_text[n - 1] == ' ';
}

void gds_osk_hide(void)
{
    if (!g_open)
        return;
    g_open = 0;
    g_done = 1;
    g_ok = 0;
    fprintf(stderr, "[osk] external hide -> cancel\n");
    fflush(stderr);
}

int gds_osk_active(void) { return g_open; }
int gds_osk_done(void) { return g_done; }
int gds_osk_result_ok(void) { return g_ok; }
const char *gds_osk_text(void) { return g_text; }

/* ------------------------------------------------------------ edit ops */
static void vk_append_char(char c)
{
    size_t n = strlen(g_text);
    if (n < (size_t)g_maxlen) {
        g_text[n] = c;
        g_text[n + 1] = 0;
        g_upper = c == ' ';
    }
}

static void vk_backspace(void)
{
    size_t n = strlen(g_text);
    if (n > 0) {
        g_text[n - 1] = 0;
        n--;
        g_upper = n == 0 || g_text[n - 1] == ' ';
    }
}

static void vk_commit(void)
{
    g_open = 0;
    g_done = 1;
    g_ok = 1;
    char vis[160];
    flockfile(stderr);
    fprintf(stderr, "[osk] DONE text=\"%s\"\n",
            gds_vis(g_text, vis, sizeof vis));
    fflush(stderr);
    funlockfile(stderr);
}

static void vk_cancel(void)
{
    g_open = 0;
    g_done = 1;
    g_ok = 0;
    fprintf(stderr, "[osk] CANCEL text=\"%s\"\n", g_text);
    fflush(stderr);
}

static void vk_move_selection(int dx, int dy)
{
    const struct vk_key *current = &vk_keys[g_sel];
    int current_x = current->x + current->w / 2;
    int current_y = current->y + current->h / 2;
    int best = -1, best_score = INT32_MAX;
    for (int i = 0; i < VK_NKEYS; i++) {
        if (i == g_sel) continue;
        const struct vk_key *candidate = &vk_keys[i];
        int candidate_x = candidate->x + candidate->w / 2;
        int candidate_y = candidate->y + candidate->h / 2;
        int along = dx ? (candidate_x - current_x) * dx
                       : (candidate_y - current_y) * dy;
        if (along <= 0) continue;
        int across = dx ? abs(candidate_y - current_y)
                        : abs(candidate_x - current_x);
        int score = dx ? along + across * 8 : along * 4 + across;
        if (score < best_score) { best_score = score; best = i; }
    }
    if (best >= 0) g_sel = best;
}

static void vk_activate_key(int index)
{
    if (index < 0 || index >= VK_NKEYS) return;
    const struct vk_key *key = &vk_keys[index];
    switch (key->action) {
    case VK_CHARACTER: vk_append_char(g_upper ? key->upper : key->lower); break;
    case VK_BACKSPACE: vk_backspace(); break;
    case VK_SHIFT: g_upper = !g_upper; break;
    case VK_SPACE: vk_append_char(' '); break;
    case VK_DONE: vk_commit(); break;
    }
}

/* Pad arrays are indexed by the shared NPB_ enum (gds.h); Terraria's
 * native_pad uses the identical slot order, so the edge logic below is a
 * 1:1 port.  Runs once per frame from gds_input_poll (input.c). */
void gds_osk_pad_tick(const unsigned char *cur, const unsigned char *prev)
{
    static int rep;
    if (!g_open) { rep = 0; return; }
#define BTN(i)   (cur[i])
#define BTNDN(i) (cur[i] && !prev[i])
    if (g_latch) {
        if (!BTN(NPB_A) && !BTN(NPB_B) && !BTN(NPB_X) && !BTN(NPB_BACK) &&
            !BTN(NPB_START) && !BTN(NPB_R3) && !BTN(NPB_DU) && !BTN(NPB_DD) &&
            !BTN(NPB_DL) && !BTN(NPB_DR))
            g_latch = 0;
        return;
    }
    int dx = BTN(NPB_DR) ? 1 : (BTN(NPB_DL) ? -1 : 0);
    int dy = BTN(NPB_DD) ? 1 : (BTN(NPB_DU) ? -1 : 0);
    int edge_move = BTNDN(NPB_DU) || BTNDN(NPB_DD) || BTNDN(NPB_DL) || BTNDN(NPB_DR);
    if ((dx || dy) && (edge_move || rep <= 0)) {
        if (dx) vk_move_selection(dx, 0);
        if (dy) vk_move_selection(0, dy);
        rep = edge_move ? 14 : 8;
    }
    if (rep > 0) rep--;
    if (BTNDN(NPB_A) || BTNDN(NPB_R3)) vk_activate_key(g_sel);
    if (BTNDN(NPB_B)) vk_backspace();
    if (BTNDN(NPB_X)) g_upper = !g_upper;
    if (BTNDN(NPB_Y)) vk_append_char(' ');
    if (BTNDN(NPB_BACK)) vk_cancel();
    if (BTNDN(NPB_START)) vk_commit();
#undef BTN
#undef BTNDN
}

/* ----------------------------------------------------------------- draw */
static void design_rect(int sw, int sh, int x, int y, int w, int h,
                        float r, float g, float b)
{
    float left = (float)(x * sw / 1280);
    float right = (float)((x + w) * sw / 1280);
    float top = (float)(y * sh / 720);
    float bottom = (float)((y + h) * sh / 720);
    gds_egl_overlay_rect(left, top, right - left, bottom - top, r, g, b);
}

static void vk_text(int sw, int sh, int x, int y, const char *s, int scale,
                    float r, float g, float b)
{
    for (int n = 0; s && s[n]; n++) {
        unsigned char rows[7];
        if (!vk_glyph(s[n], rows)) continue;
        for (int yy = 0; yy < 7; yy++) {
            int xx = 0;
            while (xx < 5) {
                while (xx < 5 && !(rows[yy] & (1 << (4 - xx)))) xx++;
                int start = xx;
                while (xx < 5 && (rows[yy] & (1 << (4 - xx)))) xx++;
                if (start < xx)
                    design_rect(sw, sh, x + n * 6 * scale + start * scale,
                                y + yy * scale, (xx - start) * scale, scale,
                                r, g, b);
            }
        }
    }
}

static int vk_text_width(const char *text, int scale)
{
    return text ? (int)strlen(text) * 6 * scale : 0;
}

void gds_osk_draw(void)
{
    if (!g_open)
        return;
    int sw = 0, sh = 0;
    if (!gds_egl_overlay_begin(&sw, &sh)) {
        static int logged;
        if (!logged) {
            logged = 1;
            fprintf(stderr, "[osk] overlay GL unavailable -- keyboard invisible!\n");
            fflush(stderr);
        }
        return;
    }
    /* Terraria's midnight-blue/warm-gold palette, verbatim. */
    design_rect(sw, sh, 132, 268, 1016, 452, 0.018f, 0.025f, 0.055f);
    design_rect(sw, sh, 140, 276, 1000, 444, 0.44f, 0.29f, 0.075f);
    design_rect(sw, sh, 148, 284, 984, 436, 0.035f, 0.070f, 0.155f);
    /* dialog title from the kairo fepTitle_ (falls back to ENTER TEXT) */
    char title[97];
    {
        size_t i, n = strlen(g_title);
        if (n > 20) n = 20;
        for (i = 0; i < n; i++) {
            char c = g_title[i];
            title[i] = (c >= 'a' && c <= 'z') ? (char)(c - 32) : c;
        }
        title[i] = 0;
    }
    vk_text(sw, sh, 171, 290, title, 2, 0.98f, 0.88f, 0.48f);
    design_rect(sw, sh, 167, 306, 947, 62, 0.66f, 0.46f, 0.11f);
    design_rect(sw, sh, 173, 312, 935, 50, 0.025f, 0.055f, 0.125f);
    char shown[130];
    /* keep the tail visible for long entries (scale 4 = 24px/char) */
    const char *t = g_text;
    size_t tl = strlen(t);
    if ((int)tl * 24 > 900)
        t += tl - (900 / 24);
    /* keep the caret: the "no cursor" wish was about the MOUSE pointer,
     * not this text caret -- and the real trailing space turned out to be
     * in the prefill text itself (handled at open above). */
    snprintf(shown, sizeof shown, "%s_", t);
    vk_text(sw, sh, 190, 323, shown, 4, 0.98f, 0.91f, 0.58f);
    for (int i = 0; i < VK_NKEYS; i++) {
        const struct vk_key *key = &vk_keys[i];
        char dynamic_label[8];
        const char *label = key->label;
        if (key->action == VK_CHARACTER) {
            dynamic_label[0] = g_upper ? key->upper : key->lower;
            dynamic_label[1] = 0;
            label = dynamic_label;
        } else if (key->action == VK_SHIFT) {
            snprintf(dynamic_label, sizeof dynamic_label, "%s",
                     g_upper ? "UPPER" : "LOWER");
            label = dynamic_label;
        }
        int highlighted = i == g_sel ||
                          (key->action == VK_SHIFT && g_upper);
        design_rect(sw, sh, key->x - 3, key->y - 3, key->w + 6, key->h + 6,
                    highlighted ? 1.00f : 0.34f, highlighted ? 0.78f : 0.23f,
                    highlighted ? 0.18f : 0.065f);
        design_rect(sw, sh, key->x, key->y, key->w, key->h,
                    highlighted ? 0.18f : 0.085f, highlighted ? 0.34f : 0.17f,
                    highlighted ? 0.62f : 0.34f);
        int scale = (int)strlen(label) > 3 ? 2 : 3;
        int tx = key->x + (key->w - vk_text_width(label, scale)) / 2;
        int ty = key->y + (key->h - 7 * scale) / 2;
        vk_text(sw, sh, tx, ty, label, scale, 0.98f, 0.91f, 0.58f);
    }
    vk_text(sw, sh, 171, 650, "A SELECT  B DELETE  X SHIFT  START DONE", 2,
            0.82f, 0.86f, 0.94f);
    gds_egl_overlay_end();
}
