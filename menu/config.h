#ifndef CONFIG_H
#define CONFIG_H

/* ── Device paths ──────────────────────────────────────────────────────────── */
#define BTN_DEV         "/dev/input/event0"
#define TCH_KEY         "/dev/input/event1"
#define TCH_SCN         "/dev/input/event2"
#define FB_BLANK        "/sys/class/graphics/fb0/blank"
#define BRIGHTNESS_FILE "/sys/class/backlight/spi3.0/brightness"
#define BATTERY_FILE    "/sys/class/power_supply/battery/capacity"

/* ── Key codes ─────────────────────────────────────────────────────────────── */
#define KEY_VOLUMEUP_CODE   115
#define KEY_VOLUMEDOWN_CODE 114
#define KEY_POWER_CODE      116
#define KEY_HOME_CODE       352
#define KEY_MENU_CODE       139
#define KEY_BACK_CODE       158

/* ── Brightness ────────────────────────────────────────────────────────────── */
#define BRIGHTNESS_MIN  0
#define BRIGHTNESS_MAX  24
#define BRIGHTNESS_DEF  8

/* ── Power-button hold thresholds (seconds) ────────────────────────────────── */
#define HOLD_REBOOT_S   2
#define HOLD_POWEROFF_S 5

#endif /* CONFIG_H */
