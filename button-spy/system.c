/*
 * system.c — low-level system helpers
 *
 * Owns: terminal, fb_blank, touch_inhibit, run_cmd, fbkeyboard,
 *       brightness, logging.
 * Nothing here knows about menus or the FSM.
 */

#include "button-spy.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

/* ── Global state definitions ────────────────────────────────────────────── */
volatile sig_atomic_t g_running = 1;
AppState         g_state        = STATE_IDLE;
bool             g_screen_blank = false;
bool             g_fbkbd_on     = false;
const Menu      *g_menu         = NULL;
uint8_t          g_sel          = 0;
int              g_brightness   = BRIGHTNESS_DEFAULT;
char             g_batt_lines[BATT_LINES_MAX][BATT_LINE_LEN + 1];
int              g_batt_nlines = 0;

int   g_fd_gpio          = -1;
int   g_fd_touchkey      = -1;
int   g_fd_fb            = -1;
int   g_fd_touchinhibit  = -1;
FILE *g_tty              = NULL;

/* ── Logging ────────────────────────────────────────────────────────────── */
void log_info(const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt);
    fputs("[menu] ", stderr);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    fflush(stderr);
    va_end(ap);
}

void log_err(const char *fmt, ...)
{
    int saved = errno;   /* capture before any stdio call can clobber it */
    va_list ap; va_start(ap, fmt);
    fprintf(stderr, "[menu] ERROR: ");
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, ": %s\n", strerror(saved));
    fflush(stderr);
    va_end(ap);
}

/* ── Terminal ─────────────────────────────────────────────────────────────
 * DEV_TTY is opened once and kept for the process lifetime.
 * terminal_raw/restore only toggle termios flags — no reopen.
 */
static struct termios g_orig_termios;
static bool           g_termios_saved = false;
static int            g_tty_fd        = -1;

bool terminal_open(void)
{
    g_tty_fd = open(DEV_TTY, O_RDWR | O_CLOEXEC);
    if (g_tty_fd < 0) { log_err("open " DEV_TTY); return false; }

    if (tcgetattr(g_tty_fd, &g_orig_termios) != 0) {
        log_err("tcgetattr " DEV_TTY);
        close(g_tty_fd); g_tty_fd = -1;
        return false;
    }

    if (write(g_tty_fd, T_DISABLE_BLANK, sizeof(T_DISABLE_BLANK) - 1) < 0) {
        log_err("failed to set blanking interval");
    }

    g_tty = fdopen(g_tty_fd, "w");
    if (!g_tty) {
        log_err("fdopen " DEV_TTY);
        close(g_tty_fd); g_tty_fd = -1;
        return false;
    }

    return true;
}

void terminal_raw(void)
{
    if (g_termios_saved || g_tty_fd < 0) return;
    struct termios t = g_orig_termios;
    t.c_lflag &= (tcflag_t)~(ICANON | ECHO | ISIG);
    t.c_cc[VMIN]  = 1;
    t.c_cc[VTIME] = 0;
    if (tcsetattr(g_tty_fd, TCSAFLUSH, &t) == 0)
        g_termios_saved = true;
    else
        log_err("tcsetattr raw");
}

bool terminal_restore(void)
{
    if (!g_termios_saved || g_tty_fd < 0) return false;
    if (tcsetattr(g_tty_fd, TCSAFLUSH, &g_orig_termios) == 0)
        g_termios_saved = false;
    else
        log_err("tcsetattr restore");
    return !g_termios_saved;
}

void terminal_close(void)
{
    if (g_tty_fd >= 0) {
        write(g_tty_fd, T_RESTORE_BLANK, sizeof(T_RESTORE_BLANK) - 1);
    }
    terminal_restore();
    if (g_tty) { fclose(g_tty); g_tty = NULL; g_tty_fd = -1; }
}

/* ── sysfs write helpers ──────────────────────────────────────────────────
 * We keep fds open for the process lifetime and lseek back to 0
 */
void fb_set_blank(bool blank)
{
    const char c = blank ? '1' : '0';
    if (write(g_fd_fb, &c, 1) < 0) log_err("fb_blank write");
    else g_screen_blank = blank;
    lseek(g_fd_fb, 0, SEEK_SET);
}

void touch_inhibit(bool inhibit)
{
    const char c = inhibit ? '1' : '0';
    if (write(g_fd_touchinhibit, &c, 1) < 0) log_err("touch inhibit write");
    lseek(g_fd_touchinhibit, 0, SEEK_SET);
}

/* ── Process execution — fork+execvp, no shell ───────────────────────────── */
int run_cmd(char *const argv[])
{
    pid_t pid = fork();
    if (pid < 0) { log_err("fork"); return -1; }
    if (pid == 0) { execvp(argv[0], argv); _exit(127); }
    int st;
    if (waitpid(pid, &st, 0) < 0) { log_err("waitpid"); return -1; }
    return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}

/* ── fbkeyboard OpenRC service ────────────────────────────────────────────── */
void fbkbd_set(bool start)
{
    const char *state = start ? "start" : "stop";
    char *const args[] = { "rc-service", "fbkeyboard", (char *)state, NULL };
    if (run_cmd(args) == 0)
        g_fbkbd_on = start;
}

/* ── Button device helpers ────────────────────────────────────────────────── */

int open_button_dev(const char *path)
{
    int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) { log_err("open %s", path); return -1; }
    if (ioctl(fd, EVIOCGRAB, (void *)1) < 0) {
        log_err("EVIOCGRAB %s", path);
        close(fd);
        return -1;
    }
    return fd;
}

void release_button_dev(int fd)
{
    if (fd < 0) return;
    if (ioctl(fd, EVIOCGRAB, (void *)0) < 0)
        log_err("EVIOCGRAB release");
    close(fd);
}

/* ── Brightness ───────────────────────────────────────────────────────────── */
int brightness_read(void)
{
    FILE *f = fopen(BRIGHTNESS_PATH, "r");
    if (!f) { log_err("open " BRIGHTNESS_PATH); return g_brightness; }
    int v = g_brightness;
    if (fscanf(f, "%d", &v) != 1) log_err("read " BRIGHTNESS_PATH);
    fclose(f);
    return v;
}

void brightness_write(int level)
{
    if (level < BRIGHTNESS_MIN) level = BRIGHTNESS_MIN;
    if (level > BRIGHTNESS_MAX) level = BRIGHTNESS_MAX;
    g_brightness = level;
    FILE *f = fopen(BRIGHTNESS_PATH, "w");
    if (!f) { log_err("open " BRIGHTNESS_PATH); return; }
    fprintf(f, "%d\n", level);
    fclose(f);
}

/* ── Battery ─────────────────────────────────────────────────────────────── */
void battery_read(void)
{
    g_batt_nlines = 0;

    FILE *fp = popen(BATTERY_STATUS_SCRIPT, "r");
    if (!fp) { log_err("popen " BATTERY_STATUS_SCRIPT); return; }
    
    char raw[256];
    while (g_batt_nlines < BATT_LINES_MAX && fgets(raw, sizeof(raw), fp)) {
        size_t len = strlen(raw);
        if (len > 0 && raw[len - 1] == '\n') raw[--len] = '\0';
        if (len > (size_t)BATT_LINE_LEN) len = (size_t)BATT_LINE_LEN;
        memcpy(g_batt_lines[g_batt_nlines], raw, len);
        g_batt_lines[g_batt_nlines][len] = '\0';
        g_batt_nlines++;
    }

    pclose(fp);
}