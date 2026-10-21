# PMOS kiosk dashboard — agent guide

Kiosk dashboard for a postmarketOS phone (Xiaomi Redmi 5A, `riva`).
It draws directly to `/dev/fb0` with **LVGL 9.6** — no X11, Wayland, or desktop.
The binary is cross-compiled on a laptop for `aarch64-linux-musl` and deployed to the phone.

Three pages: overview, details, and a **terminal** (PTY + VT100 parser + custom keyboard)
so the device can be used without SSH.

This is the **public, sanitized** version of the project. Hostnames, IP addresses,
usernames, and local paths have been replaced with generic placeholders. Read this file before
modifying the UI or adding a new monitored process.

## Hard rules

1. **All UI strings and code comments must be English.**
2. **Do not remove core services**: nginx, cloudflared, tailscale/sshd, podman, kernel/device/firmware, mesa.
3. **Destructive actions need two taps.** Service restart uses a 3 s "TAP AGAIN" window.
   Reboot/shutdown open a confirmation modal; the user must tap the red confirm button.
4. **Build on the laptop, never on the phone.** Deploy the finished static binary.
5. After any change: build, deploy, screenshot, and (for boot-related changes) reboot-test.
6. Never run `pkill -f <path>` over SSH — the pattern matches the SSH shell itself and kills the session.
7. Long-running/background work on the phone must run under systemd (`systemd-run` or a unit);
   SSH sessions are cleaned up by `killUserProcesses=yes`.

## Repository layout

```
pmos-kiosk-dashboard/
├── AGENTS.md              this guide
├── README.md              human quickstart
├── lv_conf.h              LVGL configuration (fonts + UNSCII 16, 1 MB heap, no float printf)
├── src/main.c             dashboard UI (pages 1-3, keyboard, draw hooks)
├── src/vt.c               terminal engine: PTY, VT100/xterm parser, cell grid, scrollback
├── src/vt.h               terminal engine API used by main.c
└── vendor/lvgl/           LVGL 9.6 source (src + include, MIT)
```

This repository contains **only source code and documentation**. Build/deploy helper
scripts, screenshots, and the systemd unit are intentionally not included; the commands
below are the manual equivalents.

## Host facts

| Item | Value |
|---|---|
| SSH | your device IP; examples below use `<device-ip>` |
| Running binary | `~/gui/lvgl-demo` on the phone |
| Backup binary | `~/gui/lvgl-demo.prev` |
| Unit | `/etc/systemd/system/lvgl-gui.service` |
| Pre-start | `/usr/local/bin/lvgl-gui-pre` |
| Display | `/dev/fb0`, 720x1280, 32 bpp, stride **2944** (= 736 px, 16 px padding) |
| Touch | `ft5x06`, auto-detected (normally `/dev/input/event3`) |
| Backlight | `/sys/class/backlight/backlight/brightness`; boot default **51** |
| OS | postmarketOS, Alpine/musl |

## Build

Toolchain: **zig 0.17.x**, target `aarch64-linux-musl`, fully static, stripped.

```sh
ZIG=${ZIG:-zig}
CFLAGS="-target aarch64-linux-musl -O2 -DLV_CONF_INCLUDE_SIMPLE \
        -I. -Ivendor/lvgl/include -ffunction-sections -fdata-sections"
export ZIG CFLAGS

mkdir -p build/obj dist

find vendor/lvgl/src -name '*.c' | xargs -P8 -I{} sh -c '
    f="$1"
    o="build/obj/$(printf "%s" "$f" | sed "s|vendor/lvgl/||; s|[/.]|_|g").o"
    if [ -f "$o" ] && [ "$o" -nt "$f" ] && [ "$o" -nt lv_conf.h ]; then
        exit 0
    fi
    "$ZIG" cc $CFLAGS -c "$f" -o "$o"
' _ {}

"$ZIG" cc $CFLAGS -c src/main.c -o build/obj/main.o
"$ZIG" cc $CFLAGS -c src/vt.c -o build/obj/vt.o
"$ZIG" cc -target aarch64-linux-musl -static -Wl,--gc-sections -Wl,-s \
    -o dist/lvgl-demo build/obj/*.o -lm
```

Output: `dist/lvgl-demo` (~840 KB, ELF aarch64, statically linked, stripped).
The object cache is in `build/obj`; remove it after changing `CFLAGS`.
LVGL is vendored in `vendor/lvgl`; do not add `examples/`, `demos/`, `tests/`, or
`docs/` to the build.

## Deploy

```sh
scp dist/lvgl-demo <device-ip>:~/gui/lvgl-demo.new
ssh <device-ip> 'cp -f ~/gui/lvgl-demo ~/gui/lvgl-demo.prev 2>/dev/null || true
                 sudo install -m 755 ~/gui/lvgl-demo.new ~/gui/lvgl-demo
                 rm -f ~/gui/lvgl-demo.new
                 sudo systemctl restart lvgl-gui'
ssh <device-ip> 'systemctl is-active lvgl-gui'
```

After deploy:

```sh
ssh <device-ip> 'systemctl is-active lvgl-gui; journalctl -u lvgl-gui -n 20 --no-pager'
```

### Systemd unit

`/etc/systemd/system/lvgl-gui.service`:

```ini
[Unit]
Description=LVGL dashboard GUI (kiosk)
After=multi-user.target
ConditionPathExists=/dev/fb0
StartLimitIntervalSec=0

[Service]
Type=simple
ExecStartPre=/usr/local/bin/lvgl-gui-pre
ExecStart=/home/user/gui/lvgl-demo
Restart=always
RestartSec=3
KillSignal=SIGTERM

[Install]
WantedBy=multi-user.target
```

Replace `/home/user` with the phone user's home. `lvgl-gui-pre` waits for `/dev/fb0`
and input devices, stops `fbkeyboard.service`, unbinds `fbcon` from `vtcon1`, unblanks
the display, and hides the console cursor.

### Rollback

- Bad GUI binary: reinstall `~/gui/lvgl-demo.prev` over `~/gui/lvgl-demo`, then
  `systemctl restart lvgl-gui`.
- Bad unit / need console:
  ```sh
  sudo systemctl disable --now lvgl-gui
  sudo systemctl enable --now fbkeyboard
  echo 1 | sudo tee /sys/class/vtconsole/vtcon1/bind
  sudo reboot
  ```

## Verify

### Screenshot

```sh
ssh <device-ip> 'sudo dd if=/dev/fb0 bs=3768320 count=1 2>/dev/null' > /tmp/fb.raw
magick -size 736x1280 -depth 8 bgra:/tmp/fb.raw \
    -crop 720x1280+0+0 +repage /tmp/page1.png
```

The framebuffer is 720 px wide but the stride is 736 px, so the raw frame is
`736 * 1280 * 4 = 3,768,320` bytes and the PNG crop is `720x1280+0+0`.

> The chat/image preview sometimes renders full-page screenshots with apparent "ghost" text.
> **Always verify with a crop** (`magick crop + resize`) or inspect pixel values before
> concluding that the UI is broken. A screenshot taken immediately after a service restart
> can still show the old frame; wait ~2 s or take a second shot.

To show page 2 or 3 without touching the screen, start the binary with `LVGL_PAGE=2` or
`LVGL_PAGE=3` (e.g. `systemd-run --setenv=LVGL_PAGE=3 ...`); normal service startup has no env
and opens page 1.

### Touch injection

Use any evdev injection tool (e.g. a small Python script writing `EV_ABS`/`EV_SYN` events
to `/dev/input/event3`). Useful absolute coordinates (720x1280):

| Target | Center |
|---|---|
| Swipe band (no widgets) | y = 1170, x 650 -> 60 |
| Sleep button | (132, 1034) |
| Reboot button | (360, 1034) |
| Shutdown button | (588, 1034) |
| Power modal — Cancel | (225, 724) |
| Power modal — Confirm | (495, 724) — **this really reboots/powers off** |
| Page 2 — service row 2, col 1 | (132, 795) |
| Page 2 — Tap test | (360, 885) |
| Page 2 — Refresh | (588, 885) |
| Brightness slider knob (51/255) | (134, 693) |
| Page 3 — status label | (300, 38) |
| Page 3 — letter keys | row 3 y = 1124, x = 30 + 60 * index (`a`=30 ... `l`=510) |
| Page 3 — Ctrl / Shift / Sym / Space | (240, 953) / (30, 1181) / (60, 1238) / (270, 1238) |
| Page 3 — Enter / Bksp | (660, 1124) / (660, 1010) |

### Reboot checklist

```sh
ssh <device-ip> 'cat /sys/class/backlight/backlight/brightness'      # 51
ssh <device-ip> 'systemctl is-active lvgl-gui; systemctl --failed --no-legend | wc -l'  # active, 0
ssh <device-ip> 'systemctl is-active nginx cloudflared uptime-kuma beszel filebrowser tailscaled sshd'
```

Also verify your public endpoints and Uptime Kuma monitors separately.

## UI architecture (`src/main.c`)

- Screen: 720x1280, bg `#0B0F14`, cards `#141B24`, accent `#4FC3F7`.
- `lv_tileview` with three tiles: `page1` (col 0), `page2` (col 1), `page3` (col 2); horizontal swipe.
  The tileview owns scrolling; page content uses absolute coordinates.
- Brightness slider disables `LV_OBJ_FLAG_SCROLLABLE` on the tileview on `PRESSED`
  and restores it on `RELEASED`/`PRESS_LOST`, so dragging the slider never changes pages.
- Page dots are separate objects on the screen layer (above the tileview), clickable.
  They are hidden while page 3 is active because the on-screen keyboard occupies that strip.
- **Alert banner** (`banner`, y 0..52) is hidden normally. `ui_tick` shows it on:
  monitored service DOWN, `failed_units > 0`, or SoC temp > 60 C. It overlaps the title row by design.
- **Power modal** (`power_modal`) is hidden normally. Tapping Reboot/Shutdown
  opens it; it dims the screen, shows Cancel/Confirm, and auto-cancels after 5 s.
- **Sleep mode**: Tapping **Sleep** turns off backlight (`brightness = 0`) and covers the screen
  with `sleep_overlay` on `lv_layer_sys()`. Double-tapping anywhere on the screen wakes it up,
  restoring the previous brightness level.
- Fonts: Montserrat 14/16/20/24/28/48, plus `term_font` (Fira Code Nerd Font Mono 16 px, 4-bpp).
- `LV_USE_FLOAT` is **0**: `lv_label_set_text_fmt()` cannot print floats (it prints `f`).
  Always use the local `label_fmt()` (libc `vsnprintf`) for `%f` formats.

The public source uses a generic title (`Server`) instead of a personal device name.

### Page 1 layout

| Element | Position / size |
|---|---|
| Title / subtitle / badge | (24,22) / (106,30) / top-right |
| Clock / date / net | (24,58) 48 px / (26,124) / (26,158) |
| Cards CPU, RAM, TEMP, BATTERY | (24,196), (368,196), (24,360), (368,360); 328x148 |
| Services label / 3 buttons | y 520 / y 544, 216x88 at x 24, 252, 480 |
| Brightness label / slider | y 644 / (24,668) 672x50, range 10..255 |
| CPU history card / chart | (24,734) 672x242 / child (0,24) 644x186 |
| Sleep / Reboot / Shutdown | y 992, 3x 216x84 at x 24, 252, 480 |
| Health strip | y 1090, 3x 216x68 at x 24, 252, 480 |
| Footer / dots | y 1180 / (341,1240), (355,1240), (369,1240) 10x10 |

### Page 2 layout

| Element | Position / size |
|---|---|
| Title "Details" / back hint | (24,22) / top-right |
| NETWORK card | (24,64) 672x180 |
| BATTERY card | (24,260) 672x160 |
| STORAGE & UPDATES card | (24,436) 672x190 |
| Services label / grid | y 642 / start (24,666), 3x 216x78, gaps 12 |
| Grid cells | 0-6 services, 7 Tap test, 8 Refresh |
| SYSTEM card | (24,938) 672x150 |
| Footer | y 1120 |

### Page 3 layout (terminal)

| Element | Position / size |
|---|---|
| Title "Terminal" / status label | (24,22) 24 px / (156,30) 14 px, clickable |
| Hint "swipe right to go back" | top-right (24 px inset) |
| Terminal view (`term_view`) | (0,56) 720x760, drawn manually in `LV_EVENT_DRAW_MAIN` |
| Keyboard panel (`kb_panel`) | (0,824) 720x456, bg `#0F1620` |
| Keys | Phone layout, 7 rows (48/48/58/64/64/64/64 px), 5 px gaps, 6 px margins |

Terminal geometry constants in `main.c`: `TCELL_W 16`, `TCELL_H 28`, `TCOLS 45`, `TROWS 27`.
The PTY is opened with the same size, so the shell, `nano`, `htop` and `tmux` all see 72x40.

Draw path: `term_draw_cb()` walks the visible rows, merges adjacent cells with the same
(fg, bg, attr), fills non-default backgrounds with `lv_draw_fill()` and draws each run with
`lv_draw_label()` + `term_font` (Fira Code Nerd Font Mono 16 px, 4-bpp anti-aliased).
Bold is simulated with a second 1 px shifted draw; reverse/dim/hidden/underline are applied
by swapping colors or using `lv_text_decor_t`. The cursor is a filled block drawn last and
blinks via `term_blink_cb` (500 ms), invalidating only the cursor cell.

`src/vt.c` implements the PTY (`forkpty`), the parser and the grid:

- C0 controls, CSI cursor/erase/insert/delete/scroll, SGR (16/256/true color, bold, underline,
  reverse, dim, hidden), tab stops, scroll regions, save/restore cursor.
- Alternate screen for `1049`/`1047`/`47` so `nano`/`htop` restore the shell on exit.
- 400 lines of scrollback in a ring buffer; `vt_view_row()` maps the visible row through the
  view offset, `vt_scroll()` moves it.
- Full UTF-8 rendering for embedded glyph ranges (box drawing, blocks, arrows, powerline,
  nerd font symbols); DEC Special Graphics maps directly to Unicode box characters.
- `SIGCHLD` is `SIG_IGN` in `main()`, so the shell is reaped automatically; EOF/EIO on the PTY
  master marks it dead and the status label offers a restart.

Interactions:

- Any key writes bytes to the PTY master (`vt_write`); `Ctrl`/`Alt`/`Shift` are sticky and
  reset after the next character key. `Ctrl` maps `a-z` to `0x01..0x1A` (`Ctrl+C`, `Ctrl+D`, ...).
  Every key highlights on touch (`LV_STATE_PRESSED` bg `#3B5570`, white text).
- Vertical drag on the terminal scrolls the scrollback; the tileview scroll flag is cleared
  during the drag and restored on release, so horizontal swipes still change pages.
- The status label shows `72x40 | bash`, `history -N lines - tap for live` while scrolled, and
  `shell exited - tap here to restart` (red) when the shell dies. Tapping it jumps to live or
  restarts a dead shell; entering page 3 also restarts a dead shell.

### Keyboard layout (`kb_panel`)

Weighted phone keyboard, 708 px usable width (6 px side margins, 5 px key gaps):

| Row | Height | Keys (weights in tenths of column) |
|---|---|---|
| F1 | 48 px | Esc (10) `/` (10) `-` (10) Home (10) `^` (10) End (10) PgUp (10) |
| F2 | 48 px | Tab (10) Ctrl (10) Alt (10) `<` (10) `v` (10) `>` (10) PgDn (10) |
| 1 | 58 px | `1..0` (10 keys, 10 each) |
| 2 | 64 px | `q..p` (10 keys, 10 each) |
| 3 | 64 px | `a..l` (9 keys across full width, 10 each) |
| 4 | 64 px | Shift (15) `z..m` (7 keys, 10 each) Bksp (15) |
| 5 | 64 px | `?123` (15) `,` (10) Space (45) `.` (10) Enter (20) |

`?123` switches rows 1-3 to symbols (digits -> `!@#$%^&*()`, qwerty -> `` ` ~ \ { } [ ] < > = ``,
asdf -> `- _ + : ; ' " | /`); row 4 remains letters (`z..m`) and the toggle label becomes `ABC`.
Bksp and the arrows auto-repeat on long press (`LV_EVENT_LONG_PRESSED_REPEAT`).

Key centers (720x1280): row y = 856 (F1), 909 (F2), 967 (R1), 1033 (R2), 1102 (R3), 1171 (R4), 1240 (R5):

| Key | Center (x, y) |
|---|---|
| F1: Esc / `/` / `-` / Home / `^` / End / PgUp | (57,856) (158,856) (259,856) (360,856) (461,856) (562,856) (663,856) |
| F2: Tab / Ctrl / Alt / `<` / `v` / `>` / PgDn | (57,909) (158,909) (259,909) (360,909) (461,909) (562,909) (663,909) |
| digits `1`..`0` (row 1) | (41,967) + 70.8 per key |
| `q`..`p` (row 2) | (41,1033) + 70.8 per key |
| `a`..`l` (row 3, 9 keys) | (45,1102) + 78.7 per key |
| Shift / `z`..`m` / Bksp (row 4) | (59,1171) / (148,1171) + 70.8 / (661,1171) |
| `?123` / `,` / Space / `.` / Enter (row 5) | (59,1240) / (148,1240) / (342,1240) / (537,1240) / (643,1240) |

### Power modal

- Overlay 720x1280, black 70 %.
- Card (60,460) 600x360: title 28 px, description 14 px, hint, Cancel + Confirm 250x80.
- `power_open(action)` sets text/confirm label, shows, raises, starts a 5 s one-shot timer.
- `power_confirm_cb()` executes `systemctl reboot` / `systemctl poweroff` via fork+exec.
- There is no one-tap path: the first tap only opens the modal.

## Timers

| Timer | Period | Work |
|---|---|---|
| `ui_tick` | 1 s | clock/date, CPU %, RAM, SoC temp, battery %, chart, disk %, load, alert banner, uptime/network label |
| `svc_tick` | 2.5 s | one `systemctl is-active` call for all 7 units; updates page 1 + page 2 labels |
| `net_tick` | 5 s | `iw dev wlan0 link`, IP, RX/TX average since previous tick |
| `health_tick` | 5 s | failed units, CPU freq, zram, disk `/` + `/boot`, journal size, kuma.db size, updates label, system card |
| `slow_tick` | 300 s | `apk version -l '<'`, journal size, kuma.db size |
| `term_tick` | 33 ms | drain the PTY, parse output, invalidate the terminal when it changed |
| `term_blink_cb` | 500 ms | cursor blink (invalidates one 16x17 cell) |
| power timer | 5 s one-shot | close the modal |

Service labels are updated by one `popen` per 2.5 s:
`/usr/bin/systemctl is-active nginx cloudflared uptime-kuma beszel filebrowser tailscaled sshd`.

## Services and data sources

Monitored units (`svc_units[]`, order matters for the 3x3 grid):
`nginx`, `cloudflared`, `uptime-kuma`, `beszel`, `filebrowser`, `tailscaled`, `sshd`.

| Metric | Source |
|---|---|
| CPU % | `/proc/stat` delta |
| RAM | `/proc/meminfo` (`MemTotal`, `MemAvailable`) |
| SoC temp | `/sys/class/thermal/thermal_zone0/temp` (milli-C) |
| Battery | `/sys/class/power_supply/bq27426-0/{capacity,status,voltage_now,current_now,temp}` |
| Disk | `statvfs("/")`, `statvfs("/boot")` |
| Load | `/proc/loadavg` |
| WiFi | `iw dev wlan0 link` (SSID, signal, freq); channel = derived |
| Traffic | `/sys/class/net/wlan0/statistics/{rx,tx}_bytes` |
| CPU freq | `/sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq` (kHz) |
| ZRAM | `/sys/block/zram0/disksize`, `/sys/block/zram0/mm_stat` (3rd field = used bytes) |
| Failed units | `systemctl --failed --no-legend \| wc -l` |
| Updates | `/usr/sbin/apk version -l '<' \| wc -l` |
| journal size | `/usr/sbin/du -sk /var/log/journal` |
| Kuma DB size | `stat()` of `kuma.db`, `-wal`, `-shm` |
| Uptime/kernel | `/proc/uptime`, `uname()` |

Uptime Kuma DB check (read-only, adjust the path if yours differs):

```sh
ssh <device-ip> "sudo python3 - <<'EOF'
import sqlite3
c = sqlite3.connect('file:/var/lib/uptime-kuma/kuma.db?mode=ro', uri=True)
for n, s in c.execute('select m.name, (select h.status from heartbeat h where h.monitor_id=m.id order by h.time desc limit 1) from monitor m'):
    print(n, s)
EOF"
```

## Common tasks

### Add a new metric

1. Add a global label pointer near the other page-2 labels.
2. Create it in `build_page2()` inside the relevant card (use `mk_label` + `label_fmt`).
3. Read the value in `health_tick()` (slow data) or `ui_tick()` (fast data).
4. If it should raise an alert, update the alert block at the end of `ui_tick()`.
5. Build, deploy, screenshot, crop and verify.
6. Move cards down / resize them in the layout table above if space is needed.

### Add a monitored service

1. Add the unit to `svc_units[]` and increase `NSVC`.
2. The `svc_tick` popen command is hard-coded — add the unit there in the same order.
3. The grid has 9 cells; with 7 services only 2 are left (Tap test, Refresh). If you add more,
   shrink `gh`/`gy` or add a fourth row (update page-2 layout constants).
4. Deploy and verify each label shows UP.

### Add a new standalone process/service on the phone

1. Write the unit under `/etc/systemd/system/<name>.service` with `Restart=always` if appropriate.
2. `sudo systemctl daemon-reload && sudo systemctl enable --now <name>`.
3. Verify: `systemctl is-active <name>`, `journalctl -u <name> -n 50`.
4. Only add it to the GUI if it is safe to expose/restart from the kiosk.
5. Reboot-test; confirm `systemctl --failed` stays 0.

### Change colors/fonts

- Colors are `C_*` defines at the top of `main.c`.
- Fonts must exist in `lv_conf.h` (currently 14/20/24/28/48 plus `LV_FONT_UNSCII_16`).
- Text styles use `lv_obj_set_style_text_color(obj, lv_color_hex(C_...), 0)`.

### Add or change a terminal key

1. `KB_DEF[row][index]` at the top of the page-3 block holds `{span, kind, label, shift, sym, symshift}`.
   Each row must add up to 12 units.
2. `kind` is one of `K_CHAR`, `K_ESC`, `K_TAB`, `K_ENTER`, `K_BS`, `K_DEL`, `K_UP`..`K_RIGHT`,
   `K_CTRL`, `K_ALT`, `K_SHIFT`, `K_SYM`. Special keys send fixed bytes in `kb_key_cb()`.
3. Update the key-center table above, rebuild, deploy, and verify by tapping the new key.
4. `TCOLS`/`TROWS` live next to `TCELL_W`/`TCELL_H`; changing them resizes the PTY on next start.

## Known gotchas

- **Float formatting**: `LV_USE_FLOAT 0`; use `label_fmt()`.
- **Framebuffer stride**: 736 px, visible 720 px. Cropping is mandatory.
- **Do not trust full-page screenshot previews** in chat; crop before judging.
- **Tileview vs slider**: keep the press/release scroll toggles if you move the slider.
- **Alert banner** covers the title row while shown (intentional).
- **Time zone**: set your local zone with `timedatectl set-timezone <zone>`. On this device
  the PMIC RTC rejects `RTC_SET_TIME` (ENODEV), so at boot the kernel starts at 1970-01-06
  and `systemd-timesyncd` corrects it once WiFi is up. `ui_tick` shows
  `--:--:-- / waiting for time sync` while the year is below 2025.
- **Brightness** is set to 51 at boot; the GUI reads the current value on startup and
  writes changes to sysfs.
- **Terminal font and Unicode**: `term_font` is Fira Code Nerd Font Mono (16 px, 4-bpp).
  It includes ASCII, Latin-1, arrows, box drawing (0x2500-0x257F), block elements (0x2580-0x259F),
  geometric shapes, powerline glyphs (0xE0A0-0xE0D7), and devicons. `vt.c` maps DEC Special Graphics
  directly to Unicode box characters, and preserves supported UTF-8 codepoints in the cell grid.
- **Terminal runs as the service user** (root when the unit has no `User=`). The shell is
  `/bin/bash -l` when available, otherwise `/bin/sh -l`.
- **Do not add a second `SIGCHLD` handler**: `main()` sets `SIGCHLD` to `SIG_IGN`.
- **Idle cost on page 3**: ~0.5 % CPU (5 ticks / 10 s) with a quiet shell, ~1.9 % with `htop`
  running.
- **GUI memory** is ~2-3 MB RSS; CPU is around 1-3 % on this device.
