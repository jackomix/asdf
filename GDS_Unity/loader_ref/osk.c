/* osk.c -- gamepad on-screen keyboard for the kairo FEP text entry.
 *
 * 0.95.10 v3 (user device-tested 0.95.9 and sent another round):
 *   - Selection is a FOCUS RING again (2px, fully enclosing key + drop
 *     shadow -- 0.95.9's ring ended under the face but above the shadow,
 *     exactly the "outline wraps the sides of the shadow, not underneath"
 *     the user reported) + a 10% white face lift.  White is no longer
 *     spent on selection, so...
 *   - CAPS LOCK turns the Shift key WHITE with dark text (user: "maybe
 *     for the caps button, it can turn white"); one-shot = lit gray.
 *   - Symbols page is FLAT: all 32 ASCII punct glyphs, 4x8 grid filling
 *     the space (user: "barely anything there").  Shift pairs moved to
 *     the LETTERS page digits + bottom-row punct, like a real keyboard
 *     (user's exact suggestion).  One-shot applies the whole layer;
 *     caps uppercases letters only, like a physical CAPS LOCK.
 *   - START commits, SELECT cancels -- they finally DO something: the
 *     input path itself was fixed in input.c (evdev watcher shadow
 *     merge; SDL never surfaced these two keys on this device).
 *   - SELECT-cancel still appears only when the prompt offers a negative
 *     label (dex-verified), but the pill moved from the "ugly, lonely"
 *     bottom band to the title band next to n/max.
 *   - Panel nudged down 8px -> truly centred; title + counter share one
 *     vertical centre; scrim 0.68 -> 0.74 ("a tad darker").
 *   - All badges unified: pills are light-face + dark text just like the
 *     ABXY circles (were dark + light).  B badge on Del tucked further
 *     into the corner, Del label nudged down/right (small-key kiss).
 *   - Max-length error: counter CUTS to red, fades back, shake is
 *     longer + wider (user: "not as noticeable ... cut to red then fade
 *     back").  Red is the single sanctioned colour exception.
 * The classic Terraria keyboard stays: GDS_OSK=classic forces it, and it
 * is also the automatic fallback when the atlas file or GL path is gone.
 *
 * Controls (new style):
 *   dpad move   A/R3 press key    B backspace   X shift cycle   Y space
 *   L1/R1 text caret left/right   SELECT cancel (when offered)   START done
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "gds.h"
#include "osk_font_data.h"
#include "osk_layout.h"
#include <math.h>               /* sqrtf/floorf: AA badge edges (0.95.11) */

/* egl_shim exports: save-state begin, pixel rects/text, restore. */
extern int  gds_egl_overlay_begin(int *sw, int *sh);
extern void gds_egl_overlay_rect(float x, float y, float w, float h,
                                 float r, float g, float b);
extern void gds_egl_overlay_rect_a(float x, float y, float w, float h,
                                   float r, float g, float b, float a);
extern int  gds_egl_overlay_atlas(const unsigned char *rgba, int w, int h);
extern void gds_egl_overlay_quads(const float *verts, int nquads,
                                  float r, float g, float b);
extern void gds_egl_overlay_end(void);
extern int  egl_shim_screen_w(void);
extern int  egl_shim_screen_h(void);

/* ---------------------------------------------------------------- shared */
static int  g_open, g_done, g_ok;
static char g_text[128];
static char g_title[96];
static int  g_maxlen = 16;
static int  g_latch;
static int  g_caret;            /* byte index 0..strlen(g_text) */

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

/* ------------------------------------------------------------ edit ops */
#define OVK_SHAKE_FRAMES 22     /* maxlen trip: shake + red flash length */
static int  g_shake;            /* >0: counter shake/flash frames left */

/* caret-based insert (classic keeps the caret pinned at end, so for it
 * this is exactly the old append-at-end behaviour).  Returns 0 when full. */
static int insert_char(char c)
{
    size_t n = strlen(g_text);
    if ((int)n >= g_maxlen) { g_shake = OVK_SHAKE_FRAMES; return 0; }
    memmove(g_text + g_caret + 1, g_text + g_caret, n - (size_t)g_caret + 1);
    g_text[g_caret] = c;
    g_caret++;
    return 1;
}

static int vk_backspace(void)
{
    if (g_caret <= 0) return 0;
    memmove(g_text + g_caret - 1, g_text + g_caret,
            strlen(g_text) - (size_t)g_caret + 1);
    g_caret--;
    return 1;
}

/* classic call sites use the old names */
static void vk_append_char(char c) { (void)insert_char(c); }

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
    char vis[160];
    fprintf(stderr, "[osk] CANCEL text=\"%s\"\n",
            gds_vis(g_text, vis, sizeof vis));
    fflush(stderr);
}

/* ================================================================ CLASSIC
 * The approved Terraria NextOS controller keyboard
 * (ter_vkbd_* in terraria-nextos/src/main.c, itself ported from the
 * Prizefighters 2 port's controller keyboard): QWERTY layout in a 1280x720
 * design space, move/activate/repeat logic, 5x7 run-length glyphs.  Kept
 * byte-for-byte behavior: GDS_OSK=classic or automatic fallback. */

enum { VK_CHARACTER, VK_BACKSPACE, VK_SHIFT, VK_SPACE, VK_DONE };

struct vk_key {
    int x, y, w, h;             /* design space: 1280x720 */
    const char *label;
    char lower, upper;
    int action;
};

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
    case '.': VG(0,0,0,0,0,6,6);        case ',': VG(0,0,0,0,12,4,8);
    case '!': VG(4,4,4,4,4,0,4);        case '?': VG(14,17,1,6,4,0,4);
    case '\'': VG(4,4,4,0,0,0,0);       case '/': VG(1,1,2,4,8,16,16);
    }
    return 0;
#undef VG
}

static int  g_csel, g_upper;

static void classic_move_selection(int dx, int dy)
{
    const struct vk_key *current = &vk_keys[g_csel];
    int current_x = current->x + current->w / 2;
    int current_y = current->y + current->h / 2;
    int best = -1, best_score = INT32_MAX;
    for (int i = 0; i < VK_NKEYS; i++) {
        if (i == g_csel) continue;
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
    if (best >= 0) g_csel = best;
}

static void classic_reflect_case(void)
{
    /* classic's smart-case: caps when empty or after a space */
    size_t n = strlen(g_text);
    g_upper = n == 0 || g_text[n - 1] == ' ';
}

static void classic_activate_key(int index)
{
    if (index < 0 || index >= VK_NKEYS) return;
    const struct vk_key *key = &vk_keys[index];
    switch (key->action) {
    case VK_CHARACTER:
        vk_append_char(g_upper ? key->upper : key->lower);
        classic_reflect_case();
        break;
    case VK_BACKSPACE: vk_backspace(); classic_reflect_case(); break;
    case VK_SHIFT: g_upper = !g_upper; break;
    case VK_SPACE: vk_append_char(' '); g_upper = 1; break;
    case VK_DONE: vk_commit(); break;
    }
}

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

static void classic_draw(void)
{
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
    const char *t = g_text;
    size_t tl = strlen(t);
    if ((int)tl * 24 > 900)
        t += tl - (900 / 24);
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
        int highlighted = i == g_csel ||
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

/* =================================================================== NEW
 * Generated atlas text over the same overlay GL seam.  Everything visual
 * comes from osk_layout.h (tools/make_osk_font.py). */

static int  g_style = -1;       /* -1 undecided, 0 new, 1 classic */
static int  g_sel, g_shift;     /* shift: 0 off, 1 one-shot, 2 caps lock */
static int  g_page;             /* 0 letters, 1 symbols */
static int  g_home;             /* index of the 'g' key = open position */
static char g_negative[16];     /* prompt's cancel label ("" = not offered) */

/* combined per-page tables (page keys + shared function row), built once */
static ovk_key_t g_tab_letters[OVK_N_LETTERS + OVK_N_FUNC];
static ovk_key_t g_tab_symbols[OVK_N_SYMBOLS + OVK_N_FUNC];
static int g_tab_n[2] = { OVK_N_LETTERS + OVK_N_FUNC,
                          OVK_N_SYMBOLS + OVK_N_FUNC };
static int g_tabs_built;

static void build_tables(void)
{
    if (g_tabs_built) return;
    g_tabs_built = 1;
    memcpy(g_tab_letters, ovk_letters, sizeof ovk_letters);
    memcpy(g_tab_letters + OVK_N_LETTERS, ovk_func, sizeof ovk_func);
    memcpy(g_tab_symbols, ovk_symbols, sizeof ovk_symbols);
    memcpy(g_tab_symbols + OVK_N_SYMBOLS, ovk_func, sizeof ovk_func);
    g_home = 0;
    for (int i = 0; i < OVK_N_LETTERS; i++)
        if (g_tab_letters[i].act == OVKA_CHAR && g_tab_letters[i].lo == 'g')
            { g_home = i; break; }
}

static const ovk_key_t *page_table(void)
{
    return g_page ? g_tab_symbols : g_tab_letters;
}

static void style_decide(void)
{
    if (g_style >= 0) return;
    const char *e = getenv("GDS_OSK");
    g_style = (e && !strcmp(e, "classic")) ? 1 : 0;
    build_tables();
}

/* ---- atlas file (written next to loader2 by tools/make_osk_font.py) ---- */
static unsigned char *g_font;
static int g_font_tried;
static const unsigned char *font_rgba(void)
{
    if (g_font) return g_font;
    if (g_font_tried) return NULL;
    g_font_tried = 1;
    char path[1200];
    snprintf(path, sizeof path, "%s/osk_font.rgba", gds_gamedir);
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "[osk] %s missing -> classic keyboard\n", path);
        fflush(stderr);
        return NULL;
    }
    unsigned char *buf = (unsigned char *)malloc((size_t)OVK_AW * OVK_AH * 4);
    size_t rd = buf ? fread(buf, 1, (size_t)OVK_AW * OVK_AH * 4, f) : 0;
    fclose(f);
    if (rd != (size_t)OVK_AW * OVK_AH * 4) {
        free(buf);
        fprintf(stderr, "[osk] %s short read (%zu) -> classic keyboard\n",
                path, rd);
        fflush(stderr);
        return NULL;
    }
    g_font = buf;
    return g_font;
}

/* ---- quad batching: one bucket per text color, flushed after the rects */
enum { QB_TEXT, QB_TITLE, QB_DIM, QB_CNT, QB_BADGED, QB_BADGEL, QB_COUNT };
#define QB_CAP (512 * 24)       /* floats per bucket (512 quads) */
static float g_qb[QB_COUNT][QB_CAP];
static int   g_qn[QB_COUNT];
static const float *g_qb_col[QB_COUNT];
static float g_cnt_col[3];      /* QB_CNT color: dim gray -> CUTS to red on
                                 * maxlen, fades back (user-sanctioned) */

static void qb_reset(void)
{
    memset(g_qn, 0, sizeof g_qn);
    static const float cols[QB_COUNT][3] = {
        { OVKC_TEXT_C }, { OVKC_TITLE_C }, { OVKC_DIM_C },
        { OVKC_DIM_C }, { OVKC_BADGE_TEXT }, { OVKC_PILL_TEXT },
    };
    for (int i = 0; i < QB_COUNT; i++) g_qb_col[i] = cols[i];
    g_qb_col[QB_CNT] = g_cnt_col;   /* per-frame, set by the title block */
}

static void qput(int bucket, float x, float y, float w, float h,
                 float u0, float v0, float u1, float v1)
{
    if (g_qn[bucket] + 24 > QB_CAP) return;
    float *q = g_qb[bucket] + g_qn[bucket];
    g_qn[bucket] += 24;
    q[0] = x;     q[1] = y;     q[2] = u0;  q[3] = v0;
    q[4] = x + w; q[5] = y;     q[6] = u1;  q[7] = v0;
    q[8] = x;     q[9] = y + h; q[10] = u0; q[11] = v1;
    q[12] = x + w; q[13] = y;   q[14] = u1; q[15] = v0;
    q[16] = x + w; q[17] = y + h; q[18] = u1; q[19] = v1;
    q[20] = x;    q[21] = y + h; q[22] = u0; q[23] = v1;
}

/* glyph advance within the atlas' metrics */
static float ovk_advw(unsigned char c)
{
    if (c == ' ') return OVK_SPACE_W;
    if (c < 0x21 || c > 0x7E) c = '?';
    return ovk_adv[c - 0x20];
}

static float ovk_textw_n(const char *s, size_t n, float sc)
{
    float w = 0;
    for (size_t i = 0; s && i < n && s[i]; i++)
        w += ovk_advw((unsigned char)s[i]) * sc;
    return w;
}

static float ovk_textw(const char *s, float sc)
{
    return ovk_textw_n(s, s ? strlen(s) : 0, sc);
}

/* emit glyphs whose quads land in `bucket`; pen/baseline are UNSCALED
 * design-space pixels -- the sxsy scale is applied here. */
static float g_sx = 1.0f, g_sy = 1.0f;
static float ovk_emit(int bucket, float pen, float baseline, const char *s,
                      float sc)
{
    for (; s && *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == ' ') { pen += OVK_SPACE_W * sc; continue; }
        if (c < 0x21 || c > 0x7E) c = '?';
        int idx = c - 0x20;
        int col = idx % OVK_COLS, row = idx / OVK_COLS;
        float u0 = (col * OVK_CELL + 0.5f) / OVK_AW;
        float v0 = (row * OVK_CELL + 0.5f) / OVK_AH;
        float u1 = ((col + 1) * OVK_CELL - 0.5f) / OVK_AW;
        float v1 = ((row + 1) * OVK_CELL - 0.5f) / OVK_AH;
        float x = pen;
        float y = baseline - OVK_ASC_CELL * sc;   /* cell top */
        qput(bucket, x * g_sx, y * g_sy, OVK_CELL * sc * g_sx,
             OVK_CELL * sc * g_sy, u0, v0, u1, v1);
        pen += ovk_advw(c) * sc;
    }
    return pen;
}

/* emit but stop at clipx (right edge of the text box) */
static float ovk_emit_clip(int bucket, float pen, float baseline,
                           const char *s, float sc, float clipx)
{
    for (; s && *s && pen < clipx; s++)
        pen = ovk_emit(bucket, pen, baseline, (char[]) { *s, 0 }, sc);
    return pen;
}

/* rect in 640x480 design space -> current drawable */
static void RR(float x, float y, float w, float h, float r, float g, float b)
{
    gds_egl_overlay_rect(x * g_sx, y * g_sy, w * g_sx, h * g_sy, r, g, b);
}

static void RRA(float x, float y, float w, float h,
                float r, float g, float b, float a)
{
    gds_egl_overlay_rect_a(x * g_sx, y * g_sy, w * g_sx, h * g_sy, r, g, b, a);
}

static void RR3(float x, float y, float w, float h, const float *col)
{
    RR(x, y, w, h, col[0], col[1], col[2]);
}

/* baseline helpers: place text by its visual top, or center the cell box
 * inside a key. */
static float ovk_baseline_top(float y_top, float sc)
{
    return y_top + (OVK_ASC_CELL - 3.0f) * sc;   /* 3 = generator top_pad */
}
static float ovk_baseline_cell(int y, int h, float sc)
{
    return y + (h - OVK_CELL * sc) * 0.5f + OVK_ASC_CELL * sc;
}
/* vertical-centre the CELL box on cy -- lets two text runs of different
 * scale share one optical centre line (v3: title and n/max counter). */
static float ovk_baseline_cy(float cy, float sc)
{
    return ovk_baseline_cell((int)(cy - 17.0f), 34, sc);
}

/* ---- badge shapes, anti-aliased (0.95.11) ---- */
/* integer-scanline circles/pills stepped visibly at 640x480 -- the user
 * saw exactly that ("the button icons look kind of pixelated").  Edge
 * pixels now carry real coverage: solid middle run via RR, the two
 * fractional edge pixels blended via RRA, shape tips one soft pixel. */
static void hline_cov(float xa, float xb, float y, const float *col)
{
    if (xb - xa < 1.6f) {           /* tip sliver: a single soft pixel */
        float xm = (xa + xb) * 0.5f;
        RRA(xm - 0.5f, y, 1.0f, 1.0f, col[0], col[1], col[2],
            (xb - xa) * 0.45f);
        return;
    }
    float lf = floorf(xa), rf = floorf(xb);
    float covl = lf + 1.0f - xa, covr = xb - rf;
    if (covl >= 0.98f && covr >= 0.98f) {          /* edges ~full */
        RR3(lf, y, rf - lf, 1.0f, col);
        return;
    }
    RRA(lf, y, 1.0f, 1.0f, col[0], col[1], col[2], covl);
    if (rf - lf > 1.0f)
        RR3(lf + 1.0f, y, rf - lf - 1.0f, 1.0f, col);
    RRA(rf, y, 1.0f, 1.0f, col[0], col[1], col[2], covr);
}

static void disc(float cx, float cy, float r, const float *col)
{
    for (int di = -10; di <= 10; di++) {
        float d2 = r * r - (float)(di * di);
        if (d2 < 1.0f) continue;                   /* past the shape */
        float hw = sqrtf(d2);
        hline_cov(cx - hw, cx + hw, cy + (float)di, col);
    }
}

static void rrect(float x, float y, float w, float h, const float *col)
{
    float rc = h * 0.5f;
    for (int dy = 0; dy < (int)h; dy++) {
        float yc = (float)dy + 0.5f;               /* row centre sample */
        float inset = 0.0f;
        if (yc < rc) {
            float d = rc - yc;
            inset = rc - sqrtf(rc * rc - d * d);
        } else if (yc > h - rc) {
            float d = yc - (h - rc);
            inset = rc - sqrtf(rc * rc - d * d);
        }
        hline_cov(x + inset, x + w - inset, y + (float)dy, col);
    }
}

/* face-button circle: light disc + dark letter on normal keys, dark disc
 * + light letter on a BRIGHT key face (caps-lock white) so it still reads.
 * Selection alone no longer flips it (v3: badges look identical on every
 * key -- user caught the old dark pill / light circle mismatch). */
static void badge_circle(float cx, float cy, char letter, int bright_key)
{
    static const float face_l[3] = { OVKC_BADGE_FACE };
    static const float face_d[3] = { OVKC_FN_FACE };
    char s[2] = { letter, 0 };
    disc(cx, cy, 8.5f, bright_key ? face_d : face_l);   /* AA disc */
    float sc = 0.46f;
    float lw = ovk_textw(s, sc);
    int bucket = bright_key ? QB_BADGEL : QB_BADGED;
    ovk_emit(bucket, cx - lw * 0.5f,
             ovk_baseline_cell((int)(cy - 8), 16, sc), s, sc);
}

/* shoulder/start pill: light face + dark text, same as the circles */
static void badge_pill(float x, float y, float w, float h, const char *txt,
                       float tsc)
{
    static const float face[3] = { OVKC_PILL_FACE };
    static const float edge[3] = { OVKC_PILL_EDGE };
    rrect(x, y, w, h, edge);
    rrect(x + 1, y + 1, w - 2, h - 2, face);
    float tw = ovk_textw(txt, tsc);
    ovk_emit(QB_BADGED, x + (w - tw) * 0.5f,
             ovk_baseline_cell((int)y, (int)h, tsc), txt, tsc);
}

/* ---------------------------------------------------------- interaction */
/* letters grid is 4x10, symbols 4x8: flip keeps the ROW, clamps the col
 * (fn row maps to fn row exactly, both tables carry it at the tail). */
static void page_flip(void)
{
    int idx = g_sel;
    if (g_page == 0) {
        g_page = 1;
        if (idx >= OVK_N_LETTERS)
            g_sel = OVK_N_SYMBOLS + (idx - OVK_N_LETTERS);
        else {
            int r = idx / 10, c = idx % 10;
            g_sel = r * 8 + (c > 7 ? 7 : c);
        }
    } else {
        g_page = 0;
        if (idx >= OVK_N_SYMBOLS)
            g_sel = OVK_N_LETTERS + (idx - OVK_N_SYMBOLS);
        else {
            int r = idx / 8, c = idx % 8;
            g_sel = r * 10 + c;
        }
    }
    if (g_sel >= g_tab_n[g_page]) g_sel = g_tab_n[g_page] - 1;
}

/* v3 shift layers, like a physical keyboard: one-shot shift applies the
 * FULL layer (digits -> !@#..., punct -> partners, letters uppercase);
 * CAPS LOCK uppercases LETTERS ONLY -- real Caps Lock never touches the
 * number row, so neither does ours. */
static char shifted_char(const ovk_key_t *k, int shift)
{
    if (shift == 1) return k->hi;
    if (shift == 2 && k->lo >= 'a' && k->lo <= 'z') return k->hi;
    return k->lo;
}

static void new_activate(const ovk_key_t *k)
{
    switch (k->act) {
    case OVKA_CHAR: {
        char c = shifted_char(k, g_shift);
        if (insert_char(c) && g_shift == 1)
            g_shift = 0;                    /* one-shot spent by a char */
        break;
    }
    case OVKA_SHIFT:
        g_shift = (g_shift + 1) % 3;        /* off -> 1-shot -> lock -> off */
        break;
    case OVKA_SYM:
        page_flip();
        break;
    case OVKA_SPACE:
        insert_char(' ');
        break;
    case OVKA_BKSP:
        vk_backspace();                     /* never touches shift */
        break;
    case OVKA_DONE:
        vk_commit();
        break;
    }
}

static void new_move_selection(int dx, int dy)
{
    const ovk_key_t *tab = page_table();
    int n = g_tab_n[g_page];
    int current_x = tab[g_sel].x + tab[g_sel].w / 2;
    int current_y = tab[g_sel].y + tab[g_sel].h / 2;
    int best = -1, best_score = INT32_MAX;
    for (int i = 0; i < n; i++) {
        if (i == g_sel) continue;
        int candidate_x = tab[i].x + tab[i].w / 2;
        int candidate_y = tab[i].y + tab[i].h / 2;
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

/* ---------------------------------------------------------------- draw */
/* the only bright key face v3 has: caps-lock white (selection is a ring
 * now, not an inversion).  Dark text sits on bright faces. */
static int bright_face(const ovk_key_t *k)
{
    return k->act == OVKA_SHIFT && g_shift == 2;
}

static void draw_key_label(const ovk_key_t *k)
{
    int bucket = bright_face(k) ? QB_BADGED : QB_TEXT;
    if (k->act == OVKA_CHAR) {
        char big = shifted_char(k, g_shift);
        char s[2] = { big, 0 };
        float lsc = 0.85f;
        float lw = ovk_textw(s, lsc);
        ovk_emit(bucket, (float)k->x + ((float)k->w - lw) * 0.5f,
                 ovk_baseline_cell(k->y, k->h, lsc), s, lsc);
        /* physical pairing: the paired key shows its OTHER-layer glyph
         * small at the top-left (digits + bottom-row punct on the
         * letters page; the flat symbols page has lo==hi, letters show
         * nothing -- physical keyboards don't print a small 'A' on 'a'
         * either). */
        if (k->lo != k->hi && !(k->lo >= 'a' && k->lo <= 'z')) {
            char sm[2] = { big == k->lo ? k->hi : k->lo, 0 };
            float ssc = 0.45f;
            ovk_emit(QB_DIM, (float)k->x + 6.0f,
                     ovk_baseline_top((float)k->y + 4.0f, ssc), sm, ssc);
        }
    } else {
        const char *label = k->label;
        if (k->act == OVKA_SHIFT) label = g_shift == 2 ? "Caps" : "Shift";
        else if (k->act == OVKA_SYM) label = g_page ? "ABC" : "#+=";
        float lsc = 0.66f;
        while (lsc > 0.40f && ovk_textw(label, lsc) > k->w - 14)
            lsc -= 0.02f;
        float lw = ovk_textw(label, lsc);
        /* Del is a 1u key that also carries the B circle badge: nudge its
         * label down+right so the glyph clears the badge corner. */
        int yoff = k->act == OVKA_BKSP ? 3 : 0;
        int xoff = k->act == OVKA_BKSP ? 2 : 0;
        ovk_emit(bucket, (float)k->x + xoff + ((float)k->w - lw) * 0.5f,
                 ovk_baseline_cell(k->y + yoff, k->h, lsc), label, lsc);
    }
}

static void new_draw(void)
{
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
    const unsigned char *rgba = font_rgba();
    if (!rgba || !gds_egl_overlay_atlas(rgba, OVK_AW, OVK_AH)) {
        static int logged;
        if (!logged) {
            logged = 1;
            fprintf(stderr, "[osk] font atlas/GL unavailable -> classic keyboard\n");
            fflush(stderr);
        }
        gds_egl_overlay_end();
        g_style = 1;
        classic_draw();
        return;
    }
    g_sx = (float)sw / 640.0f;
    g_sy = (float)sh / 480.0f;
    qb_reset();

    static const int pnl[4] = { OVK_PANEL };
    static const int box[4] = { OVK_BOX };
    static const int tpos[2] = { OVK_TITLE_POS };
    int px = pnl[0], py = pnl[1], pw = pnl[2], ph = pnl[3];

    /* scrim: dim the game behind (0.68 -> 0.74: "a tad bit darker") */
    RRA(0, 0, 640, 480, 0.0f, 0.0f, 0.0f, 0.74f);

    /* panel: SMOOTH vertical gradient (user: "isn't really a gradient") */
    {
        static const float top[3] = { OVKC_PANEL_TOP };
        static const float bot[3] = { OVKC_PANEL_BOT };
        for (int i = 0; i < ph; i++) {
            float t = (float)i / (float)(ph - 1);
            RR((float)px, (float)(py + i), (float)pw, 1.0f,
               top[0] + (bot[0] - top[0]) * t,
               top[1] + (bot[1] - top[1]) * t,
               top[2] + (bot[2] - top[2]) * t);
        }
        RR((float)px, (float)py, (float)pw, 2.0f, OVKC_PANEL_HI);
        RR((float)px, (float)(py + ph) - 2.0f, (float)pw, 2.0f, OVKC_PANEL_LO);
        RR((float)px, (float)py, 2.0f, (float)ph, OVKC_PANEL_LO);
        RR((float)(px + pw) - 2.0f, (float)py, 2.0f, (float)ph, OVKC_PANEL_LO);
    }

    /* title (as the game sent it) and n/max counter share ONE optical
     * centre line in the band above the text box (v3: they sat 4px off).
     * Max-length error: counter CUTS to red and fades back to gray while
     * the shake runs (user: "don't fade to it, just cut ... then fade
     * back"); shake is longer and wider than v2's. */
    char cnt[16];
    snprintf(cnt, sizeof cnt, "%d/%d", (int)strlen(g_text), g_maxlen);
    float cw = ovk_textw(cnt, 0.58f);
    float cnt_x = 584.0f - cw;          /* right edge = text box right edge */
    float band_cy = ((float)py + (float)box[1]) * 0.5f;
    {
        char t[96];
        size_t i;
        for (i = 0; g_title[i] && i < sizeof t - 1; i++)
            t[i] = (g_title[i] >= 0x20 && g_title[i] <= 0x7E) ? g_title[i] : '?';
        t[i] = 0;
        ovk_emit(QB_TITLE, (float)tpos[0],
                 ovk_baseline_cy(band_cy, 0.75f), t, 0.75f);

        static const float dimc[3] = { OVKC_DIM_C };
        static const float errc[3] = { OVKC_ERR };
        float off = 0.0f;
        for (int i = 0; i < 3; i++) g_cnt_col[i] = dimc[i];
        if (g_shake > 0) {
            float f01 = (float)g_shake / (float)OVK_SHAKE_FRAMES;
            int amp = (g_shake * 8) / OVK_SHAKE_FRAMES;   /* up to 8px */
            off = (g_shake & 1) ? -(float)amp : (float)amp;
            for (int i = 0; i < 3; i++)
                g_cnt_col[i] = errc[i] + (dimc[i] - errc[i]) * (1.0f - f01);
            g_shake--;
        }
        ovk_emit(QB_CNT, cnt_x + off,
                 ovk_baseline_cy(band_cy, 0.58f), cnt, 0.58f);
    }

    /* SELECT-cancel affordance: only when the prompt offers a negative
     * label (FepPanel negative_, dex-verified).  Lives in the title band
     * next to the counter now (v2's lonely bottom-band pill was "ugly"). */
    if (g_negative[0]) {
        const char *label = g_negative;
        float lsc = 0.42f;
        float tw = ovk_textw(label, lsc);
        float lx = cnt_x - 12.0f - tw;
        badge_pill(lx - 6.0f - 26.0f, band_cy - 7.0f,
                   26.0f, 14.0f, "SEL", 0.36f);
        ovk_emit(QB_DIM, lx, ovk_baseline_cy(band_cy, lsc), label, lsc);
    }

    /* text box + text (scrolled so the CARET stays visible) + caret */
    {
        int bx = box[0], by = box[1], bw = box[2], bh = box[3];
        RR((float)bx - 2, (float)by - 2, (float)bw + 4, (float)bh + 4,
           OVKC_BOX_BORDER);
        RR((float)bx, (float)by, (float)bw, (float)bh, OVKC_BOX_FILL);
        float sc = 0.846f;
        float bx0 = (float)bx + 16.0f;          /* room for the L1 pill */
        float clipx = (float)bx + (float)bw - 14.0f;
        float maxw = clipx - bx0;
        if (g_caret > (int)strlen(g_text)) g_caret = (int)strlen(g_text);
        size_t st = (size_t)g_caret;
        while (st > 0 &&
               ovk_textw_n(g_text + st - 1, (size_t)g_caret - (st - 1), sc)
               <= maxw)
            st--;
        float bsl = ovk_baseline_cell(by, bh, sc);
        ovk_emit_clip(QB_TEXT, bx0, bsl, g_text + st, sc, clipx);
        float cx = bx0 + ovk_textw_n(g_text + st,
                                     (size_t)g_caret - st, sc) + 2.0f;
        if (cx < clipx)
            RR(cx, (float)by + 8.0f, 3.0f, (float)bh - 16.0f, OVKC_CARET);
        /* L1/R1 caret pills gripping the box edges */
        badge_pill((float)bx - 14.0f, (float)by + (float)bh * 0.5f - 7.5f,
                   28.0f, 15.0f, "L1", 0.42f);
        badge_pill((float)(bx + bw) - 14.0f, (float)by + (float)bh * 0.5f - 7.5f,
                   28.0f, 15.0f, "R1", 0.42f);
    }

    /* keys */
    {
        const ovk_key_t *tab = page_table();
        int n = g_tab_n[g_page];
        for (int i = 0; i < n; i++) {
            const ovk_key_t *k = &tab[i];
            int selected = i == g_sel;
            int is_char = k->act == OVKA_CHAR;
            static const float fc[3] = { OVKC_KEY_FACE };
            static const float ff[3] = { OVKC_FN_FACE };
            static const float fd[3] = { OVKC_DONE_FACE };
            static const float fsh[3] = { OVKC_SHIFT_FACE };
            static const float fcap[3] = { OVKC_CAPS_FACE };
            static const float e[3] = { OVKC_KEY_EDGE };
            const float *face;
            if (k->act == OVKA_DONE)
                face = fd;
            else if (k->act == OVKA_SHIFT && g_shift == 2)
                face = fcap;        /* caps lock: the KEY turns white */
            else if ((k->act == OVKA_SHIFT && g_shift == 1) ||
                     (k->act == OVKA_SYM && g_page == 1))
                face = fsh;         /* one-shot shift / symbols page = lit */
            else
                face = is_char ? fc : ff;
            /* v3 selection: 2px focus ring that fully encloses the key
             * AND its drop shadow (v2's ring ended above the shadow, the
             * "outline wraps the sides, not underneath" the user saw),
             * plus a 10% white lift on the face.  High contrast but not
             * an inversion, and far less visible if a frame ghosts.  On
             * the caps-white key the ring goes DARK so it stays
             * unmistakable (PS5 lesson: selection must always pop). */
            if (selected)
                RR((float)k->x - 2.0f, (float)k->y - 2.0f,
                   (float)k->w + 4.0f, (float)k->h + 6.0f,
                   bright_face(k) ? 0.06f : 1.0f,
                   bright_face(k) ? 0.06f : 1.0f,
                   bright_face(k) ? 0.08f : 1.0f);
            /* skeuomorphic key: drop shadow, face, lit top, dark bottom */
            RR((float)k->x + 1.0f, (float)k->y + 2.0f,
               (float)k->w, (float)k->h, OVKC_KEY_LO);
            RR((float)k->x, (float)k->y, (float)k->w, (float)k->h,
               face[0], face[1], face[2]);
            if (selected)
                RRA((float)k->x, (float)k->y, (float)k->w, (float)k->h,
                    1.0f, 1.0f, 1.0f, 0.10f);
            RR((float)k->x, (float)k->y, (float)k->w, 2.0f,
               e[0], e[1], e[2]);
            RR((float)k->x, (float)(k->y + k->h) - 2.0f, (float)k->w, 2.0f,
               OVKC_KEY_LO);
            draw_key_label(k);
            /* controller badges on the keys they activate; they invert
             * only on a BRIGHT face (caps-white) -- selection alone no
             * longer flips them. */
            if (k->act == OVKA_SHIFT)
                badge_circle((float)k->x + 13.0f, (float)k->y + 13.0f, 'X',
                             bright_face(k));
            else if (k->act == OVKA_SPACE)
                badge_circle((float)k->x + 13.0f, (float)k->y + 13.0f, 'Y', 0);
            else if (k->act == OVKA_BKSP)   /* tucked into the corner: the
                             * 1u Del key's label nudged the other way */
                badge_circle((float)k->x + 11.0f, (float)k->y + 11.0f, 'B', 0);
            else if (k->act == OVKA_DONE)
                badge_pill((float)k->x + 5.0f, (float)k->y + 4.5f,
                           40.0f, 15.0f, "START", 0.33f);
        }
    }

    /* text over everything */
    for (int i = 0; i < QB_COUNT; i++)
        if (g_qn[i])
            gds_egl_overlay_quads(g_qb[i], g_qn[i] / 24,
                                  g_qb_col[i][0], g_qb_col[i][1],
                                  g_qb_col[i][2]);
    gds_egl_overlay_end();
}

/* ------------------------------------------------------------------- api */
void gds_osk_open(const char *title, const char *initial, int maxlen)
{
    style_decide();
    if (maxlen > 0 && maxlen < (int)sizeof g_text)
        g_maxlen = maxlen;
    else
        g_maxlen = 16;
    g_negative[0] = 0;          /* set by gds_osk_set_negative, per prompt */
    if (!g_open) {
        g_open = 1;
        g_csel = 0;
        g_sel = g_home;
        g_page = 0;
        g_shift = 0;
        g_latch = 1;            /* swallow pad until full release once */
        if (title && title[0]) text_copy(g_title, sizeof g_title, title);
        else text_copy(g_title, sizeof g_title, "Enter text");
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
     * blanks a Japanese game font would use.  PREFILL ONLY, never typed
     * text. */
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
         * it, and printing it raw made the log line overwrite itself. */
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
    g_caret = (int)n;
    g_upper = n == 0 || g_text[n - 1] == ' ';
    /* new style: sentence start -> one-shot shift, exactly like the phone
     * keyboards this replaces. */
    g_shift = (n == 0 || g_text[n - 1] == ' ') ? 1 : 0;
    /* prefill may end in a symbol ("Game #1"): open on the symbols page so
     * the user sees the matching page first. */
    if (n > 0) {
        unsigned char last = (unsigned char)g_text[n - 1];
        int on_letters = (last >= 'a' && last <= 'z') ||
                         (last >= 'A' && last <= 'Z') ||
                         (last >= '0' && last <= '9') || last == ' ';
        g_page = on_letters ? 0 : 1;
    }
    g_done = 0;
    g_ok = 0;
    g_shake = 0;                /* a fresh prompt never inherits the
                                 * previous one's red-flash mid-decay */
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

/* per-prompt cancel affordance (kairo FepPanel negative_ label); call
 * right after gds_osk_open from the Utility.startInputPanel hook. */
void gds_osk_set_negative(const char *label)
{
    if (!g_open || !label || !label[0]) { g_negative[0] = 0; return; }
    int printable = 1;
    size_t n = strlen(label);
    for (size_t i = 0; i < n; i++)
        if ((unsigned char)label[i] < 0x20 || (unsigned char)label[i] > 0x7E)
            { printable = 0; break; }
    text_copy(g_negative, sizeof g_negative,
              (printable && n <= 10) ? label : "back");
}

void gds_osk_set_text(const char *text)
{
    if (!g_open)
        return;
    text_copy(g_text, sizeof g_text, text);
    size_t n = strlen(g_text);
    if (n > (size_t)g_maxlen) g_text[g_maxlen] = 0;
    g_caret = (int)strlen(g_text);
    g_upper = n == 0 || g_text[n - 1] == ' ';
    g_shift = (n == 0 || g_text[n - 1] == ' ') ? 1 : 0;
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

/* Pad arrays are indexed by the shared NPB_ enum (gds.h).  Runs once per
 * frame from gds_input_poll (input.c). */
void gds_osk_pad_tick(const unsigned char *cur, const unsigned char *prev)
{
    static int rep, rep2;
    if (!g_open) { rep = rep2 = 0; return; }
    style_decide();
#define BTN(i)   (cur[i])
#define BTNDN(i) (cur[i] && !prev[i])
    if (g_latch) {
        int any = 0;
        for (int i = 0; i < NPB_COUNT; i++) any |= cur[i];
        if (!any) g_latch = 0;
        return;
    }
    int dx = BTN(NPB_DR) ? 1 : (BTN(NPB_DL) ? -1 : 0);
    int dy = BTN(NPB_DD) ? 1 : (BTN(NPB_DU) ? -1 : 0);
    int edge_move = BTNDN(NPB_DU) || BTNDN(NPB_DD) || BTNDN(NPB_DL) || BTNDN(NPB_DR);
    if ((dx || dy) && (edge_move || rep <= 0)) {
        if (dx) { if (g_style == 1) classic_move_selection(dx, 0);
                  else new_move_selection(dx, 0); }
        if (dy) { if (g_style == 1) classic_move_selection(0, dy);
                  else new_move_selection(0, dy); }
        rep = edge_move ? 14 : 8;
    }
    if (rep > 0) rep--;
    if (g_style == 1) {
        if (BTNDN(NPB_A) || BTNDN(NPB_R3)) classic_activate_key(g_csel);
        if (BTNDN(NPB_B)) { vk_backspace(); classic_reflect_case(); }
        if (BTNDN(NPB_X)) g_upper = !g_upper;
        if (BTNDN(NPB_Y)) { vk_append_char(' '); g_upper = 1; }
    } else {
        if (BTNDN(NPB_A) || BTNDN(NPB_R3))
            new_activate(&page_table()[g_sel]);
        if (BTNDN(NPB_B)) vk_backspace();        /* shift untouched */
        if (BTNDN(NPB_X)) g_shift = (g_shift + 1) % 3;
        if (BTNDN(NPB_Y)) insert_char(' ');
        /* L1/R1 walk the text caret (Xbox LT/RB precedent).  0.95.11
         * user: held-bumper repeat was "pretty slow, should move 1.5-2x
         * as fast" -- initial 14f->8f, repeat 8f->4f (~2x). */
        int cd = BTN(NPB_RB) ? 1 : (BTN(NPB_LB) ? -1 : 0);
        int edge_caret = BTNDN(NPB_LB) || BTNDN(NPB_RB);
        if (cd && (edge_caret || rep2 <= 0)) {
            int len = (int)strlen(g_text);
            g_caret += cd;
            if (g_caret < 0) g_caret = 0;
            if (g_caret > len) g_caret = len;
            rep2 = edge_caret ? 8 : 4;
        }
        if (rep2 > 0) rep2--;
    }
    /* SELECT = cancel only when the prompt offers it (v3): with START and
     * SELECT now actually arriving (input.c evdev shadow merge), an
     * unadvertised cancel on e.g. the company-name prompt could put the
     * game into a re-prompt loop.  Classic keeps its historic behaviour.
     * START = Done, always. */
    if (BTNDN(NPB_BACK) && (g_style == 1 || g_negative[0])) vk_cancel();
    if (BTNDN(NPB_START)) vk_commit();
#undef BTN
#undef BTNDN
}

void gds_osk_draw(void)
{
    if (!g_open)
        return;
    style_decide();
    if (g_style == 1) classic_draw();
    else new_draw();
}
