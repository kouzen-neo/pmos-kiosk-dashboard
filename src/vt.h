/* vt.h - minimal VT100/xterm terminal emulator with a PTY backend.
 *
 * The frontend (LVGL) only needs the cell grid: it draws vt_view_row() for
 * every visible row and pushes key presses through vt_write().
 */
#ifndef VT_H
#define VT_H

#include <stddef.h>
#include <stdint.h>

typedef struct vt vt_t;

/* Cell attributes */
#define VT_ATTR_BOLD     0x01
#define VT_ATTR_DIM      0x02
#define VT_ATTR_UNDER    0x04
#define VT_ATTR_REVERSE  0x08
#define VT_ATTR_HIDDEN   0x10

typedef struct {
    uint32_t ch;    /* Unicode code point, always printable ASCII in practice */
    uint32_t fg;    /* 0xRRGGBB */
    uint32_t bg;    /* 0xRRGGBB */
    uint8_t attr;   /* VT_ATTR_* flags */
} vt_cell_t;

vt_t * vt_create(int cols, int rows, int scrollback);
void   vt_destroy(vt_t * v);

/* PTY / child process */
int    vt_fd(vt_t * v);
int    vt_is_dead(vt_t * v);
int    vt_restart(vt_t * v);
void   vt_write(vt_t * v, const char * s, size_t n);

/* Read pending output and parse it. Returns the number of bytes consumed. */
int    vt_pump(vt_t * v);

/* Set when the visible grid changed; cleared by the call. */
int    vt_consume_dirty(vt_t * v);

/* Grid access */
int    vt_cols(vt_t * v);
int    vt_rows(vt_t * v);
const vt_cell_t * vt_view_row(vt_t * v, int row);

int    vt_cursor_x(vt_t * v);
int    vt_cursor_y(vt_t * v);
int    vt_cursor_visible(vt_t * v);

/* Scrollback view: 0 = live, N = N lines back. */
int    vt_view(vt_t * v);
void   vt_scroll(vt_t * v, int lines);
void   vt_scroll_live(vt_t * v);

uint32_t vt_default_fg(vt_t * v);
uint32_t vt_default_bg(vt_t * v);

#endif /* VT_H */
