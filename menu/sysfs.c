#include "sysfs.h"
#include "config.h"

#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

/* ── Hardware state ────────────────────────────────────────────────────────── */
uint8_t current_brightness = BRIGHTNESS_DEF;
bool    screen_blanked     = false;

/* ── sysfs I/O ─────────────────────────────────────────────────────────────── */
int write_sysfs(const char *path, const char *value)
{
    int fd = open(path, O_WRONLY);
    if (fd < 0) return -1;
    ssize_t len = (ssize_t)strlen(value);
    ssize_t ret = write(fd, value, (size_t)len);
    close(fd);
    return (ret == len) ? 0 : -1;
}

int read_sysfs(const char *path, char *buf, size_t size)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;
    ssize_t n = read(fd, buf, size - 1);
    close(fd);
    if (n < 0) return -1;
    buf[n] = '\0';
    while (n > 0 && (buf[n-1] == '\n' || buf[n-1] == '\r' || buf[n-1] == ' '))
        buf[--n] = '\0';
    return (int)n;
}

/* ── Process execution ─────────────────────────────────────────────────────── */
int run_cmd(char *const argv[])
{
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) { execvp(argv[0], argv); _exit(127); }
    int status;
    if (waitpid(pid, &status, 0) < 0) return -1;
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

/* ── Hardware controls ─────────────────────────────────────────────────────── */
void set_brightness(int level)
{
    if (level < BRIGHTNESS_MIN) level = BRIGHTNESS_MIN;
    if (level > BRIGHTNESS_MAX) level = BRIGHTNESS_MAX;
    current_brightness = (uint8_t)level;
    char buf[8];
    snprintf(buf, sizeof(buf), "%d", level);
    if (write_sysfs(BRIGHTNESS_FILE, buf) < 0) perror("set_brightness");
}

void set_blank(int fb_fd, bool blank)
{
    const char val = blank ? '1' : '0';
    if (write(fb_fd, &val, 1) < 0) perror("set_blank");
    else screen_blanked = blank;
}
