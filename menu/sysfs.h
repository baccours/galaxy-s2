#ifndef SYSFS_H
#define SYSFS_H

#include <stdbool.h>
#include <stddef.h>

/* ── sysfs I/O ─────────────────────────────────────────────────────────────── */
int write_sysfs(const char *path, const char *value);
int read_sysfs(const char *path, char *buf, size_t size);

/* ── Process execution ─────────────────────────────────────────────────────── */
int run_cmd(char *const argv[]);

/* ── Hardware controls ─────────────────────────────────────────────────────── */
extern uint8_t current_brightness;
extern bool    screen_blanked;

void set_brightness(int level);
void set_blank(int fb_fd, bool blank);

#endif /* SYSFS_H */
