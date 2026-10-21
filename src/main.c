/* PMOS kiosk dashboard for Redmi 5A (riva)
 * LVGL 9.6 + fbdev + evdev, cross-compiled for aarch64-linux-musl (static).
 * Page 1: overview   Page 2: details   Page 3: terminal
 * Swipe or tap the page dots to switch.
 */
#include "lvgl/lvgl.h"
#include "lvgl/drivers/display/lv_linux_fbdev.h"
#include "lvgl/drivers/indev/lv_evdev.h"

#include "vt.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <signal.h>
#include <fcntl.h>
#include <dirent.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/ioctl.h>
#include <sys/utsname.h>
#include <linux/input.h>
#include <ifaddrs.h>
#include <arpa/inet.h>
#include <netinet/in.h>

#define C_BG      0x0B0F14
#define C_CARD    0x141B24
#define C_BTN     0x1B2735
#define C_KEY_DOWN 0x3B5570
#define C_TEXT    0xE6EDF3
#define C_MUTED   0x8B98A8
#define C_ACCENT  0x4FC3F7
#define C_GREEN   0x4ADE80
#define C_AMBER   0xFBBF24
#define C_RED     0xF87171
#define C_DARKRED 0x7F1D1D

#define M 24
#define CARD_W 328
#define CARD_H 148

#define NSVC 7
static const char * svc_units[NSVC] = {
    "nginx", "cloudflared", "uptime-kuma", "beszel",
    "filebrowser", "tailscaled", "sshd"
};

/* page 1 */
static lv_obj_t * lbl_clock, * lbl_date, * lbl_net;
static lv_obj_t * lbl_cpu, * lbl_ram, * lbl_temp, * lbl_batt;
static lv_obj_t * lbl_bright, * lbl_footer1;
static lv_obj_t * lbl_hdisk, * lbl_hfail, * lbl_hload;
static lv_obj_t * chart;
static lv_chart_series_t * ser_cpu;

/* page 2 */
static lv_obj_t * lbl_ssid, * lbl_netinfo, * lbl_netrate;
static lv_obj_t * lbl_batt2, * lbl_battinfo;
static lv_obj_t * lbl_disk1, * lbl_disk2, * lbl_journal, * lbl_updates;
static lv_obj_t * lbl_sys1, * lbl_sys2, * lbl_sys3, * lbl_footer2;
static lv_obj_t * lbl_touch;

/* chrome */
static lv_obj_t * tv, * page1, * page2, * page3;
static lv_obj_t * dot1, * dot2, * dot3;
static lv_obj_t * banner, * lbl_alert;

static lv_obj_t * svc_state[NSVC];
static lv_obj_t * svc_state1[3];
static int svc_up[NSVC];
static int svc_armed[NSVC];
static lv_obj_t * btn_sleep, * btn_reboot, * btn_shutdown;
static lv_obj_t * bright_slider = NULL;
static lv_obj_t * sleep_overlay = NULL;
static int is_asleep = 0;
static int saved_brightness = 51;
static uint32_t last_sleep_tap_time = 0;

static lv_obj_t * power_modal, * power_title, * power_text;
static lv_obj_t * power_confirm, * power_confirm_lbl;
static lv_timer_t * power_timer = NULL;
static int power_action = 0;

static int touch_count = 0;
static int failed_units = 0;
static int temp_c = 0;
static int updatable = 0;

static double journal_mb = 0, kumadb_mb = 0;
static double rx_rate = 0, tx_rate = 0;
static unsigned long long prev_rx = 0, prev_tx = 0;
static struct timespec prev_net_ts;

static char ssid[64] = "offline";
static int rssi = 0, chan = 0;
static int cpu_mhz = 0;
static unsigned long long zram_used = 0, zram_total = 0;

static unsigned long long prev_idle = 0, prev_total = 0;

static char touch_path[64] = "/dev/input/event3";

static vt_t * term = NULL;
static lv_obj_t * term_view, * term_status, * kb_panel;

static void term_status_update(void);
static void term_restart(void);

static void svc_tick(lv_timer_t * t);
static void health_tick(lv_timer_t * t);
static void net_tick(lv_timer_t * t);

/* ---------------- util ---------------- */

static uint32_t tick_cb(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000u + ts.tv_nsec / 1000000u);
}

static int read_int_file(const char * path, int def)
{
    FILE * f = fopen(path, "r");
    int v;
    if(!f) return def;
    if(fscanf(f, "%d", &v) != 1) v = def;
    fclose(f);
    return v;
}

static unsigned long long read_u64_file(const char * path, unsigned long long def)
{
    FILE * f = fopen(path, "r");
    unsigned long long v;
    if(!f) return def;
    if(fscanf(f, "%llu", &v) != 1) v = def;
    fclose(f);
    return v;
}

static void read_line_file(const char * path, char * out, size_t n, const char * def)
{
    FILE * f = fopen(path, "r");
    if(!f) { snprintf(out, n, "%s", def); return; }
    if(!fgets(out, n, f)) snprintf(out, n, "%s", def);
    size_t l = strlen(out);
    while(l > 0 && (out[l - 1] == '\n' || out[l - 1] == ' ')) out[--l] = '\0';
    fclose(f);
}

static double cpu_percent(void)
{
    FILE * f = fopen("/proc/stat", "r");
    char line[256];
    unsigned long long u = 0, n = 0, s = 0, i = 0, w = 0, irq = 0, sq = 0, st = 0;
    if(!f) return 0;
    if(!fgets(line, sizeof(line), f)) { fclose(f); return 0; }
    fclose(f);
    if(sscanf(line, "cpu %llu %llu %llu %llu %llu %llu %llu %llu",
              &u, &n, &s, &i, &w, &irq, &sq, &st) < 4) return 0;

    unsigned long long idle  = i + w;
    unsigned long long total = u + n + s + i + w + irq + sq + st;
    unsigned long long d_idle  = idle  - prev_idle;
    unsigned long long d_total = total - prev_total;
    prev_idle = idle;
    prev_total = total;
    if(d_total == 0 || d_idle > d_total) return 0;
    return 100.0 * (double)(d_total - d_idle) / (double)d_total;
}

static void mem_used(long * used_mb, long * total_mb)
{
    FILE * f = fopen("/proc/meminfo", "r");
    char key[64];
    long v, total = 0, avail = 0;
    if(!f) { *used_mb = 0; *total_mb = 0; return; }
    while(fscanf(f, "%63s %ld %*s", key, &v) == 2) {
        if(strcmp(key, "MemTotal:") == 0) total = v;
        else if(strcmp(key, "MemAvailable:") == 0) avail = v;
    }
    fclose(f);
    *total_mb = total / 1024;
    *used_mb  = (total - avail) / 1024;
}

static void fmt_uptime(char * out, size_t n)
{
    double up = 0;
    FILE * f = fopen("/proc/uptime", "r");
    if(f) { if(fscanf(f, "%lf", &up) != 1) up = 0; fclose(f); }
    int s = (int)up;
    int d = s / 86400, h = (s % 86400) / 3600, m = (s % 3600) / 60;
    if(d > 0) snprintf(out, n, "%dd %dh %dm", d, h, m);
    else      snprintf(out, n, "%dh %dm", h, m);
}

static void get_ip(char * out, size_t n)
{
    struct ifaddrs * ifa = NULL, * p = NULL;
    out[0] = '\0';
    if(getifaddrs(&ifa) != 0) return;
    for(p = ifa; p; p = p->ifa_next) {
        if(!p->ifa_addr) continue;
        if(p->ifa_addr->sa_family == AF_INET && strcmp(p->ifa_name, "wlan0") == 0) {
            inet_ntop(AF_INET, &((struct sockaddr_in *)p->ifa_addr)->sin_addr, out, n);
            break;
        }
    }
    freeifaddrs(ifa);
}

static void disk_info(const char * mnt, double * used_mb, double * total_mb, int * pct)
{
    struct statvfs s;
    if(statvfs(mnt, &s) != 0) { *used_mb = *total_mb = 0; *pct = 0; return; }
    double total = (double)s.f_blocks * (double)s.f_frsize / (1024.0 * 1024.0);
    double freeb = (double)s.f_bfree  * (double)s.f_frsize / (1024.0 * 1024.0);
    *total_mb = total;
    *used_mb  = total - freeb;
    *pct = total > 0 ? (int)(((total - freeb) * 100.0 / total) + 0.5) : 0;
}

static void brightness_set(int v)
{
    FILE * f = fopen("/sys/class/backlight/backlight/brightness", "w");
    if(!f) { perror("brightness"); return; }
    fprintf(f, "%d\n", v);
    fclose(f);
}

static void run_async(char * const argv[])
{
    pid_t p = fork();
    if(p == 0) {
        execv(argv[0], argv);
        _exit(127);
    }
}

static void find_touch(void)
{
    DIR * d = opendir("/dev/input");
    struct dirent * e;
    if(!d) return;
    while((e = readdir(d)) != NULL) {
        if(strncmp(e->d_name, "event", 5) != 0) continue;
        char p[64];
        snprintf(p, sizeof(p), "/dev/input/%s", e->d_name);
        int fd = open(p, O_RDONLY | O_NONBLOCK);
        if(fd < 0) continue;
        unsigned long absbits[(ABS_MAX / 8) + 1] = {0};
        if(ioctl(fd, EVIOCGBIT(EV_ABS, sizeof(absbits)), absbits) >= 0) {
            int has_x = (absbits[ABS_X / 8] & (1u << (ABS_X % 8))) != 0;
            int has_y = (absbits[ABS_Y / 8] & (1u << (ABS_Y % 8))) != 0;
            char name[64] = {0};
            ioctl(fd, EVIOCGNAME(sizeof(name)), name);
            if(has_x && has_y && strstr(name, "ft5x06")) {
                snprintf(touch_path, sizeof(touch_path), "%s", p);
                close(fd);
                break;
            }
        }
        close(fd);
    }
    closedir(d);
}

static void fmt_rate(char * out, size_t n, double bps)
{
    if(bps >= 1048576.0)      snprintf(out, n, "%.1f MB/s", bps / 1048576.0);
    else if(bps >= 1024.0)    snprintf(out, n, "%.1f KB/s", bps / 1024.0);
    else                      snprintf(out, n, "%.0f B/s", bps);
}

/* ---------------- system queries ---------------- */

static int map_state(const char * s)
{
    if(strncmp(s, "active", 6) == 0) return 1;
    if(strncmp(s, "activating", 10) == 0) return 2;
    if(strncmp(s, "reloading", 9) == 0) return 2;
    if(strncmp(s, "deactivating", 12) == 0) return 2;
    if(strncmp(s, "inactive", 8) == 0) return 0;
    if(strncmp(s, "failed", 6) == 0) return 0;
    return -1;
}

static void count_failed(void)
{
    FILE * f = popen("/usr/bin/systemctl --failed --no-legend 2>/dev/null | wc -l", "r");
    int n = 0;
    if(f) {
        if(fscanf(f, "%d", &n) != 1) n = 0;
        pclose(f);
    }
    failed_units = n;
}

static void check_updates(void)
{
    FILE * f = popen("/usr/sbin/apk version -l '<' 2>/dev/null | wc -l", "r");
    int n = 0;
    if(f) {
        if(fscanf(f, "%d", &n) != 1) n = 0;
        pclose(f);
    }
    updatable = n;
}

static void check_journal(void)
{
    FILE * f = popen("/usr/sbin/du -sk /var/log/journal 2>/dev/null", "r");
    long kb = 0;
    if(f) {
        if(fscanf(f, "%ld", &kb) != 1) kb = 0;
        pclose(f);
    }
    journal_mb = (double)kb / 1024.0;
}

static void check_kuma_db(void)
{
    const char * files[] = {
        "/var/lib/uptime-kuma/kuma.db",
        "/var/lib/uptime-kuma/kuma.db-wal",
        "/var/lib/uptime-kuma/kuma.db-shm"
    };
    double total = 0;
    for(int i = 0; i < 3; i++) {
        struct stat st;
        if(stat(files[i], &st) == 0) total += (double)st.st_size;
    }
    kumadb_mb = total / (1024.0 * 1024.0);
}

static void check_cpu_zram(void)
{
    cpu_mhz = read_int_file("/sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq", 0) / 1000;

    zram_total = read_u64_file("/sys/block/zram0/disksize", 0);
    zram_used = 0;
    FILE * f = fopen("/sys/block/zram0/mm_stat", "r");
    if(f) {
        unsigned long long orig = 0, compr = 0, used = 0;
        if(fscanf(f, "%llu %llu %llu", &orig, &compr, &used) == 3) zram_used = used;
        fclose(f);
    }
}

static void check_wifi(void)
{
    FILE * f = popen("/usr/sbin/iw dev wlan0 link 2>/dev/null", "r");
    char line[256];
    snprintf(ssid, sizeof(ssid), "offline");
    rssi = 0;
    chan = 0;
    double freq = 0;
    if(f) {
        while(fgets(line, sizeof(line), f)) {
            char * p;
            if((p = strstr(line, "SSID:")) != NULL) {
                p += 5;
                while(*p == ' ') p++;
                snprintf(ssid, sizeof(ssid), "%s", p);
                size_t l = strlen(ssid);
                while(l > 0 && (ssid[l - 1] == '\n' || ssid[l - 1] == ' ')) ssid[--l] = '\0';
            }
            else if((p = strstr(line, "signal:")) != NULL) {
                sscanf(p, "signal: %d", &rssi);
            }
            else if((p = strstr(line, "freq:")) != NULL) {
                sscanf(p, "freq: %lf", &freq);
            }
        }
        pclose(f);
    }
    if(freq > 0) chan = freq < 3000 ? (int)((freq - 2407) / 5) : (int)((freq - 5000) / 5);
}

static void read_net_counters(void)
{
    unsigned long long rx = read_u64_file("/sys/class/net/wlan0/statistics/rx_bytes", 0);
    unsigned long long tx = read_u64_file("/sys/class/net/wlan0/statistics/tx_bytes", 0);
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);

    double dt = (double)(now.tv_sec - prev_net_ts.tv_sec) +
                (double)(now.tv_nsec - prev_net_ts.tv_nsec) / 1e9;
    if(prev_net_ts.tv_sec != 0 && dt > 0.2) {
        rx_rate = (double)(rx - prev_rx) / dt;
        tx_rate = (double)(tx - prev_tx) / dt;
    }
    prev_rx = rx;
    prev_tx = tx;
    prev_net_ts = now;
}

/* ---------------- widgets ---------------- */

static lv_obj_t * mk_label(lv_obj_t * parent, const char * txt, const lv_font_t * font, uint32_t color)
{
    lv_obj_t * l = lv_label_create(parent);
    lv_label_set_text(l, txt);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    return l;
}

static void label_fmt(lv_obj_t * l, const char * fmt, ...)
{
    char b[192];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b, sizeof(b), fmt, ap);
    va_end(ap);
    lv_label_set_text(l, b);
}

static lv_obj_t * mk_card(lv_obj_t * parent, int x, int y, int w, int h, const char * title)
{
    lv_obj_t * c = lv_obj_create(parent);
    lv_obj_remove_style_all(c);
    lv_obj_set_pos(c, x, y);
    lv_obj_set_size(c, w, h);
    lv_obj_set_style_bg_color(c, lv_color_hex(C_CARD), 0);
    lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(c, 20, 0);
    lv_obj_set_style_pad_all(c, 14, 0);
    if(title) {
        lv_obj_t * t = mk_label(c, title, &lv_font_montserrat_14, C_MUTED);
        lv_obj_align(t, LV_ALIGN_TOP_LEFT, 0, 0);
    }
    return c;
}

static lv_obj_t * mk_button(lv_obj_t * parent, int x, int y, int w, int h,
                            const char * txt, const lv_font_t * font, uint32_t fg)
{
    lv_obj_t * b = lv_button_create(parent);
    lv_obj_remove_style_all(b);
    lv_obj_set_pos(b, x, y);
    lv_obj_set_size(b, w, h);
    lv_obj_set_style_bg_color(b, lv_color_hex(C_BTN), 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(b, 18, 0);
    lv_obj_t * l = mk_label(b, txt, font, fg);
    lv_obj_center(l);
    return b;
}

/* ---------------- events ---------------- */

static void flash_reset_cb(lv_timer_t * t)
{
    lv_obj_t * btn = (lv_obj_t *)lv_timer_get_user_data(t);
    lv_obj_set_style_bg_color(btn, lv_color_hex(C_BTN), 0);
    lv_timer_delete(t);
}

static void touch_cb(lv_event_t * e)
{
    lv_obj_t * btn = (lv_obj_t *)lv_event_get_target(e);
    touch_count++;
    label_fmt(lbl_touch, "Tap test: %d", touch_count);
    lv_obj_set_style_bg_color(btn, lv_color_hex(C_GREEN), 0);
    lv_timer_create(flash_reset_cb, 200, btn);
}

static void slider_press_cb(lv_event_t * e)
{
    (void)e;
    lv_obj_clear_flag(tv, LV_OBJ_FLAG_SCROLLABLE);
}

static void slider_release_cb(lv_event_t * e)
{
    (void)e;
    lv_obj_add_flag(tv, LV_OBJ_FLAG_SCROLLABLE);
}

static void bright_cb(lv_event_t * e)
{
    lv_obj_t * sl = (lv_obj_t *)lv_event_get_target(e);
    int v = (int)lv_slider_get_value(sl);
    label_fmt(lbl_bright, "Brightness: %d / 255", v);
    brightness_set(v);
}

static void svc_reset_cb(lv_timer_t * t)
{
    int i = (int)(intptr_t)lv_timer_get_user_data(t);
    svc_armed[i] = 0;
    lv_timer_delete(t);
}

static void svc_cb(lv_event_t * e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);

    if(!svc_armed[i]) {
        svc_armed[i] = 1;
        lv_label_set_text(svc_state[i], "TAP AGAIN");
        lv_obj_set_style_text_color(svc_state[i], lv_color_hex(C_AMBER), 0);
        lv_timer_create(svc_reset_cb, 3000, (void *)(intptr_t)i);
        return;
    }

    svc_armed[i] = 0;
    lv_label_set_text(svc_state[i], "RESTART...");
    lv_obj_set_style_text_color(svc_state[i], lv_color_hex(C_AMBER), 0);

    char * argv[] = {"/usr/bin/systemctl", "restart", (char *)svc_units[i], NULL};
    run_async(argv);
}

static void power_close(void)
{
    if(power_timer) {
        lv_timer_delete(power_timer);
        power_timer = NULL;
    }
    power_action = 0;
    lv_obj_add_flag(power_modal, LV_OBJ_FLAG_HIDDEN);
}

static void power_timeout_cb(lv_timer_t * t)
{
    power_timer = NULL;
    lv_timer_delete(t);
    power_close();
}

static void power_open(int action)
{
    power_action = action;

    if(action == 1) {
        lv_label_set_text(power_title, "Reboot server?");
        lv_label_set_text(power_text, "The server will restart. All services go down briefly.");
        lv_label_set_text(power_confirm_lbl, "Reboot");
    }
    else {
        lv_label_set_text(power_title, "Shutdown server?");
        lv_label_set_text(power_text, "The server will power off and must be started manually.");
        lv_label_set_text(power_confirm_lbl, "Shutdown");
    }

    lv_obj_set_style_bg_color(power_confirm, lv_color_hex(C_DARKRED), 0);
    lv_obj_remove_flag(power_modal, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(power_modal);

    if(power_timer) lv_timer_delete(power_timer);
    power_timer = lv_timer_create(power_timeout_cb, 5000, NULL);
}

static void power_confirm_cb(lv_event_t * e)
{
    (void)e;
    int act = power_action;
    power_close();

    if(act == 1) {
        char * argv[] = {"/usr/bin/systemctl", "reboot", NULL};
        run_async(argv);
    }
    else if(act == 2) {
        char * argv[] = {"/usr/bin/systemctl", "poweroff", NULL};
        run_async(argv);
    }
}

static void power_cancel_cb(lv_event_t * e)
{
    (void)e;
    power_close();
}

static void reboot_cb(lv_event_t * e)
{
    (void)e;
    power_open(1);
}

static void shutdown_cb(lv_event_t * e)
{
    (void)e;
    power_open(2);
}

static void screen_wake(void)
{
    if(!is_asleep) return;
    is_asleep = 0;
    if(saved_brightness < 10) saved_brightness = 51;
    brightness_set(saved_brightness);
    if(sleep_overlay) lv_obj_add_flag(sleep_overlay, LV_OBJ_FLAG_HIDDEN);
    if(lbl_bright) label_fmt(lbl_bright, "Brightness: %d / 255  (drag)", saved_brightness);
    if(bright_slider) lv_slider_set_value(bright_slider, saved_brightness, LV_ANIM_OFF);
}

static void screen_sleep(void)
{
    if(is_asleep) return;
    int cur = read_int_file("/sys/class/backlight/backlight/brightness", 51);
    if(cur >= 10) saved_brightness = cur;
    else if(saved_brightness < 10) saved_brightness = 51;

    is_asleep = 1;
    last_sleep_tap_time = 0;
    power_close();
    brightness_set(0);
    if(sleep_overlay) {
        lv_obj_remove_flag(sleep_overlay, LV_OBJ_FLAG_HIDDEN);
    }
}

static void sleep_cb(lv_event_t * e)
{
    (void)e;
    screen_sleep();
}

static void sleep_wake_cb(lv_event_t * e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if(code == LV_EVENT_CLICKED) {
        uint32_t now = lv_tick_get();
        if(last_sleep_tap_time > 0 && (now - last_sleep_tap_time >= 40) && (now - last_sleep_tap_time <= 800)) {
            screen_wake();
            last_sleep_tap_time = 0;
        }
        else {
            last_sleep_tap_time = now;
        }
    }
}

static void tv_changed_cb(lv_event_t * e)
{
    (void)e;
    lv_obj_t * act = lv_tileview_get_tile_active(tv);
    int idx = (act == page3) ? 2 : (act == page2 ? 1 : 0);

    lv_obj_t * dots[3] = {dot1, dot2, dot3};
    for(int i = 0; i < 3; i++) {
        if(!dots[i]) continue;
        lv_obj_set_style_bg_color(dots[i], lv_color_hex(i == idx ? C_ACCENT : 0x334155), 0);
        /* the dots would overlap the on-screen keyboard on page 3 */
        if(idx == 2) lv_obj_add_flag(dots[i], LV_OBJ_FLAG_HIDDEN);
        else lv_obj_remove_flag(dots[i], LV_OBJ_FLAG_HIDDEN);
    }

    if(idx == 2 && term) {
        if(vt_is_dead(term)) term_restart();
        term_status_update();
        if(term_view) lv_obj_invalidate(term_view);
    }
}

static void dot_cb(lv_event_t * e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    lv_tileview_set_tile_by_index(tv, (uint32_t)idx, 0, LV_ANIM_ON);
}

static void refresh_cb(lv_event_t * e)
{
    (void)e;
    check_wifi();
    check_cpu_zram();
    check_journal();
    check_kuma_db();
    check_updates();
    count_failed();
    svc_tick(NULL);
    health_tick(NULL);
    net_tick(NULL);
    lv_label_set_text(lbl_touch, "Refreshed");
}

/* ---------------- periodic updates ---------------- */

static void set_svc_label(lv_obj_t * l, int st)
{
    if(!l) return;
    if(st == 1) {
        lv_label_set_text(l, "UP");
        lv_obj_set_style_text_color(l, lv_color_hex(C_GREEN), 0);
    }
    else if(st == 2) {
        lv_label_set_text(l, "SYNC");
        lv_obj_set_style_text_color(l, lv_color_hex(C_AMBER), 0);
    }
    else if(st == 0) {
        lv_label_set_text(l, "DOWN");
        lv_obj_set_style_text_color(l, lv_color_hex(C_RED), 0);
    }
    else {
        lv_label_set_text(l, "--");
        lv_obj_set_style_text_color(l, lv_color_hex(C_MUTED), 0);
    }
}

static void svc_tick(lv_timer_t * t)
{
    (void)t;
    FILE * f = popen("/usr/bin/systemctl is-active nginx cloudflared uptime-kuma "
                      "beszel filebrowser tailscaled sshd 2>/dev/null", "r");
    char line[64];
    for(int i = 0; i < NSVC; i++) {
        int st = -1;
        if(f && fgets(line, sizeof(line), f)) st = map_state(line);
        svc_up[i] = st;
        set_svc_label(svc_state[i], st);
        if(i < 3) set_svc_label(svc_state1[i], st);
    }
    if(f) pclose(f);
}

static void net_tick(lv_timer_t * t)
{
    (void)t;
    read_net_counters();
    check_wifi();
    char b[128];
    char ip[32];
    get_ip(ip, sizeof(ip));
    snprintf(b, sizeof(b), "IP %s   |   RSSI %d dBm   |   ch %d",
             ip[0] ? ip : "-", rssi, chan);
    lv_label_set_text(lbl_netinfo, b);
    lv_label_set_text(lbl_ssid, ssid);
    lv_obj_set_style_text_color(lbl_ssid,
        lv_color_hex(strcmp(ssid, "offline") == 0 ? C_RED : C_TEXT), 0);

    char rr[32], tr[32];
    fmt_rate(rr, sizeof(rr), rx_rate);
    fmt_rate(tr, sizeof(tr), tx_rate);
    snprintf(b, sizeof(b), "RX %s   |   TX %s", rr, tr);
    lv_label_set_text(lbl_netrate, b);
}

static void health_tick(lv_timer_t * t)
{
    (void)t;
    count_failed();
    check_cpu_zram();

    double used = 0, total = 0;
    int pct = 0;
    disk_info("/", &used, &total, &pct);

    label_fmt(lbl_hdisk, "%d%%", pct);
    lv_obj_set_style_text_color(lbl_hdisk, lv_color_hex(pct >= 80 ? C_AMBER : C_TEXT), 0);

    label_fmt(lbl_hfail, "%d", failed_units);
    lv_obj_set_style_text_color(lbl_hfail, lv_color_hex(failed_units ? C_RED : C_GREEN), 0);

    double bu = 0, bt = 0;
    int bp = 0;
    disk_info("/boot", &bu, &bt, &bp);
    label_fmt(lbl_disk1, "/   %.1f / %.1f GB   (%d%%)",
                          used / 1024.0, total / 1024.0, pct);
    label_fmt(lbl_disk2, "/boot   %.0f / %.0f MB   (%d%%)", bu, bt, bp);

    label_fmt(lbl_journal, "journal %.0f MB   |   kuma.db %.1f MB",
                          journal_mb, kumadb_mb);

    if(updatable > 0) {
        label_fmt(lbl_updates, "%d packages upgradable", updatable);
        lv_obj_set_style_text_color(lbl_updates, lv_color_hex(C_AMBER), 0);
    }
    else {
        lv_label_set_text(lbl_updates, "All packages up to date");
        lv_obj_set_style_text_color(lbl_updates, lv_color_hex(C_GREEN), 0);
    }

    struct utsname un;
    char up[32];
    fmt_uptime(up, sizeof(up));
    if(uname(&un) == 0)
        label_fmt(lbl_sys1, "kernel %s   |   up %s", un.release, up);
    else
        label_fmt(lbl_sys1, "up %s", up);

    double l1 = 0, l5 = 0, l15 = 0;
    FILE * lf = fopen("/proc/loadavg", "r");
    if(lf) { if(fscanf(lf, "%lf %lf %lf", &l1, &l5, &l15) != 3) { l1 = l5 = l15 = 0; } fclose(lf); }
    label_fmt(lbl_sys2, "CPU %.2f GHz   |   load %.2f %.2f %.2f",
                          cpu_mhz / 1000.0, l1, l5, l15);
    label_fmt(lbl_sys3, "ZRAM %.0f MB / %.0f MB",
                          zram_used / (1024.0 * 1024.0), zram_total / (1024.0 * 1024.0));
}

static void slow_tick(lv_timer_t * t)
{
    (void)t;
    check_updates();
    check_journal();
    check_kuma_db();
}

static void ui_tick(lv_timer_t * t)
{
    (void)t;
    static const char * DAYS[]   = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
    static const char * MONTHS[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                    "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    char b[128];
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);

    /* The PMIC RTC cannot be written, so boot starts at 1970 until NTP syncs. */
    if(tm.tm_year + 1900 < 2025) {
        lv_label_set_text(lbl_clock, "--:--:--");
        lv_label_set_text(lbl_date, "waiting for time sync");
    }
    else {
        strftime(b, sizeof(b), "%H:%M:%S", &tm);
        lv_label_set_text(lbl_clock, b);

        snprintf(b, sizeof(b), "%s, %02d %s %d", DAYS[tm.tm_wday], tm.tm_mday,
                 MONTHS[tm.tm_mon], tm.tm_year + 1900);
        lv_label_set_text(lbl_date, b);
    }

    char ip[32], up[32];
    get_ip(ip, sizeof(ip));
    fmt_uptime(up, sizeof(up));
    snprintf(b, sizeof(b), "wlan0 %s  |  up %s", ip[0] ? ip : "offline", up);
    lv_label_set_text(lbl_net, b);

    int cpu = (int)(cpu_percent() + 0.5);
    label_fmt(lbl_cpu, "%d%%", cpu);
    lv_chart_set_next_value(chart, ser_cpu, cpu);

    long used = 0, total = 0;
    mem_used(&used, &total);
    label_fmt(lbl_ram, "%ld / %ld MB", used, total);

    int mt = read_int_file("/sys/class/thermal/thermal_zone0/temp", 0);
    temp_c = mt / 1000;
    label_fmt(lbl_temp, "%d.%d C", mt / 1000, (mt % 1000) / 100);

    int cap = read_int_file("/sys/class/power_supply/bq27426-0/capacity", -1);
    if(cap < 0) {
        lv_label_set_text(lbl_batt, "--");
    }
    else {
        label_fmt(lbl_batt, "%d%%", cap);
        lv_obj_set_style_text_color(lbl_batt,
            lv_color_hex(cap < 20 ? C_RED : (cap < 50 ? C_AMBER : C_GREEN)), 0);
    }

    /* battery details */
    char status[32];
    read_line_file("/sys/class/power_supply/bq27426-0/status", status, sizeof(status), "-");
    int volt = read_int_file("/sys/class/power_supply/bq27426-0/voltage_now", 0);
    int curr = read_int_file("/sys/class/power_supply/bq27426-0/current_now", 0);
    int btemp = read_int_file("/sys/class/power_supply/bq27426-0/temp", 0);

    if(cap >= 0) label_fmt(lbl_batt2, "%d%%   %s", cap, status);
    else         lv_label_set_text(lbl_batt2, status);

    label_fmt(lbl_battinfo, "%.2f V   |   %d mA   |   %.1f C",
                          volt / 1e6, curr / 1000, btemp / 10.0);

    /* health strip */
    double du = 0, dt = 0;
    int dpct = 0;
    disk_info("/", &du, &dt, &dpct);
    label_fmt(lbl_hdisk, "%d%%", dpct);
    lv_obj_set_style_text_color(lbl_hdisk, lv_color_hex(dpct >= 80 ? C_AMBER : C_TEXT), 0);

    double l1 = 0, l5 = 0, l15 = 0;
    FILE * lf = fopen("/proc/loadavg", "r");
    if(lf) { if(fscanf(lf, "%lf %lf %lf", &l1, &l5, &l15) != 3) { l1 = l5 = l15 = 0; } fclose(lf); }
    label_fmt(lbl_hload, "%.2f", l1);

    /* alert banner */
    char msg[128] = "";
    for(int i = 0; i < NSVC; i++) {
        if(svc_up[i] == 0) {
            snprintf(msg, sizeof(msg), "ALERT: %s DOWN", svc_units[i]);
            break;
        }
    }
    if(msg[0] == '\0' && failed_units > 0)
        snprintf(msg, sizeof(msg), "ALERT: %d failed unit%s", failed_units,
                 failed_units > 1 ? "s" : "");
    if(msg[0] == '\0' && temp_c > 60)
        snprintf(msg, sizeof(msg), "ALERT: CPU temperature %d C", temp_c);

    if(msg[0]) {
        lv_label_set_text(lbl_alert, msg);
        lv_obj_remove_flag(banner, LV_OBJ_FLAG_HIDDEN);
    }
    else {
        lv_obj_add_flag(banner, LV_OBJ_FLAG_HIDDEN);
    }
}

/* ---------------- pages ---------------- */

static void build_page1(void)
{
    lv_obj_t * p = page1;

    lv_obj_t * title = mk_label(p, "Server", &lv_font_montserrat_24, C_ACCENT);
    lv_obj_set_pos(title, M, 22);
    lv_obj_t * sub = mk_label(p, "Redmi 5A | riva", &lv_font_montserrat_14, C_MUTED);
    lv_obj_set_pos(sub, M + 82, 30);

    lv_obj_t * badge = mk_label(p, "SERVER PANEL", &lv_font_montserrat_14, C_MUTED);
    lv_obj_align(badge, LV_ALIGN_TOP_RIGHT, -M, 26);

    lbl_clock = mk_label(p, "--:--:--", &lv_font_montserrat_48, C_TEXT);
    lv_obj_set_pos(lbl_clock, M, 58);

    lbl_date = mk_label(p, "", &lv_font_montserrat_20, C_MUTED);
    lv_obj_set_pos(lbl_date, M + 2, 124);

    lbl_net = mk_label(p, "", &lv_font_montserrat_14, C_MUTED);
    lv_obj_set_pos(lbl_net, M + 2, 158);

    lv_obj_t * cards[4];
    cards[0] = mk_card(p, M,               196, CARD_W, CARD_H, "CPU");
    cards[1] = mk_card(p, M + CARD_W + 16, 196, CARD_W, CARD_H, "RAM");
    cards[2] = mk_card(p, M,               360, CARD_W, CARD_H, "TEMP");
    cards[3] = mk_card(p, M + CARD_W + 16, 360, CARD_W, CARD_H, "BATTERY");

    lv_obj_t * vals[4];
    for(int i = 0; i < 4; i++) {
        vals[i] = mk_label(cards[i], "--", &lv_font_montserrat_28, C_TEXT);
        lv_obj_align(vals[i], LV_ALIGN_BOTTOM_LEFT, 0, 0);
    }
    lbl_cpu  = vals[0];
    lbl_ram  = vals[1];
    lbl_temp = vals[2];
    lbl_batt = vals[3];

    lv_obj_t * svc_label = mk_label(p, "SERVICES  (tap twice to restart)",
                                    &lv_font_montserrat_14, C_MUTED);
    lv_obj_set_pos(svc_label, M + 2, 520);

    for(int i = 0; i < 3; i++) {
        lv_obj_t * b = lv_button_create(p);
        lv_obj_remove_style_all(b);
        lv_obj_set_pos(b, M + i * (216 + 12), 544);
        lv_obj_set_size(b, 216, 88);
        lv_obj_set_style_bg_color(b, lv_color_hex(C_CARD), 0);
        lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(b, 18, 0);
        lv_obj_set_flex_flow(b, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(b, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_row(b, 2, 0);
        lv_obj_add_event_cb(b, svc_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);

        mk_label(b, svc_units[i], &lv_font_montserrat_20, C_TEXT);
        svc_state1[i] = mk_label(b, "--", &lv_font_montserrat_14, C_MUTED);
    }

    int bcur = read_int_file("/sys/class/backlight/backlight/brightness", 128);
    lbl_bright = mk_label(p, "", &lv_font_montserrat_14, C_MUTED);
    lv_obj_set_pos(lbl_bright, M + 2, 644);
    label_fmt(lbl_bright, "Brightness: %d / 255  (drag)", bcur);

    bright_slider = lv_slider_create(p);
    lv_obj_set_pos(bright_slider, M, 668);
    lv_obj_set_size(bright_slider, 672, 50);
    lv_slider_set_range(bright_slider, 10, 255);
    lv_slider_set_value(bright_slider, bcur, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(bright_slider, lv_color_hex(0x1E293B), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(bright_slider, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(bright_slider, 25, LV_PART_MAIN);
    lv_obj_set_style_bg_color(bright_slider, lv_color_hex(C_ACCENT), LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(bright_slider, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_radius(bright_slider, 25, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(bright_slider, lv_color_hex(C_TEXT), LV_PART_KNOB);
    lv_obj_set_style_bg_opa(bright_slider, LV_OPA_COVER, LV_PART_KNOB);
    lv_obj_set_style_radius(bright_slider, LV_RADIUS_CIRCLE, LV_PART_KNOB);
    lv_obj_set_style_width(bright_slider, 40, LV_PART_KNOB);
    lv_obj_set_style_height(bright_slider, 40, LV_PART_KNOB);
    lv_obj_add_event_cb(bright_slider, bright_cb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(bright_slider, slider_press_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(bright_slider, slider_release_cb, LV_EVENT_RELEASED, NULL);
    lv_obj_add_event_cb(bright_slider, slider_release_cb, LV_EVENT_PRESS_LOST, NULL);

    lv_obj_t * chart_card = mk_card(p, M, 734, 672, 242, "CPU HISTORY  (last 60 s)");
    chart = lv_chart_create(chart_card);
    lv_obj_remove_style_all(chart);
    lv_obj_set_pos(chart, 0, 24);
    lv_obj_set_size(chart, 644, 186);
    lv_obj_set_style_bg_opa(chart, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(chart, 0, 0);
    lv_chart_set_type(chart, LV_CHART_TYPE_LINE);
    lv_chart_set_point_count(chart, 60);
    lv_chart_set_range(chart, LV_CHART_AXIS_PRIMARY_Y, 0, 100);
    lv_chart_set_div_line_count(chart, 4, 0);
    lv_obj_set_style_line_width(chart, 3, LV_PART_ITEMS);
    lv_obj_set_style_size(chart, 0, 0, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(chart, LV_OPA_TRANSP, LV_PART_INDICATOR);
    lv_obj_set_style_line_width(chart, 1, LV_PART_MAIN);
    lv_obj_set_style_line_color(chart, lv_color_hex(0x223040), LV_PART_MAIN);
    ser_cpu = lv_chart_add_series(chart, lv_color_hex(C_ACCENT), LV_CHART_AXIS_PRIMARY_Y);
    lv_chart_set_all_values(chart, ser_cpu, LV_CHART_POINT_NONE);

    btn_sleep = mk_button(p, M, 992, 216, 84, "Sleep",
                          &lv_font_montserrat_20, C_ACCENT);
    lv_obj_add_event_cb(btn_sleep, sleep_cb, LV_EVENT_CLICKED, NULL);

    btn_reboot = mk_button(p, M + 216 + 12, 992, 216, 84, "Reboot",
                           &lv_font_montserrat_20, C_AMBER);
    lv_obj_add_event_cb(btn_reboot, reboot_cb, LV_EVENT_CLICKED, NULL);

    btn_shutdown = mk_button(p, M + (216 + 12) * 2, 992, 216, 84, "Shutdown",
                             &lv_font_montserrat_20, C_RED);
    lv_obj_add_event_cb(btn_shutdown, shutdown_cb, LV_EVENT_CLICKED, NULL);

    /* health strip */
    int hx[3] = {M, M + 216 + 12, M + (216 + 12) * 2};
    const char * htitle[3] = {"DISK", "FAILED", "LOAD"};
    for(int i = 0; i < 3; i++) {
        lv_obj_t * c = mk_card(p, hx[i], 1090, 216, 68, NULL);
        lv_obj_set_style_pad_all(c, 10, 0);
        lv_obj_t * t = mk_label(c, htitle[i], &lv_font_montserrat_14, C_MUTED);
        lv_obj_align(t, LV_ALIGN_TOP_LEFT, 0, 0);
        lv_obj_t * v = mk_label(c, "--", &lv_font_montserrat_20, C_TEXT);
        lv_obj_align(v, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
        if(i == 0) lbl_hdisk = v;
        else if(i == 1) lbl_hfail = v;
        else lbl_hload = v;
    }

    lbl_footer1 = mk_label(p, "LVGL 9.6.0 | aarch64-linux-musl | swipe left: details, terminal",
                           &lv_font_montserrat_14, C_MUTED);
    lv_obj_set_pos(lbl_footer1, M, 1180);
    lv_obj_set_width(lbl_footer1, 672);
    lv_obj_set_style_text_align(lbl_footer1, LV_TEXT_ALIGN_CENTER, 0);
}

static void build_page2(void)
{
    lv_obj_t * p = page2;

    lv_obj_t * title = mk_label(p, "Details", &lv_font_montserrat_24, C_ACCENT);
    lv_obj_set_pos(title, M, 22);
    lv_obj_t * hint = mk_label(p, "swipe right to go back", &lv_font_montserrat_14, C_MUTED);
    lv_obj_align(hint, LV_ALIGN_TOP_RIGHT, -M, 28);

    /* network */
    lv_obj_t * nc = mk_card(p, M, 64, 672, 180, "NETWORK");
    lbl_ssid = mk_label(nc, "offline", &lv_font_montserrat_20, C_TEXT);
    lv_obj_set_pos(lbl_ssid, 0, 32);
    lbl_netinfo = mk_label(nc, "", &lv_font_montserrat_14, C_MUTED);
    lv_obj_set_pos(lbl_netinfo, 0, 66);
    lbl_netrate = mk_label(nc, "", &lv_font_montserrat_14, C_MUTED);
    lv_obj_set_pos(lbl_netrate, 0, 92);

    /* battery */
    lv_obj_t * bc = mk_card(p, M, 260, 672, 160, "BATTERY");
    lbl_batt2 = mk_label(bc, "--", &lv_font_montserrat_20, C_TEXT);
    lv_obj_set_pos(lbl_batt2, 0, 32);
    lbl_battinfo = mk_label(bc, "", &lv_font_montserrat_14, C_MUTED);
    lv_obj_set_pos(lbl_battinfo, 0, 66);

    /* storage + updates */
    lv_obj_t * sc = mk_card(p, M, 436, 672, 190, "STORAGE & UPDATES");
    lbl_disk1 = mk_label(sc, "--", &lv_font_montserrat_20, C_TEXT);
    lv_obj_set_pos(lbl_disk1, 0, 32);
    lbl_disk2 = mk_label(sc, "", &lv_font_montserrat_14, C_MUTED);
    lv_obj_set_pos(lbl_disk2, 0, 66);
    lbl_journal = mk_label(sc, "", &lv_font_montserrat_14, C_MUTED);
    lv_obj_set_pos(lbl_journal, 0, 92);
    lbl_updates = mk_label(sc, "", &lv_font_montserrat_20, C_AMBER);
    lv_obj_set_pos(lbl_updates, 0, 126);

    /* services grid */
    lv_obj_t * svc_label = mk_label(p, "SERVICES  (tap twice to restart)",
                                    &lv_font_montserrat_14, C_MUTED);
    lv_obj_set_pos(svc_label, M + 2, 642);

    int gw = 216, gh = 78, gx = 12, gy = 12;
    for(int i = 0; i < NSVC; i++) {
        int col = i % 3, row = i / 3;
        lv_obj_t * b = lv_button_create(p);
        lv_obj_remove_style_all(b);
        lv_obj_set_pos(b, M + col * (gw + gx), 666 + row * (gh + gy));
        lv_obj_set_size(b, gw, gh);
        lv_obj_set_style_bg_color(b, lv_color_hex(C_CARD), 0);
        lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(b, 16, 0);
        lv_obj_set_flex_flow(b, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(b, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_row(b, 1, 0);
        lv_obj_add_event_cb(b, svc_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);

        mk_label(b, svc_units[i], &lv_font_montserrat_20, C_TEXT);
        svc_state[i] = mk_label(b, "--", &lv_font_montserrat_14, C_MUTED);
    }

    /* tap test + refresh */
    lv_obj_t * tb = mk_button(p, M + 1 * (gw + gx), 666 + 2 * (gh + gy), gw, gh,
                              "Tap test: 0", &lv_font_montserrat_14, C_TEXT);
    lv_obj_add_event_cb(tb, touch_cb, LV_EVENT_CLICKED, NULL);
    lbl_touch = lv_obj_get_child(tb, 0);

    lv_obj_t * rb = mk_button(p, M + 2 * (gw + gx), 666 + 2 * (gh + gy), gw, gh,
                              "Refresh", &lv_font_montserrat_14, C_ACCENT);
    lv_obj_add_event_cb(rb, refresh_cb, LV_EVENT_CLICKED, NULL);

    /* system */
    lv_obj_t * sysc = mk_card(p, M, 938, 672, 150, "SYSTEM");
    lbl_sys1 = mk_label(sysc, "", &lv_font_montserrat_14, C_TEXT);
    lv_obj_set_pos(lbl_sys1, 0, 32);
    lbl_sys2 = mk_label(sysc, "", &lv_font_montserrat_14, C_TEXT);
    lv_obj_set_pos(lbl_sys2, 0, 58);
    lbl_sys3 = mk_label(sysc, "", &lv_font_montserrat_14, C_TEXT);
    lv_obj_set_pos(lbl_sys3, 0, 84);

    lbl_footer2 = mk_label(p, "page 2 / 3  |  swipe left for terminal",
                           &lv_font_montserrat_14, C_MUTED);
    lv_obj_set_pos(lbl_footer2, M, 1120);
    lv_obj_set_width(lbl_footer2, 672);
    lv_obj_set_style_text_align(lbl_footer2, LV_TEXT_ALIGN_CENTER, 0);
}

/* ---------------- page 3: terminal ---------------- */

/* The keyboard follows a phone layout: two function rows on top, a digit row,
 * three letter rows and a bottom row with space and enter. Every key carries a
 * width weight in tenths of a column and the rows are scaled to the same
 * width, so short rows get wider (easier to hit) keys. */

#define TCELL_W   10
#define TCELL_H   19
#define TCOLS     72
#define TROWS     40
#define TERM_TOP  56

#define KB_MARGIN 6      /* side margin of the key area */
#define KB_GAP    5      /* gap between keys and between rows */
#define KB_TOP    832    /* y of the first key row */
#define KB_PAD_TOP 8     /* padding above the first key row */
#define KB_W      708    /* 720 - 2 * KB_MARGIN */
#define KB_H_FUNC 48
#define KB_H_NUM  58
#define KB_H_KEY  64

enum {
    K_CHAR = 0, K_ESC, K_TAB, K_ENTER, K_BS, K_DEL, K_HOME, K_END, K_PGUP, K_PGDN,
    K_UP, K_DOWN, K_LEFT, K_RIGHT, K_CTRL, K_ALT, K_SHIFT, K_SYM
};

typedef struct {
    const char * a;       /* label on the normal layer */
    const char * sh;      /* label with Shift */
    const char * sym;     /* label on the symbol layer */
    const char * symsh;   /* label on the symbol layer with Shift */
    uint8_t kind;
    uint8_t w;            /* width weight, 10 = one column */
} keydef_t;

typedef struct {
    const keydef_t * keys;
    uint8_t n;
    uint8_t h;
} kbrow_t;

typedef struct {
    const keydef_t * def;
    lv_obj_t * btn;
    lv_obj_t * lbl;
} kbkey_t;

static const keydef_t ROW_FUNC1[] = {
    {"Esc",  NULL, NULL, NULL, K_ESC,  10},
    {"/",    NULL, NULL, NULL, K_CHAR, 10},
    {"-",    NULL, NULL, NULL, K_CHAR, 10},
    {"Home", NULL, NULL, NULL, K_HOME, 10},
    {"^",    NULL, NULL, NULL, K_UP,   10},
    {"End",  NULL, NULL, NULL, K_END,  10},
    {"PgUp", NULL, NULL, NULL, K_PGUP, 10}
};

static const keydef_t ROW_FUNC2[] = {
    {"Tab",  NULL, NULL, NULL, K_TAB,   10},
    {"Ctrl", NULL, NULL, NULL, K_CTRL,  10},
    {"Alt",  NULL, NULL, NULL, K_ALT,   10},
    {"<",    NULL, NULL, NULL, K_LEFT,  10},
    {"v",    NULL, NULL, NULL, K_DOWN,  10},
    {">",    NULL, NULL, NULL, K_RIGHT, 10},
    {"PgDn", NULL, NULL, NULL, K_PGDN,  10}
};

static const keydef_t ROW_NUM[] = {
    {"1", "!", "!", NULL, K_CHAR, 10},
    {"2", "@", "@", NULL, K_CHAR, 10},
    {"3", "#", "#", NULL, K_CHAR, 10},
    {"4", "$", "$", NULL, K_CHAR, 10},
    {"5", "%", "%", NULL, K_CHAR, 10},
    {"6", "^", "^", NULL, K_CHAR, 10},
    {"7", "&", "&", NULL, K_CHAR, 10},
    {"8", "*", "*", NULL, K_CHAR, 10},
    {"9", "(", "(", NULL, K_CHAR, 10},
    {"0", ")", ")", NULL, K_CHAR, 10}
};

static const keydef_t ROW_QW[] = {
    {"q", NULL, "`",  NULL, K_CHAR, 10},
    {"w", NULL, "~",  NULL, K_CHAR, 10},
    {"e", NULL, "\\", "|",  K_CHAR, 10},
    {"r", NULL, "{",  NULL, K_CHAR, 10},
    {"t", NULL, "}",  NULL, K_CHAR, 10},
    {"y", NULL, "[",  NULL, K_CHAR, 10},
    {"u", NULL, "]",  NULL, K_CHAR, 10},
    {"i", NULL, "<",  NULL, K_CHAR, 10},
    {"o", NULL, ">",  NULL, K_CHAR, 10},
    {"p", NULL, "=",  "+",  K_CHAR, 10}
};

static const keydef_t ROW_AS[] = {
    {"a", NULL, "-", NULL, K_CHAR, 10},
    {"s", NULL, "_", NULL, K_CHAR, 10},
    {"d", NULL, "+", NULL, K_CHAR, 10},
    {"f", NULL, ":", NULL, K_CHAR, 10},
    {"g", NULL, ";", NULL, K_CHAR, 10},
    {"h", NULL, "'", NULL, K_CHAR, 10},
    {"j", NULL, "\"", NULL, K_CHAR, 10},
    {"k", NULL, "|", NULL, K_CHAR, 10},
    {"l", NULL, "/", NULL, K_CHAR, 10}
};

static const keydef_t ROW_ZX[] = {
    {"Shift", NULL, NULL, NULL, K_SHIFT, 15},
    {"z", NULL, NULL, NULL, K_CHAR, 10},
    {"x", NULL, NULL, NULL, K_CHAR, 10},
    {"c", NULL, NULL, NULL, K_CHAR, 10},
    {"v", NULL, NULL, NULL, K_CHAR, 10},
    {"b", NULL, NULL, NULL, K_CHAR, 10},
    {"n", NULL, NULL, NULL, K_CHAR, 10},
    {"m", NULL, NULL, NULL, K_CHAR, 10},
    {"Bksp", NULL, NULL, NULL, K_BS, 15}
};

static const keydef_t ROW_BOT[] = {
    {"?123", NULL, NULL, NULL, K_SYM, 15},
    {",", NULL, NULL, NULL, K_CHAR, 10},
    {" ", NULL, NULL, NULL, K_CHAR, 45},
    {".", NULL, NULL, NULL, K_CHAR, 10},
    {"Enter", NULL, NULL, NULL, K_ENTER, 20}
};

static const kbrow_t KB_ROWS[] = {
    {ROW_FUNC1,  7, KB_H_FUNC},
    {ROW_FUNC2,  7, KB_H_FUNC},
    {ROW_NUM,   10, KB_H_NUM},
    {ROW_QW,    10, KB_H_KEY},
    {ROW_AS,     9, KB_H_KEY},
    {ROW_ZX,     9, KB_H_KEY},
    {ROW_BOT,    5, KB_H_KEY}
};

static kbkey_t keys_def[64];

static int nkeys = 0;
static int kb_ctrl = 0, kb_alt = 0, kb_shift = 0, kb_sym = 0;
static int term_dead_shown = 0;
static int cursor_on = 1;

static void kb_update(void);
static void kb_key_cb(lv_event_t * e);

static uint32_t dim_color(uint32_t c)
{
    uint32_t r = ((c >> 16) & 0xFF) * 3 / 5;
    uint32_t g = ((c >> 8) & 0xFF) * 3 / 5;
    uint32_t b = (c & 0xFF) * 3 / 5;
    return (r << 16) | (g << 8) | b;
}

static const char * key_text(const kbkey_t * k)
{
    const keydef_t * d = k->def;
    if(d->kind != K_CHAR) {
        if(d->kind == K_SYM) return kb_sym ? "ABC" : d->a;
        return d->a;
    }

    const char * s = (kb_sym && d->sym) ? d->sym : d->a;
    if(kb_shift) {
        if(kb_sym && d->symsh) return d->symsh;
        if(!kb_sym && d->sh) return d->sh;
        if(s[0] >= 'a' && s[0] <= 'z' && s[1] == '\0') {
            static char up[2];
            up[0] = (char)(s[0] - 'a' + 'A');
            up[1] = '\0';
            return up;
        }
    }
    return s;
}

static void kb_send(const char * s, size_t n)
{
    if(term) vt_write(term, s, n);
}

static void kb_key_cb(lv_event_t * e)
{
    kbkey_t * k = (kbkey_t *)lv_event_get_user_data(e);
    if(!k || !term) return;

    switch(k->def->kind) {
        case K_CTRL:  kb_ctrl = !kb_ctrl; kb_update(); return;
        case K_ALT:   kb_alt = !kb_alt; kb_update(); return;
        case K_SHIFT: kb_shift = !kb_shift; kb_update(); return;
        case K_SYM:   kb_sym = !kb_sym; kb_update(); return;
        case K_ESC:   kb_send("\x1b", 1); break;
        case K_TAB:   kb_send("\t", 1); break;
        case K_ENTER: kb_send("\r", 1); break;
        case K_BS:    kb_send("\x7f", 1); break;
        case K_DEL:   kb_send("\x1b[3~", 4); break;
        case K_HOME:  kb_send("\x1b[H", 3); break;
        case K_END:   kb_send("\x1b[F", 3); break;
        case K_PGUP:  kb_send("\x1b[5~", 4); break;
        case K_PGDN:  kb_send("\x1b[6~", 4); break;
        case K_UP:    kb_send("\x1b[A", 3); break;
        case K_DOWN:  kb_send("\x1b[B", 3); break;
        case K_RIGHT: kb_send("\x1b[C", 3); break;
        case K_LEFT:  kb_send("\x1b[D", 3); break;
        case K_CHAR: {
            const char * s = key_text(k);
            char out[2];
            size_t n = strlen(s);
            if(kb_ctrl && n > 0) {
                char c = s[0];
                if(c >= 'a' && c <= 'z') c = (char)(c - 'a' + 1);
                else if(c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 1);
                else if(c == ' ') c = 0;
                else if(c >= '@' && c <= '_') c = (char)(c - '@');
                else c = 0;
                out[0] = c;
                s = out;
                n = 1;
                kb_ctrl = 0;
            }
            if(kb_alt) {
                kb_send("\x1b", 1);
                kb_alt = 0;
            }
            kb_send(s, n);
            kb_shift = 0;
            break;
        }
        default: break;
    }

    kb_ctrl = 0;
    kb_alt = 0;
    kb_update();
}

static void kb_press_cb(lv_event_t * e)
{
    kbkey_t * k = (kbkey_t *)lv_event_get_user_data(e);
    if(!k || !k->lbl) return;
    lv_event_code_t c = lv_event_get_code(e);
    lv_obj_set_style_text_color(k->lbl,
                                lv_color_hex(c == LV_EVENT_PRESSED ? 0xFFFFFF : C_TEXT), 0);
}

static void kb_add(int x, int y, int w, int h, const keydef_t * d)
{
    if(nkeys >= (int)(sizeof(keys_def) / sizeof(keys_def[0]))) return;

    kbkey_t * k = &keys_def[nkeys++];
    k->def = d;

    lv_obj_t * b = lv_button_create(kb_panel);
    lv_obj_remove_style_all(b);
    lv_obj_set_pos(b, x, y);
    lv_obj_set_size(b, w, h);
    lv_obj_set_style_bg_color(b, lv_color_hex(C_BTN), 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(b, 10, 0);
    /* touch feedback: the key lights up while it is held down */
    lv_obj_set_style_bg_color(b, lv_color_hex(C_KEY_DOWN), LV_STATE_PRESSED);

    lv_obj_t * l = mk_label(b, d->a ? d->a : "", &lv_font_montserrat_20, C_TEXT);
    lv_obj_center(l);

    k->btn = b;
    k->lbl = l;

    lv_obj_add_event_cb(b, kb_key_cb, LV_EVENT_CLICKED, k);
    lv_obj_add_event_cb(b, kb_press_cb, LV_EVENT_PRESSED, k);
    lv_obj_add_event_cb(b, kb_press_cb, LV_EVENT_RELEASED, k);
    lv_obj_add_event_cb(b, kb_press_cb, LV_EVENT_PRESS_LOST, k);

    if(d->kind == K_BS || d->kind == K_LEFT || d->kind == K_RIGHT ||
       d->kind == K_UP || d->kind == K_DOWN)
        lv_obj_add_event_cb(b, kb_key_cb, LV_EVENT_LONG_PRESSED_REPEAT, k);
}

static void kb_update(void)
{
    for(int i = 0; i < nkeys; i++) {
        kbkey_t * k = &keys_def[i];
        const char * t = key_text(k);
        lv_label_set_text(k->lbl, t);
        lv_obj_set_style_text_font(k->lbl, strlen(t) > 1 ? &lv_font_montserrat_16
                                                         : &lv_font_montserrat_20, 0);

        int active = 0;
        switch(k->def->kind) {
            case K_CTRL:  active = kb_ctrl; break;
            case K_ALT:   active = kb_alt; break;
            case K_SHIFT: active = kb_shift; break;
            case K_SYM:   active = kb_sym; break;
            default: break;
        }
        lv_obj_set_style_bg_color(k->btn, lv_color_hex(active ? C_ACCENT : C_BTN), 0);
        lv_obj_set_style_text_color(k->lbl, lv_color_hex(active ? C_BG : C_TEXT), 0);
    }
}

static void build_keyboard(void)
{
    /* key coordinates are relative to the keyboard panel, which starts
     * KB_PAD_TOP pixels above the first key row */
    int y = KB_PAD_TOP;

    for(size_t r = 0; r < sizeof(KB_ROWS) / sizeof(KB_ROWS[0]); r++) {
        const kbrow_t * row = &KB_ROWS[r];
        int total = 0;
        int acc = 0;
        int x = KB_MARGIN;

        for(int i = 0; i < row->n; i++) total += row->keys[i].w;

        for(int i = 0; i < row->n; i++) {
            acc += row->keys[i].w;
            int nx = KB_MARGIN + acc * KB_W / total;
            int w = nx - x;
            if(i < row->n - 1) w -= KB_GAP;
            kb_add(x, y, w, row->h, &row->keys[i]);
            x = nx;
        }

        y += row->h + KB_GAP;
    }

    kb_update();
}

extern const lv_font_t term_font;

static inline int put_utf8(char * b, uint32_t cp)
{
    if(cp < 0x80) { b[0] = (char)cp; return 1; }
    if(cp < 0x800) { b[0] = (char)(0xC0 | (cp >> 6)); b[1] = (char)(0x80 | (cp & 0x3F)); return 2; }
    if(cp < 0x10000) { b[0] = (char)(0xE0 | (cp >> 12)); b[1] = (char)(0x80 | ((cp >> 6) & 0x3F)); b[2] = (char)(0x80 | (cp & 0x3F)); return 3; }
    b[0] = (char)(0xF0 | (cp >> 18)); b[1] = (char)(0x80 | ((cp >> 12) & 0x3F)); b[2] = (char)(0x80 | ((cp >> 6) & 0x3F)); b[3] = (char)(0x80 | (cp & 0x3F)); return 4;
}

static void term_draw_cb(lv_event_t * e)
{
    lv_obj_t * obj = (lv_obj_t *)lv_event_get_target(e);
    lv_layer_t * layer = lv_event_get_layer(e);
    if(!term) return;

    lv_area_t oa;
    lv_obj_get_coords(obj, &oa);

    uint32_t def_bg = vt_default_bg(term);

    lv_draw_fill_dsc_t fill;
    lv_draw_fill_dsc_init(&fill);
    fill.opa = LV_OPA_COVER;
    fill.radius = 0;

    lv_draw_label_dsc_t ld;
    lv_draw_label_dsc_init(&ld);
    ld.font = &term_font;
    ld.opa = LV_OPA_COVER;
    ld.text_local = 1;
    ld.flag = LV_TEXT_FLAG_NONE;
    ld.align = LV_TEXT_ALIGN_LEFT;

    char buf[TCOLS * 4 + 1];

    for(int r = 0; r < TROWS; r++) {
        const vt_cell_t * cells = vt_view_row(term, r);
        if(!cells) break;
        int c = 0;

        while(c < TCOLS) {
            uint32_t fg = cells[c].fg;
            uint32_t bgc = cells[c].bg;
            uint8_t at = cells[c].attr;
            int start = c;
            int n = 0;

            while(c < TCOLS && cells[c].fg == fg && cells[c].bg == bgc && cells[c].attr == at) {
                uint32_t ch = cells[c].ch;
                if(ch < 0x20) ch = ' ';
                n += put_utf8(buf + n, ch);
                c++;
            }
            buf[n] = '\0';

            uint32_t rfg = fg, rbg = bgc;
            if(at & VT_ATTR_REVERSE) {
                rfg = bgc;
                rbg = fg;
            }
            if(at & VT_ATTR_HIDDEN) rfg = rbg;
            if(at & VT_ATTR_DIM) rfg = dim_color(rfg);

            if(rbg != def_bg) {
                fill.color = lv_color_hex(rbg);
                lv_area_t ra;
                ra.x1 = oa.x1 + start * TCELL_W;
                ra.y1 = oa.y1 + r * TCELL_H;
                ra.x2 = oa.x1 + c * TCELL_W - 1;
                ra.y2 = oa.y1 + (r + 1) * TCELL_H - 1;
                lv_draw_fill(layer, &fill, &ra);
            }

            ld.color = lv_color_hex(rfg);
            ld.decor = (at & VT_ATTR_UNDER) ? LV_TEXT_DECOR_UNDERLINE : LV_TEXT_DECOR_NONE;
            ld.text = buf;
            ld.text_length = (uint32_t)n;

            lv_area_t ta;
            ta.x1 = oa.x1 + start * TCELL_W;
            ta.y1 = oa.y1 + r * TCELL_H;
            ta.x2 = oa.x1 + TCOLS * TCELL_W - 1;
            ta.y2 = oa.y1 + (r + 1) * TCELL_H - 1;
            lv_draw_label(layer, &ld, &ta);

            if(at & VT_ATTR_BOLD) {
                ta.x1++;
                lv_draw_label(layer, &ld, &ta);
            }
        }
    }

    /* Cursor: block, drawn last so it stays on top. */
    if(cursor_on && vt_view(term) == 0 && vt_cursor_visible(term)) {
        int cx = vt_cursor_x(term);
        int cy = vt_cursor_y(term);
        const vt_cell_t * row = vt_view_row(term, cy);
        uint32_t ch = (row && cx < TCOLS) ? row[cx].ch : ' ';
        if(ch < 0x20) ch = ' ';
        int n = put_utf8(buf, ch);
        buf[n] = '\0';

        lv_area_t ca;
        ca.x1 = oa.x1 + cx * TCELL_W;
        ca.y1 = oa.y1 + cy * TCELL_H;
        ca.x2 = ca.x1 + TCELL_W - 1;
        ca.y2 = ca.y1 + TCELL_H - 1;

        fill.color = lv_color_hex(0xC9D6E3);
        lv_draw_fill(layer, &fill, &ca);

        ld.color = lv_color_hex(def_bg);
        ld.decor = LV_TEXT_DECOR_NONE;
        ld.text = buf;
        ld.text_length = (uint32_t)n;
        lv_draw_label(layer, &ld, &ca);
    }
}

static int term_drag = 0;
static int term_drag_y = 0;

static void term_press_cb(lv_event_t * e)
{
    (void)e;
    term_drag = 0;
    lv_indev_t * ind = lv_indev_active();
    if(ind) {
        lv_point_t p;
        lv_indev_get_point(ind, &p);
        term_drag_y = p.y;
    }
}

static void term_pressing_cb(lv_event_t * e)
{
    (void)e;
    if(!term) return;
    lv_indev_t * ind = lv_indev_active();
    if(!ind) return;

    lv_point_t p;
    lv_indev_get_point(ind, &p);
    if(!term_drag) term_drag = 1;

    int dy = p.y - term_drag_y;
    int lines = dy / TCELL_H;
    if(lines != 0) {
        lv_obj_clear_flag(tv, LV_OBJ_FLAG_SCROLLABLE);
        vt_scroll(term, lines);
        term_drag_y += lines * TCELL_H;
    }
}

static void term_release_cb(lv_event_t * e)
{
    (void)e;
    term_drag = 0;
    lv_obj_add_flag(tv, LV_OBJ_FLAG_SCROLLABLE);
}

static void term_status_update(void)
{
    if(!term || !term_status) return;

    if(vt_is_dead(term)) {
        term_dead_shown = 1;
        lv_label_set_text(term_status, "shell exited - tap here to restart");
        lv_obj_set_style_text_color(term_status, lv_color_hex(C_RED), 0);
    }
    else if(vt_view(term) > 0) {
        term_dead_shown = 0;
        label_fmt(term_status, "history -%d lines - tap for live", vt_view(term));
        lv_obj_set_style_text_color(term_status, lv_color_hex(C_AMBER), 0);
    }
    else {
        term_dead_shown = 0;
        lv_label_set_text(term_status, "72x40  |  bash");
        lv_obj_set_style_text_color(term_status, lv_color_hex(C_MUTED), 0);
    }
}

static void term_restart(void)
{
    if(!term) return;
    vt_restart(term);
    term_drag = 0;
    vt_scroll_live(term);
    term_status_update();
    if(term_view) lv_obj_invalidate(term_view);
}

static void term_status_cb(lv_event_t * e)
{
    (void)e;
    if(!term) return;
    if(vt_is_dead(term)) {
        term_restart();
        return;
    }
    vt_scroll_live(term);
    term_status_update();
    if(term_view) lv_obj_invalidate(term_view);
}

static void term_tick(lv_timer_t * t)
{
    (void)t;
    if(!term) return;

    static int last_view = 0;
    vt_pump(term);

    if(vt_is_dead(term) && !term_dead_shown) term_status_update();

    int view = vt_view(term);
    if(view != last_view) {
        last_view = view;
        term_status_update();
    }

    if(vt_consume_dirty(term) && term_view) lv_obj_invalidate(term_view);
}

static void term_blink_cb(lv_timer_t * t)
{
    (void)t;
    if(!term || !term_view) return;
    if(!vt_cursor_visible(term) || vt_view(term) != 0) {
        cursor_on = 1;
        return;
    }
    cursor_on = !cursor_on;

    lv_area_t oa;
    lv_obj_get_coords(term_view, &oa);
    lv_area_t ca;
    ca.x1 = oa.x1 + vt_cursor_x(term) * TCELL_W;
    ca.y1 = oa.y1 + vt_cursor_y(term) * TCELL_H;
    ca.x2 = ca.x1 + TCELL_W - 1;
    ca.y2 = ca.y1 + TCELL_H - 1;
    lv_obj_invalidate_area(term_view, &ca);
}

static void build_page3(void)
{
    lv_obj_t * p = page3;

    lv_obj_t * title = mk_label(p, "Terminal", &lv_font_montserrat_24, C_ACCENT);
    lv_obj_set_pos(title, M, 22);

    term_status = mk_label(p, "", &lv_font_montserrat_14, C_MUTED);
    lv_obj_set_pos(term_status, M + 132, 30);
    lv_obj_add_flag(term_status, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(term_status, term_status_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t * hint = mk_label(p, "swipe right to go back", &lv_font_montserrat_14, C_MUTED);
    lv_obj_align(hint, LV_ALIGN_TOP_RIGHT, -M, 28);

    term_view = lv_obj_create(p);
    lv_obj_remove_style_all(term_view);
    lv_obj_set_pos(term_view, 0, TERM_TOP);
    lv_obj_set_size(term_view, TCOLS * TCELL_W, TROWS * TCELL_H);
    lv_obj_set_style_bg_color(term_view, lv_color_hex(C_BG), 0);
    lv_obj_set_style_bg_opa(term_view, LV_OPA_COVER, 0);
    lv_obj_clear_flag(term_view, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(term_view, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(term_view, term_draw_cb, LV_EVENT_DRAW_MAIN, NULL);
    lv_obj_add_event_cb(term_view, term_press_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(term_view, term_pressing_cb, LV_EVENT_PRESSING, NULL);
    lv_obj_add_event_cb(term_view, term_release_cb, LV_EVENT_RELEASED, NULL);
    lv_obj_add_event_cb(term_view, term_release_cb, LV_EVENT_PRESS_LOST, NULL);

    kb_panel = lv_obj_create(p);
    lv_obj_remove_style_all(kb_panel);
    lv_obj_set_pos(kb_panel, 0, KB_TOP - KB_PAD_TOP);
    lv_obj_set_size(kb_panel, 720, 1280 - (KB_TOP - KB_PAD_TOP));
    lv_obj_set_style_bg_color(kb_panel, lv_color_hex(0x0F1620), 0);
    lv_obj_set_style_bg_opa(kb_panel, LV_OPA_COVER, 0);
    lv_obj_clear_flag(kb_panel, LV_OBJ_FLAG_SCROLLABLE);

    build_keyboard();
}

static void build_chrome(void)
{
    lv_obj_set_style_bg_color(lv_screen_active(), lv_color_hex(C_BG), 0);
    lv_obj_set_style_bg_opa(lv_screen_active(), LV_OPA_COVER, 0);

    tv = lv_tileview_create(lv_screen_active());
    lv_obj_set_size(tv, 720, 1280);
    lv_obj_set_pos(tv, 0, 0);
    lv_obj_set_scrollbar_mode(tv, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_bg_opa(tv, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(tv, 0, 0);
    lv_obj_set_style_pad_all(tv, 0, 0);

    page1 = lv_tileview_add_tile(tv, 0, 0, LV_DIR_RIGHT);
    page2 = lv_tileview_add_tile(tv, 1, 0, LV_DIR_LEFT | LV_DIR_RIGHT);
    page3 = lv_tileview_add_tile(tv, 2, 0, LV_DIR_LEFT);

    lv_obj_t * pages[3] = {page1, page2, page3};
    for(int i = 0; i < 3; i++) {
        lv_obj_set_style_bg_opa(pages[i], LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(pages[i], 0, 0);
        lv_obj_set_style_pad_all(pages[i], 0, 0);
        lv_obj_clear_flag(pages[i], LV_OBJ_FLAG_SCROLLABLE);
    }

    /* page dots */
    dot1 = lv_obj_create(lv_screen_active());
    lv_obj_remove_style_all(dot1);
    lv_obj_set_size(dot1, 10, 10);
    lv_obj_set_pos(dot1, 341, 1240);
    lv_obj_set_style_radius(dot1, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(dot1, lv_color_hex(C_ACCENT), 0);
    lv_obj_set_style_bg_opa(dot1, LV_OPA_COVER, 0);

    dot2 = lv_obj_create(lv_screen_active());
    lv_obj_remove_style_all(dot2);
    lv_obj_set_size(dot2, 10, 10);
    lv_obj_set_pos(dot2, 355, 1240);
    lv_obj_set_style_radius(dot2, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(dot2, lv_color_hex(0x334155), 0);
    lv_obj_set_style_bg_opa(dot2, LV_OPA_COVER, 0);

    dot3 = lv_obj_create(lv_screen_active());
    lv_obj_remove_style_all(dot3);
    lv_obj_set_size(dot3, 10, 10);
    lv_obj_set_pos(dot3, 369, 1240);
    lv_obj_set_style_radius(dot3, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(dot3, lv_color_hex(0x334155), 0);
    lv_obj_set_style_bg_opa(dot3, LV_OPA_COVER, 0);

    lv_obj_add_event_cb(dot1, dot_cb, LV_EVENT_CLICKED, (void *)(intptr_t)0);
    lv_obj_add_event_cb(dot2, dot_cb, LV_EVENT_CLICKED, (void *)(intptr_t)1);
    lv_obj_add_event_cb(dot3, dot_cb, LV_EVENT_CLICKED, (void *)(intptr_t)2);
    lv_obj_add_event_cb(tv, tv_changed_cb, LV_EVENT_VALUE_CHANGED, NULL);

    /* alert banner (hidden by default, on top of everything) */
    banner = lv_obj_create(lv_screen_active());
    lv_obj_remove_style_all(banner);
    lv_obj_set_size(banner, 720, 52);
    lv_obj_set_pos(banner, 0, 0);
    lv_obj_set_style_bg_color(banner, lv_color_hex(0xB91C1C), 0);
    lv_obj_set_style_bg_opa(banner, LV_OPA_COVER, 0);
    lbl_alert = mk_label(banner, "ALERT", &lv_font_montserrat_20, 0xFFFFFF);
    lv_obj_center(lbl_alert);
    lv_obj_add_flag(banner, LV_OBJ_FLAG_HIDDEN);

    /* power confirmation modal (hidden until a power button is tapped) */
    power_modal = lv_obj_create(lv_screen_active());
    lv_obj_remove_style_all(power_modal);
    lv_obj_set_size(power_modal, 720, 1280);
    lv_obj_set_pos(power_modal, 0, 0);
    lv_obj_set_style_bg_color(power_modal, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(power_modal, LV_OPA_70, 0);
    lv_obj_add_flag(power_modal, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(power_modal, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t * pc = mk_card(power_modal, 60, 460, 600, 360, NULL);
    lv_obj_set_style_radius(pc, 24, 0);

    power_title = mk_label(pc, "Reboot server?", &lv_font_montserrat_28, C_TEXT);
    lv_obj_align(power_title, LV_ALIGN_TOP_MID, 0, 18);

    power_text = mk_label(pc, "", &lv_font_montserrat_14, C_MUTED);
    lv_obj_set_width(power_text, 500);
    lv_obj_set_style_text_align(power_text, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(power_text, LV_ALIGN_TOP_MID, 0, 68);

    lv_obj_t * hint = mk_label(pc, "Auto-cancels in 5 seconds.", &lv_font_montserrat_14, C_MUTED);
    lv_obj_align(hint, LV_ALIGN_TOP_MID, 0, 98);

    lv_obj_t * cancel = mk_button(pc, 26, 210, 250, 80, "Cancel",
                                  &lv_font_montserrat_20, C_TEXT);
    lv_obj_add_event_cb(cancel, power_cancel_cb, LV_EVENT_CLICKED, NULL);

    power_confirm = mk_button(pc, 296, 210, 250, 80, "Reboot",
                              &lv_font_montserrat_20, 0xFFFFFF);
    lv_obj_set_style_bg_color(power_confirm, lv_color_hex(C_DARKRED), 0);
    lv_obj_add_event_cb(power_confirm, power_confirm_cb, LV_EVENT_CLICKED, NULL);
    power_confirm_lbl = lv_obj_get_child(power_confirm, 0);

    /* sleep overlay (full-screen black shield for screen off + double-tap wake) */
    sleep_overlay = lv_obj_create(lv_layer_sys());
    lv_obj_remove_style_all(sleep_overlay);
    lv_obj_set_size(sleep_overlay, 720, 1280);
    lv_obj_set_pos(sleep_overlay, 0, 0);
    lv_obj_set_style_bg_color(sleep_overlay, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(sleep_overlay, LV_OPA_COVER, 0);
    lv_obj_clear_flag(sleep_overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(sleep_overlay, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(sleep_overlay, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(sleep_overlay, sleep_wake_cb, LV_EVENT_CLICKED, NULL);
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
    signal(SIGCHLD, SIG_IGN);

    lv_init();
    lv_tick_set_cb(tick_cb);

    lv_display_t * disp = lv_linux_fbdev_create();
    if(!disp) {
        fprintf(stderr, "failed to create fbdev display\n");
        return 1;
    }
    if(lv_linux_fbdev_set_file(disp, "/dev/fb0") != LV_RESULT_OK) {
        fprintf(stderr, "failed to open /dev/fb0\n");
        return 1;
    }

    lv_theme_t * th = lv_theme_default_init(disp,
                                            lv_color_hex(C_ACCENT),
                                            lv_color_hex(C_GREEN),
                                            true,
                                            &lv_font_montserrat_14);
    lv_display_set_theme(disp, th);

    find_touch();
    lv_indev_t * indev = lv_evdev_create(LV_INDEV_TYPE_POINTER, touch_path);
    if(!indev) fprintf(stderr, "warning: failed to open touch device %s\n", touch_path);

    build_chrome();
    build_page1();
    build_page2();
    build_page3();

    term = vt_create(TCOLS, TROWS, 400);
    if(!term) fprintf(stderr, "warning: failed to start terminal\n");
    term_status_update();

    const char * start_page = getenv("LVGL_PAGE");
    int sp = start_page ? atoi(start_page) : 1;
    if(sp < 1 || sp > 3) sp = 1;
    if(sp > 1) {
        lv_tileview_set_tile_by_index(tv, (uint32_t)(sp - 1), 0, LV_ANIM_OFF);
        tv_changed_cb(NULL);
    }

    for(int i = 0; i < NSVC; i++) svc_up[i] = -1;

    check_wifi();
    check_cpu_zram();
    check_journal();
    check_kuma_db();
    check_updates();
    count_failed();
    read_net_counters();

    svc_tick(NULL);
    ui_tick(NULL);
    health_tick(NULL);
    net_tick(NULL);

    lv_timer_create(svc_tick, 2500, NULL);
    lv_timer_create(ui_tick, 1000, NULL);
    lv_timer_create(health_tick, 5000, NULL);
    lv_timer_create(net_tick, 5000, NULL);
    lv_timer_create(slow_tick, 300000, NULL);
    lv_timer_create(term_tick, 33, NULL);
    lv_timer_create(term_blink_cb, 500, NULL);

    printf("GUI running: fb0 720x1280, touch %s\n", touch_path);

    while(1) {
        uint32_t idle = lv_timer_handler();
        if(idle < 10) idle = 10;
        usleep(idle * 1000);
    }
    return 0;
}
