/* vt.c - minimal VT100/xterm terminal emulator with a PTY backend.
 *
 * Scope: enough of ECMA-48/xterm to run a shell, nano, htop and tmux.
 * Supported: C0 controls, CSI cursor/erase/insert/delete/scroll, SGR
 * (16/256/true color, bold, underline, reverse), scroll regions, tab stops,
 * alternate screen (1049/47/1047), save/restore cursor, DEC special
 * graphics, UTF-8 decoding (unknown glyphs fall back to ASCII art).
 *
 * Not supported: mouse reporting, sixel, bidi, wide/combining characters.
 */
#define _GNU_SOURCE
#include "vt.h"

#include <errno.h>
#include <fcntl.h>
#include <pty.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

#define PARAM_MAX 16

enum {
    ST_GROUND = 0,
    ST_ESC,
    ST_ESC_INT,
    ST_CSI,
    ST_CSI_INT,
    ST_OSC,
    ST_OSC_ESC,
    ST_STR,
    ST_STR_ESC
};

struct vt {
    int cols, rows, sb, total;

    vt_cell_t ** lines;      /* ring of total = rows + sb lines */
    vt_cell_t ** alt_lines;  /* alternate screen, rows lines */
    int top;
    int alt;
    int alt_cx, alt_cy;

    int cx, cy;
    int cur_vis;
    int wrap_pending;
    int autowrap;
    int origin;
    int insert;
    int st_top, st_bot;

    uint32_t fg, bg;
    uint8_t attr;
    uint32_t def_fg, def_bg;

    int s_cx, s_cy, s_origin;
    uint32_t s_fg, s_bg;
    uint8_t s_attr;

    int view;
    int dirty;
    int dead;
    int fd;
    pid_t pid;

    int state;
    int params[PARAM_MAX];
    int nparam;
    int cur;
    int priv;
    int inter;
    int g0_graphics;

    int utf8_need;
    uint32_t utf8_cp;

    uint8_t tabs[512];
};

/* ---------------- colors ---------------- */

static uint32_t pal256(int i)
{
    static const uint32_t base[16] = {
        0x1C2126, 0xE06C75, 0x98C379, 0xE5C07B,
        0x61AFEF, 0xC678DD, 0x56B6C2, 0xD7DAE0,
        0x5C6370, 0xFF7B86, 0xB5E890, 0xFFD68A,
        0x7CC4FF, 0xDDA0F0, 0x6FD3DE, 0xFFFFFF
    };
    static const int lv[6] = {0, 95, 135, 175, 215, 255};

    if(i < 0) i = 0;
    if(i < 16) return base[i];
    if(i < 232) {
        int n = i - 16;
        int r = lv[n / 36];
        int g = lv[(n % 36) / 6];
        int b = lv[n % 6];
        return ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
    }
    if(i > 255) i = 255;
    int g = 8 + (i - 232) * 10;
    return ((uint32_t)g << 16) | ((uint32_t)g << 8) | (uint32_t)g;
}

/* ---------------- small helpers ---------------- */

static int clampi(int v, int lo, int hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

static void line_clear(vt_t * v, vt_cell_t * cells, uint32_t bg)
{
    vt_cell_t c;
    c.ch = ' ';
    c.fg = v->def_fg;
    c.bg = bg;
    c.attr = 0;
    for(int i = 0; i < v->cols; i++) cells[i] = c;
}

static vt_cell_t * srow(vt_t * v, int r)
{
    if(v->alt) return v->alt_lines[r];
    return v->lines[(v->top + r) % v->total];
}

static uint32_t map_unicode(uint32_t cp)
{
    if(cp < 0x20) return ' ';
    if(cp <= 0x7E) return cp;
    if(cp == 0x00A0) return ' ';

    /* Ranges included in our term_font (Fira Code Nerd Font) */
    if((cp >= 0x00A0 && cp <= 0x00FF) ||
       (cp >= 0x2010 && cp <= 0x203A) ||
       (cp >= 0x2190 && cp <= 0x21FF) ||
       (cp >= 0x2300 && cp <= 0x23FF) ||
       (cp >= 0x2500 && cp <= 0x25FF) ||
       (cp >= 0x2600 && cp <= 0x27BF) ||
       (cp >= 0xE0A0 && cp <= 0xE0D7) ||
       (cp >= 0xE700 && cp <= 0xE7C5))
        return cp;

    /* Fallbacks for common symbols outside the embedded font */
    switch(cp) {
        case 0x2018: case 0x2019: return '\'';
        case 0x201C: case 0x201D: return '"';
        case 0x2022: return '*';
        case 0x2026: return '.';
        default: break;
    }
    return '?';
}

static uint32_t map_graphics(uint32_t ch)
{
    switch(ch) {
        case '`': return 0x25C6; /* black diamond ◆ */
        case 'a': return 0x2592; /* checkerboard ▒ */
        case 'f': return 0x00B0; /* degree symbol ° */
        case 'g': return 0x00B1; /* plus-minus ± */
        case 'j': return 0x2518; /* lower right corner ┘ */
        case 'k': return 0x2510; /* upper right corner ┐ */
        case 'l': return 0x250C; /* upper left corner ┌ */
        case 'm': return 0x2514; /* lower left corner └ */
        case 'n': return 0x253C; /* crossing lines ┼ */
        case 'o': return 0x23BA; /* scan 1 ⎺ */
        case 'p': return 0x23BB; /* scan 3 ⎻ */
        case 'q': return 0x2500; /* horizontal line ─ */
        case 'r': return 0x23BC; /* scan 7 ⎼ */
        case 's': return 0x23BD; /* scan 9 ⎽ */
        case 't': return 0x251C; /* left tee ├ */
        case 'u': return 0x2524; /* right tee ┤ */
        case 'v': return 0x2534; /* bottom tee ┴ */
        case 'w': return 0x252C; /* top tee ┬ */
        case 'x': return 0x2502; /* vertical line │ */
        case 'y': return 0x2264; /* <= */
        case 'z': return 0x2265; /* >= */
        case '{': return 0x03C0; /* pi */
        case '|': return 0x2260; /* != */
        case '}': return 0x00A3; /* pound */
        case '~': return 0x00B7; /* bullet · */
        default: return ch;
    }
}

/* ---------------- scrolling ---------------- */

static void scroll_up(vt_t * v, int top, int bot, int n)
{
    if(n <= 0 || top > bot) return;
    if(n > bot - top + 1) n = bot - top + 1;

    if(!v->alt && top == 0 && bot == v->rows - 1) {
        /* Full screen on the main buffer: advance the ring so the top lines
         * become scrollback history. */
        v->top = (v->top + n) % v->total;
        for(int r = v->rows - n; r < v->rows; r++) line_clear(v, srow(v, r), v->bg);
        if(v->view > 0) v->view = clampi(v->view + n, 0, v->sb);
        v->dirty = 1;
        return;
    }

    int cnt = bot - top + 1 - n;
    for(int i = 0; i < cnt; i++)
        memcpy(srow(v, top + i), srow(v, top + i + n), (size_t)v->cols * sizeof(vt_cell_t));
    for(int i = cnt; i < bot - top + 1; i++)
        line_clear(v, srow(v, top + i), v->bg);
    v->dirty = 1;
}

static void scroll_down(vt_t * v, int top, int bot, int n)
{
    if(n <= 0 || top > bot) return;
    if(n > bot - top + 1) n = bot - top + 1;

    int cnt = bot - top + 1 - n;
    for(int i = cnt - 1; i >= 0; i--)
        memcpy(srow(v, top + i + n), srow(v, top + i), (size_t)v->cols * sizeof(vt_cell_t));
    for(int i = 0; i < n; i++)
        line_clear(v, srow(v, top + i), v->bg);
    v->dirty = 1;
}

/* ---------------- cursor / output ---------------- */

static void linefeed(vt_t * v)
{
    v->wrap_pending = 0;
    if(v->cy == v->st_bot) scroll_up(v, v->st_top, v->st_bot, 1);
    else if(v->cy < v->rows - 1) v->cy++;
}

static void reverse_index(vt_t * v)
{
    v->wrap_pending = 0;
    if(v->cy == v->st_top) scroll_down(v, v->st_top, v->st_bot, 1);
    else if(v->cy > 0) v->cy--;
}

static void put_char(vt_t * v, uint32_t ch)
{
    int wide = 0;
    (void)wide;

    if(ch == 0) ch = ' ';
    if(v->wrap_pending && v->autowrap) {
        v->cx = 0;
        linefeed(v);
    }
    v->wrap_pending = 0;

    if(v->cx >= v->cols) v->cx = v->cols - 1;
    vt_cell_t * row = srow(v, v->cy);

    if(v->insert)
        memmove(row + v->cx + 1, row + v->cx,
                (size_t)(v->cols - v->cx - 1) * sizeof(vt_cell_t));

    row[v->cx].ch = ch;
    row[v->cx].fg = v->fg;
    row[v->cx].bg = v->bg;
    row[v->cx].attr = v->attr;

    if(v->cx + 1 >= v->cols) {
        v->cx = v->cols - 1;
        if(v->autowrap) v->wrap_pending = 1;
    }
    else v->cx++;

    v->dirty = 1;
}

static void tab_forward(vt_t * v, int n)
{
    for(int k = 0; k < n; k++) {
        int x = v->cx + 1;
        while(x < v->cols && x < (int)sizeof(v->tabs) && !v->tabs[x]) x++;
        v->cx = x < v->cols ? x : v->cols - 1;
    }
    v->wrap_pending = 0;
}

static void tab_backward(vt_t * v, int n)
{
    for(int k = 0; k < n; k++) {
        int x = v->cx - 1;
        while(x > 0 && !v->tabs[x]) x--;
        v->cx = x < 0 ? 0 : x;
    }
    v->wrap_pending = 0;
}

static void erase_cells(vt_t * v, int row, int from, int to)
{
    if(row < 0 || row >= v->rows) return;
    if(from < 0) from = 0;
    if(to > v->cols - 1) to = v->cols - 1;
    vt_cell_t * r = srow(v, row);
    for(int i = from; i <= to; i++) {
        r[i].ch = ' ';
        r[i].fg = v->def_fg;
        r[i].bg = v->bg;
        r[i].attr = 0;
    }
    v->dirty = 1;
}

static void erase_display(vt_t * v, int mode)
{
    if(mode == 0) {
        erase_cells(v, v->cy, v->cx, v->cols - 1);
        for(int r = v->cy + 1; r < v->rows; r++) erase_cells(v, r, 0, v->cols - 1);
    }
    else if(mode == 1) {
        for(int r = 0; r < v->cy; r++) erase_cells(v, r, 0, v->cols - 1);
        erase_cells(v, v->cy, 0, v->cx);
    }
    else if(mode == 2) {
        for(int r = 0; r < v->rows; r++) erase_cells(v, r, 0, v->cols - 1);
    }
    else if(mode == 3) {
        /* clear scrollback: blank every line that is not on screen */
        if(!v->alt) {
            for(int i = v->rows; i < v->total; i++) {
                int idx = (v->top + i) % v->total;
                line_clear(v, v->lines[idx], v->bg);
            }
            v->view = 0;
        }
    }
    v->dirty = 1;
}

static void save_cursor(vt_t * v)
{
    if(v->alt) {
        v->alt_cx = v->cx;
        v->alt_cy = v->cy;
    }
    else {
        v->s_cx = v->cx;
        v->s_cy = v->cy;
        v->s_fg = v->fg;
        v->s_bg = v->bg;
        v->s_attr = v->attr;
        v->s_origin = v->origin;
    }
}

static void restore_cursor(vt_t * v)
{
    if(v->alt) {
        v->cx = clampi(v->alt_cx, 0, v->cols - 1);
        v->cy = clampi(v->alt_cy, 0, v->rows - 1);
    }
    else {
        v->cx = clampi(v->s_cx, 0, v->cols - 1);
        v->cy = clampi(v->s_cy, 0, v->rows - 1);
        v->fg = v->s_fg;
        v->bg = v->s_bg;
        v->attr = v->s_attr;
        v->origin = v->s_origin;
    }
    v->wrap_pending = 0;
}

static void reset_screen(vt_t * v)
{
    v->fg = v->def_fg;
    v->bg = v->def_bg;
    v->attr = 0;
    v->insert = 0;
    v->origin = 0;
    v->autowrap = 1;
    v->cur_vis = 1;
    v->cx = v->cy = 0;
    v->wrap_pending = 0;
    v->st_top = 0;
    v->st_bot = v->rows - 1;
    v->g0_graphics = 0;
    v->state = ST_GROUND;
    v->nparam = 0;
    v->cur = 0;
    v->priv = 0;
    v->inter = 0;
    v->utf8_need = 0;
    v->utf8_cp = 0;
    for(int i = 0; i < (int)sizeof(v->tabs); i++) v->tabs[i] = (i % 8) == 0;
    v->tabs[0] = 0;

    for(int i = 0; i < v->total; i++) line_clear(v, v->lines[i], v->def_bg);
    if(v->alt_lines)
        for(int i = 0; i < v->rows; i++) line_clear(v, v->alt_lines[i], v->def_bg);
    v->view = 0;
    v->dirty = 1;
}

static void enter_alt(vt_t * v, int save)
{
    if(v->alt) return;
    if(!v->alt_lines) {
        v->alt_lines = calloc((size_t)v->rows, sizeof(vt_cell_t *));
        for(int i = 0; i < v->rows; i++) v->alt_lines[i] = malloc((size_t)v->cols * sizeof(vt_cell_t));
    }
    if(save) save_cursor(v);
    v->alt = 1;
    v->alt_cx = v->alt_cy = 0;
    v->cx = v->cy = 0;
    v->st_top = 0;
    v->st_bot = v->rows - 1;
    for(int i = 0; i < v->rows; i++) line_clear(v, v->alt_lines[i], v->bg);
    v->dirty = 1;
}

static void leave_alt(vt_t * v, int restore)
{
    if(!v->alt) return;
    v->alt_cx = v->cx;
    v->alt_cy = v->cy;
    v->alt = 0;
    v->st_top = 0;
    v->st_bot = v->rows - 1;
    v->cx = 0;
    v->cy = 0;
    v->view = 0;
    if(restore) restore_cursor(v);
    v->dirty = 1;
}

/* ---------------- SGR ---------------- */

static void sgr(vt_t * v)
{
    if(v->nparam == 0) {
        v->fg = v->def_fg;
        v->bg = v->def_bg;
        v->attr = 0;
        return;
    }

    for(int i = 0; i < v->nparam; i++) {
        int p = v->params[i];
        switch(p) {
            case 0: v->fg = v->def_fg; v->bg = v->def_bg; v->attr = 0; break;
            case 1: v->attr |= VT_ATTR_BOLD; break;
            case 2: v->attr |= VT_ATTR_DIM; break;
            case 3: break;                      /* italic: no font support */
            case 4: v->attr |= VT_ATTR_UNDER; break;
            case 5: case 6: break;              /* blink: ignored */
            case 7: v->attr |= VT_ATTR_REVERSE; break;
            case 8: v->attr |= VT_ATTR_HIDDEN; break;
            case 9: v->attr |= VT_ATTR_UNDER; break;
            case 21: case 22: v->attr &= (uint8_t)~(VT_ATTR_BOLD | VT_ATTR_DIM); break;
            case 23: break;
            case 24: v->attr &= (uint8_t)~VT_ATTR_UNDER; break;
            case 25: break;
            case 27: v->attr &= (uint8_t)~VT_ATTR_REVERSE; break;
            case 28: v->attr &= (uint8_t)~VT_ATTR_HIDDEN; break;
            case 29: v->attr &= (uint8_t)~VT_ATTR_UNDER; break;
            case 39: v->fg = v->def_fg; break;
            case 49: v->bg = v->def_bg; break;
            case 38:
            case 48: {
                uint32_t col = 0;
                int ok = 0;
                if(i + 1 < v->nparam && v->params[i + 1] == 5 && i + 2 < v->nparam) {
                    col = pal256(v->params[i + 2]);
                    i += 2;
                    ok = 1;
                }
                else if(i + 1 < v->nparam && v->params[i + 1] == 2 && i + 4 < v->nparam) {
                    int r = clampi(v->params[i + 2], 0, 255);
                    int g = clampi(v->params[i + 3], 0, 255);
                    int b = clampi(v->params[i + 4], 0, 255);
                    col = ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
                    i += 4;
                    ok = 1;
                }
                if(ok) {
                    if(p == 38) v->fg = col;
                    else v->bg = col;
                }
                break;
            }
            default:
                if(p >= 30 && p <= 37) v->fg = pal256(p - 30);
                else if(p >= 40 && p <= 47) v->bg = pal256(p - 40);
                else if(p >= 90 && p <= 97) v->fg = pal256(p - 90 + 8);
                else if(p >= 100 && p <= 107) v->bg = pal256(p - 100 + 8);
                break;
        }
    }
}

/* ---------------- CSI dispatch ---------------- */

static int pp(vt_t * v, int i, int def)
{
    if(i >= v->nparam) return def;
    return v->params[i] > 0 ? v->params[i] : def;
}

static void csi_dispatch(vt_t * v, uint32_t final)
{
    int n;
    int top_lim = v->origin ? v->st_top : 0;
    int bot_lim = v->origin ? v->st_bot : v->rows - 1;

    switch(final) {
        case '@': { /* ICH */
            n = clampi(pp(v, 0, 1), 1, v->cols);
            if(v->cx + n > v->cols) n = v->cols - v->cx;
            vt_cell_t * r = srow(v, v->cy);
            memmove(r + v->cx + n, r + v->cx, (size_t)(v->cols - v->cx - n) * sizeof(vt_cell_t));
            for(int i = 0; i < n; i++) {
                r[v->cx + i].ch = ' ';
                r[v->cx + i].fg = v->def_fg;
                r[v->cx + i].bg = v->bg;
                r[v->cx + i].attr = 0;
            }
            break;
        }
        case 'A': v->cy = clampi(v->cy - pp(v, 0, 1), top_lim, v->rows - 1); v->wrap_pending = 0; break;
        case 'B': v->cy = clampi(v->cy + pp(v, 0, 1), 0, bot_lim); v->wrap_pending = 0; break;
        case 'C': v->cx = clampi(v->cx + pp(v, 0, 1), 0, v->cols - 1); v->wrap_pending = 0; break;
        case 'D': v->cx = clampi(v->cx - pp(v, 0, 1), 0, v->cols - 1); v->wrap_pending = 0; break;
        case 'E': v->cy = clampi(v->cy + pp(v, 0, 1), 0, bot_lim); v->cx = 0; v->wrap_pending = 0; break;
        case 'F': v->cy = clampi(v->cy - pp(v, 0, 1), top_lim, v->rows - 1); v->cx = 0; v->wrap_pending = 0; break;
        case 'G': v->cx = clampi(pp(v, 0, 1) - 1, 0, v->cols - 1); v->wrap_pending = 0; break;
        case 'H':
        case 'f': {
            int row = pp(v, 0, 1) - 1;
            int col = pp(v, 1, 1) - 1;
            if(v->origin) row += v->st_top;
            v->cy = clampi(row, 0, v->rows - 1);
            v->cx = clampi(col, 0, v->cols - 1);
            v->wrap_pending = 0;
            break;
        }
        case 'I': tab_forward(v, clampi(pp(v, 0, 1), 1, 64)); break;
        case 'J': erase_display(v, pp(v, 0, 0)); break;
        case 'K': {
            int mode = pp(v, 0, 0);
            if(mode == 0) erase_cells(v, v->cy, v->cx, v->cols - 1);
            else if(mode == 1) erase_cells(v, v->cy, 0, v->cx);
            else if(mode == 2) erase_cells(v, v->cy, 0, v->cols - 1);
            break;
        }
        case 'L': { /* IL */
            n = clampi(pp(v, 0, 1), 1, v->rows);
            if(v->cy >= v->st_top && v->cy <= v->st_bot) {
                scroll_down(v, v->cy, v->st_bot, n);
                v->cx = 0;
            }
            break;
        }
        case 'M': { /* DL */
            n = clampi(pp(v, 0, 1), 1, v->rows);
            if(v->cy >= v->st_top && v->cy <= v->st_bot) {
                scroll_up(v, v->cy, v->st_bot, n);
                v->cx = 0;
            }
            break;
        }
        case 'P': { /* DCH */
            n = clampi(pp(v, 0, 1), 1, v->cols);
            if(v->cx + n > v->cols) n = v->cols - v->cx;
            vt_cell_t * r = srow(v, v->cy);
            memmove(r + v->cx, r + v->cx + n, (size_t)(v->cols - v->cx - n) * sizeof(vt_cell_t));
            for(int i = v->cols - n; i < v->cols; i++) {
                r[i].ch = ' ';
                r[i].fg = v->def_fg;
                r[i].bg = v->bg;
                r[i].attr = 0;
            }
            break;
        }
        case 'S': scroll_up(v, v->st_top, v->st_bot, pp(v, 0, 1)); break;
        case 'T': scroll_down(v, v->st_top, v->st_bot, pp(v, 0, 1)); break;
        case 'X': { /* ECH */
            n = clampi(pp(v, 0, 1), 1, v->cols);
            erase_cells(v, v->cy, v->cx, clampi(v->cx + n - 1, 0, v->cols - 1));
            break;
        }
        case 'Z': tab_backward(v, clampi(pp(v, 0, 1), 1, 64)); break;
        case '`': v->cx = clampi(pp(v, 0, 1) - 1, 0, v->cols - 1); v->wrap_pending = 0; break;
        case 'a': v->cx = clampi(v->cx + pp(v, 0, 1), 0, v->cols - 1); v->wrap_pending = 0; break;
        case 'd': v->cy = clampi(pp(v, 0, 1) - 1, 0, v->rows - 1); v->wrap_pending = 0; break;
        case 'e': v->cy = clampi(v->cy + pp(v, 0, 1), 0, v->rows - 1); v->wrap_pending = 0; break;
        case 'g':
            if(pp(v, 0, 0) == 0) v->tabs[v->cx] = 0;
            else if(pp(v, 0, 0) == 3) memset(v->tabs, 0, sizeof(v->tabs));
            break;
        case 'h':
        case 'l': {
            int set = (final == 'h');
            if(v->priv == '?') {
                for(int i = 0; i < v->nparam; i++) {
                    int mode = v->params[i];
                    switch(mode) {
                        case 6: v->origin = set; v->cx = 0; v->cy = 0; break;
                        case 7: v->autowrap = set; break;
                        case 25: v->cur_vis = set; v->dirty = 1; break;
                        case 47:
                        case 1047:
                        case 1049:
                            if(set) enter_alt(v, mode == 1049);
                            else leave_alt(v, mode == 1049);
                            break;
                        case 1048:
                            if(set) save_cursor(v);
                            else restore_cursor(v);
                            break;
                        default: break;   /* mouse, bracketed paste, cursor style: ignored */
                    }
                }
            }
            else {
                for(int i = 0; i < v->nparam; i++)
                    if(v->params[i] == 4) v->insert = set;
            }
            break;
        }
        case 'm': sgr(v); break;
        case 'r': {
            int top = pp(v, 0, 1) - 1;
            int bot = pp(v, 1, v->rows) - 1;
            if(bot > v->rows - 1) bot = v->rows - 1;
            if(top < bot) {
                v->st_top = top;
                v->st_bot = bot;
                v->cx = 0;
                v->cy = v->origin ? top : 0;
                v->wrap_pending = 0;
            }
            break;
        }
        case 's': save_cursor(v); break;
        case 'u': restore_cursor(v); break;
        case 'n': {
            char rep[32];
            int mode = pp(v, 0, 0);
            if(mode == 5) {
                vt_write(v, "\x1b[0n", 4);
            }
            else if(mode == 6) {
                int len = snprintf(rep, sizeof(rep), "\x1b[%d;%dR", v->cy + 1, v->cx + 1);
                vt_write(v, rep, (size_t)len);
            }
            break;
        }
        case 'c':
            vt_write(v, "\x1b[?1;2c", 8);
            break;
        default:
            break;
    }
}

/* ---------------- parser ---------------- */

static void push_param(vt_t * v)
{
    if(v->nparam < PARAM_MAX) {
        v->params[v->nparam] = v->cur;
        v->nparam++;
    }
    v->cur = 0;
}

static void parse_byte(vt_t * v, uint8_t b)
{
    /* UTF-8 continuation handling */
    if(v->utf8_need > 0) {
        if((b & 0xC0) == 0x80) {
            v->utf8_cp = (v->utf8_cp << 6) | (uint32_t)(b & 0x3F);
            if(--v->utf8_need == 0) {
                uint32_t ch = map_unicode(v->utf8_cp);
                put_char(v, ch);
            }
            return;
        }
        v->utf8_need = 0;   /* broken sequence: drop and reprocess byte */
    }

    switch(v->state) {
        case ST_GROUND:
            if(b == 0x1B) {
                v->state = ST_ESC;
                v->nparam = 0;
                v->cur = 0;
                v->priv = 0;
                v->inter = 0;
            }
            else if(b < 0x20) {
                switch(b) {
                    case 0x08:
                        v->wrap_pending = 0;
                        if(v->cx > 0) v->cx--;
                        break;
                    case 0x09: tab_forward(v, 1); break;
                    case 0x0A: case 0x0B: case 0x0C: linefeed(v); break;
                    case 0x0D:
                        v->cx = 0;
                        v->wrap_pending = 0;
                        break;
                    default: break;   /* BEL, NUL, SO/SI: ignored */
                }
            }
            else if(b == 0x7F) {
                /* DEL: ignored */
            }
            else if(b < 0x80) {
                uint32_t ch = b;
                if(v->g0_graphics && b >= 0x60 && b <= 0x7E) ch = map_graphics(ch);
                put_char(v, ch);
            }
            else if((b & 0xE0) == 0xC0) {
                v->utf8_cp = b & 0x1F;
                v->utf8_need = 1;
            }
            else if((b & 0xF0) == 0xE0) {
                v->utf8_cp = b & 0x0F;
                v->utf8_need = 2;
            }
            else if((b & 0xF8) == 0xF0) {
                v->utf8_cp = b & 0x07;
                v->utf8_need = 3;
            }
            else put_char(v, '?');
            break;

        case ST_ESC:
            if(b == '[') v->state = ST_CSI;
            else if(b == ']') v->state = ST_OSC;
            else if(b == 'P' || b == 'X' || b == '^' || b == '_') v->state = ST_STR;
            else if(b == '(' || b == ')' || b == '*' || b == '+') {
                v->inter = b;
                v->state = ST_ESC_INT;
            }
            else {
                v->state = ST_GROUND;
                switch(b) {
                    case '7': save_cursor(v); break;
                    case '8': restore_cursor(v); break;
                    case 'D': linefeed(v); break;
                    case 'E': v->cx = 0; linefeed(v); break;
                    case 'M': reverse_index(v); break;
                    case 'c': reset_screen(v); break;
                    default: break;
                }
            }
            break;

        case ST_ESC_INT:
            v->state = ST_GROUND;
            if(v->inter == '(') v->g0_graphics = (b == '0');
            break;

        case ST_CSI:
            if(b >= '0' && b <= '9') {
                v->cur = v->cur * 10 + (b - '0');
                if(v->cur > 100000) v->cur = 100000;
            }
            else if(b == ';') push_param(v);
            else if(b == '?' || b == '>' || b == '=' || b == '<') v->priv = (int)b;
            else if(b >= 0x20 && b <= 0x2F) {
                v->inter = (int)b;
                v->state = ST_CSI_INT;
            }
            else if(b >= 0x40 && b <= 0x7E) {
                push_param(v);
                v->state = ST_GROUND;
                csi_dispatch(v, b);
                v->nparam = 0;
                v->cur = 0;
                v->priv = 0;
            }
            else if(b == 0x1B) {
                v->state = ST_ESC;
            }
            else if(b < 0x20) {
                /* control inside CSI: execute minimal set */
                if(b == 0x0A) linefeed(v);
                else if(b == 0x0D) v->cx = 0;
                else if(b == 0x08 && v->cx > 0) v->cx--;
            }
            break;

        case ST_CSI_INT:
            if(b >= 0x40 && b <= 0x7E) {
                push_param(v);
                v->state = ST_GROUND;
                csi_dispatch(v, b);
                v->nparam = 0;
                v->cur = 0;
                v->priv = 0;
                v->inter = 0;
            }
            else if(b == 0x1B) v->state = ST_ESC;
            break;

        case ST_OSC:
            if(b == 0x07) v->state = ST_GROUND;
            else if(b == 0x1B) v->state = ST_OSC_ESC;
            break;

        case ST_OSC_ESC:
            v->state = (b == '\\') ? ST_GROUND : ST_OSC;
            break;

        case ST_STR:
            if(b == 0x1B) v->state = ST_STR_ESC;
            break;

        case ST_STR_ESC:
            v->state = (b == '\\') ? ST_GROUND : ST_STR;
            break;

        default:
            v->state = ST_GROUND;
            break;
    }
}

/* ---------------- PTY ---------------- */

static int spawn_shell(vt_t * v)
{
    struct winsize ws;
    memset(&ws, 0, sizeof(ws));
    ws.ws_row = (unsigned short)v->rows;
    ws.ws_col = (unsigned short)v->cols;

    int fd = -1;
    pid_t p = forkpty(&fd, NULL, NULL, &ws);
    if(p < 0) return -1;

    if(p == 0) {
        const char * home = getenv("HOME");
        if(!home || !*home) home = "/";
        chdir(home);
        setenv("TERM", "xterm-256color", 1);
        setenv("COLORTERM", "truecolor", 1);
        setenv("LANG", "C.UTF-8", 1);
        setenv("HOME", home, 1);
        const char * path = getenv("PATH");
        if(!path || !*path)
            setenv("PATH", "/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin", 1);
        const char * sh = access("/bin/bash", X_OK) == 0 ? "/bin/bash" : "/bin/sh";
        execl(sh, sh, "-l", (char *)NULL);
        _exit(127);
    }

    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
    v->fd = fd;
    v->pid = p;
    v->dead = 0;
    return 0;
}

vt_t * vt_create(int cols, int rows, int scrollback)
{
    if(cols < 1 || rows < 1) return NULL;
    if(scrollback < 0) scrollback = 0;

    vt_t * v = calloc(1, sizeof(*v));
    if(!v) return NULL;

    v->cols = cols;
    v->rows = rows;
    v->sb = scrollback;
    v->total = rows + scrollback;
    v->def_fg = 0xD8DEE9;
    v->def_bg = 0x0B0F14;
    v->fd = -1;
    v->pid = -1;

    v->lines = calloc((size_t)v->total, sizeof(vt_cell_t *));
    if(!v->lines) { free(v); return NULL; }
    for(int i = 0; i < v->total; i++) {
        v->lines[i] = malloc((size_t)cols * sizeof(vt_cell_t));
        if(!v->lines[i]) {
            for(int j = 0; j < i; j++) free(v->lines[j]);
            free(v->lines);
            free(v);
            return NULL;
        }
    }

    reset_screen(v);
    spawn_shell(v);
    return v;
}

void vt_destroy(vt_t * v)
{
    if(!v) return;
    if(v->fd >= 0) close(v->fd);
    if(v->pid > 0) kill(v->pid, SIGHUP);
    for(int i = 0; i < v->total; i++) free(v->lines[i]);
    free(v->lines);
    if(v->alt_lines) {
        for(int i = 0; i < v->rows; i++) free(v->alt_lines[i]);
        free(v->alt_lines);
    }
    free(v);
}

int vt_restart(vt_t * v)
{
    if(!v) return -1;
    if(v->fd >= 0) {
        close(v->fd);
        v->fd = -1;
    }
    if(v->pid > 0) {
        kill(v->pid, SIGHUP);
        v->pid = -1;
    }
    v->alt = 0;
    reset_screen(v);
    return spawn_shell(v);
}

int vt_fd(vt_t * v)
{
    return v ? v->fd : -1;
}

int vt_is_dead(vt_t * v)
{
    return v ? v->dead : 1;
}

void vt_write(vt_t * v, const char * s, size_t n)
{
    if(!v || v->fd < 0 || !s) return;
    size_t off = 0;
    int guard = 0;
    while(off < n && guard++ < 64) {
        ssize_t w = write(v->fd, s + off, n - off);
        if(w > 0) off += (size_t)w;
        else if(w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) break;
        else break;
    }
    v->view = 0;
}

int vt_pump(vt_t * v)
{
    if(!v || v->fd < 0) return 0;

    char buf[8192];
    int total = 0;

    while(total < 65536) {
        ssize_t n = read(v->fd, buf, sizeof(buf));
        if(n > 0) {
            for(ssize_t i = 0; i < n; i++) parse_byte(v, (uint8_t)buf[i]);
            total += (int)n;
            continue;
        }
        if(n == 0) {
            v->dead = 1;
            break;
        }
        if(errno == EINTR) continue;
        if(errno == EAGAIN || errno == EWOULDBLOCK) break;
        v->dead = 1;
        break;
    }
    return total;
}

int vt_consume_dirty(vt_t * v)
{
    if(!v) return 0;
    int d = v->dirty;
    v->dirty = 0;
    return d;
}

int vt_cols(vt_t * v)
{
    return v ? v->cols : 0;
}

int vt_rows(vt_t * v)
{
    return v ? v->rows : 0;
}

const vt_cell_t * vt_view_row(vt_t * v, int row)
{
    if(!v) return NULL;
    if(v->alt) return v->alt_lines[row];
    int idx = v->top - v->view + row;
    idx %= v->total;
    if(idx < 0) idx += v->total;
    return v->lines[idx];
}

int vt_cursor_x(vt_t * v)
{
    return v ? v->cx : 0;
}

int vt_cursor_y(vt_t * v)
{
    return v ? v->cy : 0;
}

int vt_cursor_visible(vt_t * v)
{
    return v ? (v->cur_vis && !v->alt) : 0;
}

int vt_view(vt_t * v)
{
    return v ? v->view : 0;
}

void vt_scroll(vt_t * v, int lines)
{
    if(!v) return;
    v->view = clampi(v->view + lines, 0, v->sb);
    v->dirty = 1;
}

void vt_scroll_live(vt_t * v)
{
    if(!v) return;
    v->view = 0;
    v->dirty = 1;
}

uint32_t vt_default_fg(vt_t * v)
{
    return v ? v->def_fg : 0xFFFFFF;
}

uint32_t vt_default_bg(vt_t * v)
{
    return v ? v->def_bg : 0x000000;
}
