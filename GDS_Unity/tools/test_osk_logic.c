/* host test for the OSK interaction logic: includes the real osk.c with
 * the GL seam stubbed, then drives pad scripts and asserts text/shift/
 * page/caret/commit/cancel outcomes.  NOT shipped (build-time proof).
 * Run: gcc -O1 -Wall -o /tmp/test_osk_logic tools/test_osk_logic.c -lm
 *      /tmp/test_osk_logic */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/* ---- GL/platform stubs ------------------------------------------------ */
char gds_gamedir[1024] = "/nonexistent";
int gds_egl_overlay_begin(int *sw, int *sh) { *sw = 640; *sh = 480; return 1; }
void gds_egl_overlay_rect(float x, float y, float w, float h, float r, float g, float b)
{ (void)x;(void)y;(void)w;(void)h;(void)r;(void)g;(void)b; }
void gds_egl_overlay_rect_a(float x, float y, float w, float h, float r, float g, float b, float a)
{ (void)x;(void)y;(void)w;(void)h;(void)r;(void)g;(void)b;(void)a; }
int gds_egl_overlay_atlas(const unsigned char *rgba, int w, int h)
{ (void)rgba;(void)w;(void)h; return 1; }
void gds_egl_overlay_quads(const float *v, int n, float r, float g, float b)
{ (void)v;(void)n;(void)r;(void)g;(void)b; }
void gds_egl_overlay_end(void) {}
int egl_shim_screen_w(void) { return 640; }
int egl_shim_screen_h(void) { return 480; }
long gds_mono_ms(void) { static long t = 0; t += 33; return t; }

#include "../loader_ref/osk.c"

/* ---- pad driving helpers ---------------------------------------------- */
static unsigned char cur[NPB_COUNT], prev[NPB_COUNT];
static void tick(void) { gds_osk_pad_tick(cur, prev); memcpy(prev, cur, sizeof cur); }
static void press(int b) { memset(cur, 0, sizeof cur); cur[b] = 1; tick(); memset(cur, 0, sizeof cur); tick(); }
static void release_all(void) { memset(cur, 0, sizeof cur); tick(); }

static int find_key(int page, char lo) {
    const ovk_key_t *t = page ? g_tab_symbols : g_tab_letters;
    for (int i = 0; i < g_tab_n[page]; i++)
        if (t[i].act == OVKA_CHAR && t[i].lo == lo) return i;
    return -1;
}
static int find_act(int page, int act) {
    const ovk_key_t *t = page ? g_tab_symbols : g_tab_letters;
    for (int i = 0; i < g_tab_n[page]; i++)
        if (t[i].act == act) return i;
    return -1;
}

#define CHECK(cond, msg) do { if (!(cond)) { \
        printf("FAIL: %s (text=\"%s\" shift=%d page=%d sel=%d caret=%d shake=%d open=%d done=%d ok=%d)\n", msg, \
               g_text, g_shift, g_page, g_sel, g_caret, g_shake, g_open, g_done, g_ok); failures++; \
    } else { printf("ok: %s\n", msg); } } while (0)

static int failures;

int main(void) {
    unsetenv("GDS_OSK");
    style_decide();
    CHECK(g_style == 0, "style defaults to new");

    /* 1. open with CR-suffixed prefill: trim works, shift=0, caret at end */
    gds_osk_open("Company Name\r", "Sunny Studios\r", 14);
    CHECK(strcmp(g_title, "Company Name") == 0, "title trailing CR trimmed");
    CHECK(blink_visible() == 1, "caret solid (not blinked out) at open");
    CHECK(strcmp(g_text, "Sunny Studios") == 0, "prefill CR trimmed");
    CHECK(g_shift == 0, "no shift after letter-ending prefill");
    CHECK(g_page == 0, "letters page for letter ending");
    CHECK(g_caret == 13, "caret starts at end");
    CHECK(g_latch == 1, "latch armed");

    /* 2. latch swallows the first press */
    press(NPB_A);
    CHECK(strcmp(g_text, "Sunny Studios") == 0, "latch swallowed press");

    /* 3. empty text -> sentence-case one-shot shift; home selection */
    gds_osk_open("Game name", "", 12);
    release_all();
    CHECK(g_shift == 1, "empty text arms one-shot shift");
    CHECK(g_sel == g_home, "selection starts at g home");

    g_sel = find_key(0, 'a');
    press(NPB_A);
    CHECK(strcmp(g_text, "A") == 0, "one-shot typed capital");
    CHECK(g_shift == 0, "one-shot reverted by letter");
    CHECK(g_caret == 1, "caret advanced");
    press(NPB_A);
    CHECK(strcmp(g_text, "Aa") == 0, "second letter lowercase");

    /* 4. X cycles 0->1->2->0; caps lock holds across letters */
    press(NPB_X); CHECK(g_shift == 1, "X -> one-shot");
    press(NPB_X); CHECK(g_shift == 2, "X -> caps lock");
    g_sel = find_key(0, 'b');
    press(NPB_A); press(NPB_A);
    CHECK(strcmp(g_text + strlen(g_text) - 2, "BB") == 0, "caps lock typed capitals");
    CHECK(g_shift == 2, "caps lock survived letters");

    /* 5. v3 LAYERS: caps uppercases LETTERS ONLY (physical Caps Lock);
     *    one-shot shift applies the full layer (digits + punct pairs). */
    g_sel = find_key(0, '1');
    press(NPB_A);
    CHECK(g_text[strlen(g_text) - 1] == '1', "caps lock digit stays digit");
    g_sel = find_key(0, ',');
    press(NPB_A);
    CHECK(g_text[strlen(g_text) - 1] == ',', "caps lock comma stays comma");
    press(NPB_X); CHECK(g_shift == 0, "X -> off");
    press(NPB_X); CHECK(g_shift == 1, "X -> one-shot again");
    g_sel = find_key(0, '1');
    press(NPB_A);
    CHECK(g_text[strlen(g_text) - 1] == '!', "one-shot digit -> ! (pair)");
    CHECK(g_shift == 0, "one-shot spent by symbol too");
    press(NPB_X);
    g_sel = find_key(0, '-');
    press(NPB_A);
    CHECK(g_text[strlen(g_text) - 1] == '_', "one-shot hyphen -> underscore");
    press(NPB_X);
    g_sel = find_key(0, '\'');
    press(NPB_A);
    CHECK(g_text[strlen(g_text) - 1] == '"', "one-shot quote -> dquote");
    press(NPB_X);
    g_sel = find_key(0, '.');
    press(NPB_A);
    CHECK(g_text[strlen(g_text) - 1] == '>', "one-shot period -> gt");

    /* 6. Y = space (leaves shift alone), B = backspace at caret */
    int sh_before = g_shift;
    press(NPB_Y);
    CHECK(g_text[strlen(g_text) - 1] == ' ', "Y typed a space");
    CHECK(g_shift == sh_before, "Y did not touch shift");
    press(NPB_B);
    CHECK(g_text[strlen(g_text) - 1] == '>', "B backspaced the space");
    CHECK(g_shift == sh_before, "backspace never touches shift");

    /* 7. page flip: row kept, col clamped 10->8 and back; fn <-> fn */
    g_page = 0;
    g_sel = 5;                                   /* letters row 0 col 5 = '6' */
    page_flip();
    CHECK(g_page == 1 && g_sel == 5, "flip keeps row, same col (^)");
    g_page = 0;
    g_sel = 9;                                   /* col 9 '0' */
    page_flip();
    CHECK(g_page == 1 && g_sel == 7, "col 9 clamps to 7 (*)");
    g_page = 0;
    g_sel = 39;                                  /* row 3 col 9 '-' */
    page_flip();
    CHECK(g_page == 1 && g_sel == 31, "row kept on last row (?)");
    page_flip();
    CHECK(g_page == 0 && g_sel == 37, "symbols col 7 -> letters col 7");
    g_sel = find_act(0, OVKA_DONE);              /* fn <-> fn 1:1 */
    int done_idx = g_sel;
    page_flip();
    CHECK(g_page == 1 && g_sel == OVK_N_SYMBOLS + (done_idx - OVK_N_LETTERS),
          "Done maps to Done across pages");
    page_flip();
    CHECK(g_page == 0 && g_sel == done_idx, "and back");

    /* 8. symbols page is FLAT: lo==hi everywhere, shift changes nothing */
    CHECK(find_key(1, '!') >= 0, "bang on symbols page");
    CHECK(find_key(1, '~') >= 0 && find_key(1, '|') >= 0, "tilde + pipe present");
    CHECK(find_key(1, '`') >= 0 && find_key(1, '_') >= 0, "backtick + underscore present");
    {
        int i, pairs = 0;
        for (i = 0; i < OVK_N_SYMBOLS; i++)
            if (g_tab_symbols[i].lo != g_tab_symbols[i].hi) pairs++;
        CHECK(pairs == 0, "no pairs on symbols page");
        CHECK(OVK_N_SYMBOLS == 32, "32 flat symbol keys (4x8 grid)");
    }
    g_page = 1;
    g_sel = find_key(1, '#');
    g_shift = 0;
    press(NPB_A);
    CHECK(g_text[strlen(g_text) - 1] == '#', "# types unshifted (Game #1!)");
    g_shift = 1;                                 /* stale one-shot... */
    g_sel = find_key(1, ';');
    press(NPB_A);
    CHECK(g_text[strlen(g_text) - 1] == ';', "shift on symbols page changes nothing");
    CHECK(g_shift == 0, "...but is still spent like a keyboard");
    press(NPB_X);                                /* X cycles there too */
    CHECK(g_shift == 1 && g_page == 1, "X cycles on symbols page");
    g_shift = 0;

    /* 9. shifted_char truth table (display layer == type layer) */
    {
        ovk_key_t d1 = { 0,0,0,0, '1','!', OVKA_CHAR, "1" };
        ovk_key_t qa = { 0,0,0,0, 'q','Q', OVKA_CHAR, "q" };
        ovk_key_t cm = { 0,0,0,0, ',','<', OVKA_CHAR, "," };
        ovk_key_t at = { 0,0,0,0, '@','@', OVKA_CHAR, "@" };
        CHECK(shifted_char(&d1, 0) == '1' && shifted_char(&d1, 1) == '!'
              && shifted_char(&d1, 2) == '1', "digit: shift pairs, caps not");
        CHECK(shifted_char(&qa, 0) == 'q' && shifted_char(&qa, 1) == 'Q'
              && shifted_char(&qa, 2) == 'Q', "letter: shift + caps upper");
        CHECK(shifted_char(&cm, 0) == ',' && shifted_char(&cm, 1) == '<'
              && shifted_char(&cm, 2) == ',', "punct: shift pairs, caps not");
        CHECK(shifted_char(&at, 1) == '@' && shifted_char(&at, 2) == '@',
              "flat key never changes");
    }

    /* 10. caret: L1/R1 walk (not pages), insert + backspace AT caret */
    gds_osk_open("Game name", "Game 1", 12);
    release_all();
    g_page = 0;
    press(NPB_LB);
    CHECK(g_caret == 5, "LB walks caret left");
    press(NPB_RB);
    CHECK(g_caret == 6, "RB walks caret right");
    press(NPB_LB); press(NPB_LB);
    CHECK(g_caret == 4, "caret walks twice more");
    g_sel = find_key(0, 'x');
    press(NPB_A);
    CHECK(strcmp(g_text, "Gamex 1") == 0, "insert at caret");
    CHECK(g_caret == 5, "caret after inserted char");
    press(NPB_B);
    CHECK(strcmp(g_text, "Game 1") == 0, "backspace at caret");
    press(NPB_LB); press(NPB_LB); press(NPB_LB);
    press(NPB_LB); press(NPB_LB); press(NPB_LB);
    CHECK(g_caret == 0, "caret clamps at 0");
    press(NPB_B);
    CHECK(strcmp(g_text, "Game 1") == 0, "backspace at 0 no-ops");

    /* 11. nav: home g -> right x2 -> j, x3 -> k, down -> ',', fn row */
    gds_osk_open("t", "", 12);
    release_all();
    g_page = 0; g_sel = g_home;
    press(NPB_DR); press(NPB_DR);
    CHECK(g_tab_letters[g_sel].lo == 'j', "home -> right x2 lands on j");
    press(NPB_DR);
    CHECK(g_tab_letters[g_sel].lo == 'k', "right again lands on k");
    press(NPB_DD);
    CHECK(g_tab_letters[g_sel].lo == ',', "down from k lands on comma (x-aligned)");
    press(NPB_DD);
    CHECK(g_tab_letters[g_sel].act != OVKA_CHAR, "down again reaches fn row");
    press(NPB_DU);
    CHECK(g_tab_letters[g_sel].act == OVKA_CHAR, "up returns to keys");

    /* 12. maxlen: shake/flash armed, text unchanged */
    gds_osk_open("t", "", 3);
    release_all();
    g_sel = find_key(0, 'a');
    g_shift = 0;
    press(NPB_A); press(NPB_A); press(NPB_A);
    CHECK(strcmp(g_text, "aaa") == 0, "three chars typed");
    press(NPB_A);
    CHECK(g_shake == OVK_SHAKE_FRAMES, "maxlen armed shake/flash");
    CHECK(strcmp(g_text, "aaa") == 0, "overflow char rejected");

    /* 13. START commits; SELECT does NOTHING without a negative label */
    g_open = 1; g_done = 0; g_ok = 0; g_negative[0] = 0;
    press(NPB_BACK);
    CHECK(g_open == 1 && g_done == 0, "SELECT ignored when cancel not offered");
    press(NPB_START);
    CHECK(g_done == 1 && g_ok == 1 && g_open == 0, "START commits");

    /* 14. negative label offered -> SELECT backs out with the ORIGINAL
     * prefill.  0.95.13: the old NULL result was device-proven game-fatal
     * (cancel at boot "Company Name" -> the game raises its own error
     * dialog and exits clean), so cancel must finish as "unchanged OK".
     * Any mid-prompt edits are undone by the restore. */
    gds_osk_open("t", "abc", 12);
    release_all();
    gds_osk_set_negative("back");
    CHECK(g_negative[0] != 0, "negative stored");
    g_sel = find_key(0, 'd');
    g_shift = 0;
    press(NPB_A);
    CHECK(strcmp(g_text, "abcd") == 0, "edit made before cancel");
    press(NPB_BACK);
    CHECK(g_done == 1 && g_ok == 1 && strcmp(g_text, "abc") == 0,
          "SELECT backs out with ORIGINAL text (game-safe)");
    gds_osk_open("t", "abc", 12);
    release_all();
    gds_osk_set_negative("");
    CHECK(g_negative[0] == 0, "empty negative = not offered");
    gds_osk_set_negative("12345678901234567890");
    CHECK(strcmp(g_negative, "back") == 0, "long garbage -> generic back");

    /* 15. classic style: SELECT backs out with original text (same
     * 0.95.13 game-fatal-null fix), START commits */
    setenv("GDS_OSK", "classic", 1);
    g_style = -1;
    style_decide();
    CHECK(g_style == 1, "classic forced");
    gds_osk_open("t", "abc", 12);
    release_all();
    press(NPB_START);
    CHECK(g_done == 1 && g_ok == 1, "classic START commits");
    gds_osk_open("t", "abc", 12);
    release_all();
    press(NPB_BACK);
    CHECK(g_done == 1 && g_ok == 1 && strcmp(g_text, "abc") == 0,
          "classic SELECT backs out with ORIGINAL text");
    unsetenv("GDS_OSK");
    g_style = -1;
    style_decide();

    printf(failures ? "\n%d FAILURES\n" : "\nALL OK\n", failures);
    return failures ? 1 : 0;
}
