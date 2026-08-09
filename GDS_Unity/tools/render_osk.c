/* host render harness: drives the REAL osk.c draw path with the GL seam
 * stubbed to record primitives, so a pixel-true preview of the actual C
 * layout can be rendered offline (render_osk.py rasterizes).  NOT shipped.
 * Run: gcc -O1 -o /tmp/render_osk tools/render_osk.c -lm && cd /tmp &&
 *      /tmp/render_osk && /home/user/venv/bin/python3 tools/render_osk.py */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

char gds_gamedir[1024] = "/home/user/asdf/GDS_Unity/ports/gamedevstory/gamedevstory";

static FILE *out;
int gds_egl_overlay_begin(int *sw, int *sh) { *sw = 640; *sh = 480; return 1; }
void gds_egl_overlay_rect(float x, float y, float w, float h, float r, float g, float b)
{ fprintf(out, "RECT %.2f %.2f %.2f %.2f %.3f %.3f %.3f\n", x, y, w, h, r, g, b); }
void gds_egl_overlay_rect_a(float x, float y, float w, float h, float r, float g, float b, float a)
{ fprintf(out, "RECTA %.2f %.2f %.2f %.2f %.3f %.3f %.3f %.3f\n", x, y, w, h, r, g, b, a); }
int gds_egl_overlay_atlas(const unsigned char *rgba, int w, int h)
{ (void)rgba; (void)w; (void)h; return 1; }
void gds_egl_overlay_quads(const float *v, int n, float r, float g, float b)
{
    for (int i = 0; i < n; i++) {
        const float *q = v + i * 24;
        float x = q[0], y = q[1], w = q[16] - q[0], h = q[21] - q[1];
        float u0 = q[2], v0 = q[3], u1 = q[18], v1 = q[19];
        fprintf(out, "QUAD %.2f %.2f %.2f %.2f %.5f %.5f %.5f %.5f %.3f %.3f %.3f\n",
                x, y, w, h, u0, v0, u1, v1, r, g, b);
    }
}
void gds_egl_overlay_end(void) { fprintf(out, "END\n"); }
int egl_shim_screen_w(void) { return 640; }
int egl_shim_screen_h(void) { return 480; }
long gds_mono_ms(void) { static long t = 0; t += 33; return t; }

#include "../loader_ref/osk.c"

static int find_key(int page, char lo) {
    const ovk_key_t *t = page ? g_tab_symbols : g_tab_letters;
    for (int i = 0; i < g_tab_n[page]; i++)
        if (t[i].act == OVKA_CHAR && t[i].lo == lo) return i;
    return -1;
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    unsetenv("GDS_OSK");
    /* scene 1: letters page, "Sunny Studios" (CR-trimmed), sel on 'y' */
    out = fopen("/tmp/osk2_scene1.prim", "w");
    gds_osk_open("Company name", "Sunny Studios\r", 14);
    g_latch = 0;
    g_sel = find_key(0, 'y');
    gds_osk_draw();
    fclose(out);

    /* scene 2: flat symbols page, sel on '#', caret mid-string, maxlen
     * counter mid-FLASH (solid red), SEL cancel pill in the title band */
    out = fopen("/tmp/osk2_scene2.prim", "w");
    g_open = 0;
    gds_osk_open("Game name", "Game #1", 12);
    g_latch = 0;
    g_page = 1;
    g_sel = find_key(1, '#');
    g_shift = 0;
    g_caret = 4;                       /* caret between 'e' and ' ' */
    g_shake = 14;                      /* mid-flash: counter well into red */
    gds_osk_set_negative("Cancel");    /* cancellable prompt */
    gds_osk_draw();
    fclose(out);

    /* scene 3: caps lock on letters page, sel on Shift key: white Caps
     * cap + dark focus ring + dark badge */
    out = fopen("/tmp/osk2_scene3.prim", "w");
    g_open = 0;
    g_shake = 0;               /* keep the counter gray (scene 2 flashed) */
    gds_osk_open("Company name", "", 14);
    g_latch = 0;
    g_shift = 2;                       /* caps lock */
    for (int i = 0; i < g_tab_n[0]; i++)
        if (g_tab_letters[i].act == OVKA_SHIFT) g_sel = i;
    gds_osk_draw();
    fclose(out);

    /* scene 4: one-shot shift on the letters page: digits/punct show the
     * paired layer BIG with the base glyph small, Shift key lit gray,
     * selection ring on '7' */
    out = fopen("/tmp/osk2_scene4.prim", "w");
    g_open = 0;
    gds_osk_open("Game name", "Game", 12);
    g_latch = 0;
    g_shift = 1;
    g_sel = find_key(0, '7');
    gds_osk_draw();
    fclose(out);

    printf("scenes written, font %s\n", g_font ? "loaded" : "MISSING");
    return 0;
}
