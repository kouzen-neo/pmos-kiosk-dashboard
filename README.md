# PMOS Kiosk Dashboard

Kiosk dashboard built with **LVGL 9.6** for postmarketOS devices
(tested on Xiaomi Redmi 5A / `riva`). It renders directly to `/dev/fb0` with no desktop
environment. The binary is a static `aarch64-linux-musl` executable (~930 KB).

This is the public repository. All device names, network addresses, usernames,
and local paths use generic placeholders.

## Repository contents

| Path | Purpose |
|---|---|
| `src/main.c` | Dashboard UI: pages 1-3, custom keyboard, draw hooks |
| `src/vt.c`, `src/vt.h` | Terminal engine: PTY, VT100/xterm parser, cell grid, scrollback |
| `src/fonts/` | Generated fonts (Fira Code Nerd Font Mono 16 px) |
| `lv_conf.h` | LVGL configuration (Montserrat 14/16/20/24/28/48, 1 MB heap) |
| `vendor/lvgl/` | Vendored LVGL 9.6 source code (MIT license) |
| `AGENTS.md` | Detailed technical architecture and developer guide |

This repository contains only source code and documentation. Build and deployment
helper scripts are omitted; manual commands are documented below.

## Build

Requirements: **zig 0.17.x** (or compatible cross-compiler).

```sh
ZIG=${ZIG:-zig}
CFLAGS="-target aarch64-linux-musl -O2 -DLV_CONF_INCLUDE_SIMPLE \
        -I. -Ivendor/lvgl/include -ffunction-sections -fdata-sections"
export ZIG CFLAGS

mkdir -p build/obj dist

find vendor/lvgl/src -name '*.c' | xargs -P8 -I{} sh -c '
    f="$1"
    o="build/obj/$(printf "%s" "$f" | sed "s|vendor/lvgl/||; s|[/.]|_|g").o"
    if [ -f "$o" ] && [ "$o" -nt "$f" ] && [ "$o" -nt lv_conf.h ]; then exit 0; fi
    "$ZIG" cc $CFLAGS -c "$f" -o "$o"
' _ {}

"$ZIG" cc $CFLAGS -c src/main.c -o build/obj/main.o
"$ZIG" cc -target aarch64-linux-musl -static -Wl,--gc-sections -Wl,-s \
    -o dist/lvgl-demo build/obj/*.o -lm
```

Output: `dist/lvgl-demo` (static, stripped binary).

## Deploy

Replace `<device-ip>` with your device IP address (e.g. `192.168.1.50` or configured SSH host):

```sh
scp dist/lvgl-demo <device-ip>:~/gui/lvgl-demo.new
ssh <device-ip> 'cp -f ~/gui/lvgl-demo ~/gui/lvgl-demo.prev 2>/dev/null || true
                 sudo install -m 755 ~/gui/lvgl-demo.new ~/gui/lvgl-demo
                 rm -f ~/gui/lvgl-demo.new
                 sudo systemctl restart lvgl-gui'
ssh <device-ip> 'systemctl is-active lvgl-gui'
```

## Rollback

To restore the previous binary:

```sh
ssh <device-ip> 'sudo install -m 755 ~/gui/lvgl-demo.prev ~/gui/lvgl-demo && sudo systemctl restart lvgl-gui'
```

To switch back to a virtual console:

```sh
ssh <device-ip> 'sudo systemctl disable --now lvgl-gui; sudo systemctl enable --now fbkeyboard'
ssh <device-ip> 'echo 1 | sudo tee /sys/class/vtconsole/vtcon1/bind; sudo reboot'
```

## Framebuffer capture

```sh
ssh <device-ip> 'sudo dd if=/dev/fb0 bs=3768320 count=1 2>/dev/null' > /tmp/fb.raw
magick -size 736x1280 -depth 8 bgra:/tmp/fb.raw \
    -crop 720x1280+0+0 +repage /tmp/page1.png
```

Framebuffer resolution is 720x1280, stride 736 px (2944 bytes per scanline), 32 bpp.

## UI navigation

- **Swipe left** on page 1: details (page 2), then terminal (page 3). **Swipe right** to go back.
- **Page dots** at the bottom also switch pages (they are hidden on page 3).
- **Sleep**: Tap **Sleep** to turn off the screen; double-tap anywhere on the screen while dark to wake.
- **Service restart**: Tap twice (3-second confirmation window).
- **Reboot / Shutdown**: Tap button to open confirmation modal, then tap **Confirm**.
  The modal auto-cancels after 5 seconds; a **Cancel** button is also available.
- **Brightness**: Drag slider (tileview scrolling is locked during drag).
- **Refresh**: Available on page 2 to refresh network, storage, and service stats immediately.

## Terminal (page 3)

Page 3 is a real shell on a PTY (72x40 cells, 400 lines of scrollback), so `htop`, `nano`,
`top`, and `tmux` work with colors, cursor addressing, and the alternate screen. Everything is
drawn by the GUI; no SSH or virtual console is involved.

- The on-screen keyboard follows a phone layout with two function rows (`Esc / - Home ^ End PgUp`
  and `Tab Ctrl Alt < v > PgDn`), digits `1..0`, 3 letter rows (`q..p`, `a..l`, `Shift z..m Bksp`),
  and bottom row `?123 , Space . Enter`.
- Keys feature visual press feedback (highlight on touch) and weighted widths.
- `Ctrl`/`Alt`/`Shift` are sticky and reset after the next character key. `Ctrl`-`C` sends a real
  SIGINT, `Ctrl`-`D` sends EOF.
- Drag the terminal **vertically** to scroll back; tap the status label to jump back to live.
- The status label also reports `shell exited - tap here to restart`; tapping it, or swiping
  back to page 3, starts a fresh shell.
- The shell runs as the same user as the GUI service and starts as `/bin/bash -l` when available,
  otherwise `/bin/sh -l`.
- Terminal font is **Fira Code Nerd Font Mono** (16 px, 4-bpp anti-aliased), with support
  for box-drawing, block elements, arrows, powerline glyphs, and nerd font symbols.

## Notes

- Set device timezone using `timedatectl set-timezone <zone>`. On devices where PMIC RTC
  is read-only, the clock displays `waiting for time sync` during early boot until NTP synchronizes.
- See `AGENTS.md` for architecture details, coordinate maps, timers, and systemd units.
