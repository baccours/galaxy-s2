/*
 * system.c — low-level system helpers
 *
 * Owns: terminal raw/restore, fb_blank sysfs fd, run_cmd (fork+execvp),
 *       EVIOCGRAB wrappers, fbkeyboard OpenRC service, logging.
 *
 * Nothing here knows about menus or the FSM.
 * Binary: menu
 */

#include "main.h"

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

/* ── Global state definitions (declared extern in main.h) ───────────────── */
volatile sig_atomic_t g_running      = 1;
AppState              g_state        = STATE_IDLE;
bool                  g_screen_blank = false;
bool                  g_fbkbd_on     = false;
const Menu           *g_menu         = NULL;
uint8_t               g_sel          = 0;

int  g_brightness = BRIGHTNESS_DEFAULT;

int g_fd_gpio     = -1;
int g_fd_touchkey = -1;
int g_fd_touch    = -1;
int g_fd_fb       = -1;

/* ── Terminal ─────────────────────────────────────────────────────────────── */
/*
 * We open /dev/tty once at startup and keep it open for the process
 * lifetime. raw/restore just toggle termios flags on the same fd.
 * This avoids any race where close+reopen loses the saved state, and
 * works correctly when stdout/stdin are redirected.
 */
static struct termios g_orig_termios;
static bool           g_termios_saved = false;
static int            g_tty_fd        = -1;

bool terminal_open(void)
{
    g_tty_fd = open("/dev/tty", O_RDWR | O_CLOEXEC);
    if (g_tty_fd < 0) { log_err("open /dev/tty"); return false; }
    if (tcgetattr(g_tty_fd, &g_orig_termios) != 0) {
        log_err("tcgetattr"); close(g_tty_fd); g_tty_fd = -1; return false;
    }
    log_info("terminal_open ok, tty_fd=%d lflag=0x%x",
             g_tty_fd, (unsigned)g_orig_termios.c_lflag);
    return true;
}

void terminal_raw(void)
{
    log_info("terminal_raw called: saved=%d tty_fd=%d", g_termios_saved, g_tty_fd);
    if (g_termios_saved || g_tty_fd < 0) return;
    struct termios t = g_orig_termios;
    t.c_lflag &= (tcflag_t)~(ICANON | ECHO | ISIG);
    t.c_cc[VMIN]  = 1;
    t.c_cc[VTIME] = 0;
    if (tcsetattr(g_tty_fd, TCSAFLUSH, &t) == 0) {
        g_termios_saved = true;
        log_info("terminal_raw ok");
    } else {
        log_err("tcsetattr raw");
    }
}

bool terminal_restore(void)
{
    log_info("terminal_restore called: saved=%d tty_fd=%d", g_termios_saved, g_tty_fd);
    if (!g_termios_saved || g_tty_fd < 0) return false;
    if (tcsetattr(g_tty_fd, TCSAFLUSH, &g_orig_termios) == 0) {
        g_termios_saved = false;
        log_info("terminal_restore ok");
    } else {
        log_err("tcsetattr restore");
    }
    return !g_termios_saved;
}

void terminal_close(void)
{
    terminal_restore();
    if (g_tty_fd >= 0) { close(g_tty_fd); g_tty_fd = -1; }
}

/* ── Logging ──────────────────────────────────────────────────────────────── */
void log_info(const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt);
    fputs("[menu] ", stderr);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
}

void log_err(const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt);
    fprintf(stderr, "[menu] ERROR: ");
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, ": %s\n", strerror(errno));
    va_end(ap);
}

/* ── Framebuffer blank ────────────────────────────────────────────────────── */

/*
 * Write blank/unblank to the already-open fb sysfs fd.
 * lseek back to 0 so the next write lands at offset 0 again.
 */
void fb_set_blank(bool blank)
{
    const char c = blank ? '1' : '0';
    if (write(g_fd_fb, &c, 1) < 0) log_err("fb_blank write");
    else g_screen_blank = blank;
    lseek(g_fd_fb, 0, SEEK_SET);
    log_info("screen %s", blank ? "blanked" : "unblanked");
}

/* ── Process execution ────────────────────────────────────────────────────── */

/* fork + execvp — no shell, no injection surface */
int run_cmd(char *const argv[])
{
    log_info("exec: %s", argv[0]);
    pid_t pid = fork();
    if (pid < 0) { log_err("fork"); return -1; }
    if (pid == 0) { execvp(argv[0], argv); _exit(127); }
    int st;
    if (waitpid(pid, &st, 0) < 0) { log_err("waitpid"); return -1; }
    int rc = WIFEXITED(st) ? WEXITSTATUS(st) : -1;
    if (rc != 0) log_info("%s returned %d", argv[0], rc);
    return rc;
}

/* ── Input device grab helpers ────────────────────────────────────────────── */

/* EVIOCGRAB returns EINVAL if you grab an already-grabbed fd, or release
 * one that is not grabbed. Track state explicitly and make it idempotent. */
static bool g_touch_grabbed = false;

void grab(int fd, bool on)
{
    if (fd < 0) return;
    /* Only log errors when grabbing — releasing a non-grabbed fd
     * returns EINVAL which is harmless and expected (e.g. in cleanup). */
    if (ioctl(fd, EVIOCGRAB, on ? (void *)1 : (void *)0) < 0 && on)
        log_err("EVIOCGRAB");
}

/*
 * Touchscreen: grabbed when menu open OR screen blank.
 * fbkeyboard is a virtual device layered on the touchscreen;
 * grabbing the touchscreen implicitly disables it too.
 * Idempotent: only calls ioctl when the state actually changes.
 */
void update_grabs(void)
{
    bool want = (g_state == STATE_MENU || g_screen_blank);
    if (want == g_touch_grabbed) return;   /* no change needed */
    grab(g_fd_touch, want);
    g_touch_grabbed = want;
}

/* ── fbkeyboard OpenRC service ────────────────────────────────────────────── */
void fbkbd_set(bool start)
{
    char *const a_start[] = { "rc-service", "fbkeyboard", "start", NULL };
    char *const a_stop[]  = { "rc-service", "fbkeyboard", "stop",  NULL };
    if (run_cmd(start ? a_start : a_stop) == 0)
        g_fbkbd_on = start;
}

/* ── Brightness ──────────────────────────────────────────────────────────── */

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
    log_info("brightness -> %d", level);
}

/* ── Device open helper ───────────────────────────────────────────────────── */
int open_dev(const char *path, bool grab_now)
{
    int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) { log_err("open %s", path); return -1; }
    if (grab_now && ioctl(fd, EVIOCGRAB, (void *)1) < 0)
        log_err("EVIOCGRAB %s", path);
    return fd;
}
