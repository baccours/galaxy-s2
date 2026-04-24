/*
 * system.c — low-level system helpers
 *
 * Owns: terminal raw/restore, fb_blank sysfs fd, run_cmd (fork+execvp),
 *       EVIOCGRAB wrappers, fbkeyboard OpenRC service, logging.
 *
 * Nothing here knows about menus or the FSM.
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

int g_fd_gpio     = -1;
int g_fd_touchkey = -1;
int g_fd_touch    = -1;
int g_fd_fbkbd    = -1;
int g_fd_fb       = -1;

/* ── Terminal ─────────────────────────────────────────────────────────────── */
static struct termios g_orig_termios;
static bool           g_termios_saved = false;

void terminal_raw(void)
{
    struct termios t;
    if (tcgetattr(STDOUT_FILENO, &t) != 0) return;
    g_orig_termios  = t;
    g_termios_saved = true;
    t.c_lflag &= (tcflag_t)~(ICANON | ECHO | ISIG);
    t.c_cc[VMIN]  = 1;
    t.c_cc[VTIME] = 0;
    tcsetattr(STDOUT_FILENO, TCSAFLUSH, &t);
}

void terminal_restore(void)
{
    if (g_termios_saved)
        tcsetattr(STDOUT_FILENO, TCSAFLUSH, &g_orig_termios);
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
void grab(int fd, bool on)
{
    if (fd < 0) return;
    if (ioctl(fd, EVIOCGRAB, on ? (void *)1 : (void *)0) < 0)
        log_err("EVIOCGRAB");
}

/*
 * Touchscreen: grabbed when menu open OR screen blank (disables touch input).
 * fbkeyboard device: grabbed when menu open (prevents stray chars).
 */
void update_grabs(void)
{
    bool menu_on = (g_state == STATE_MENU);
    grab(g_fd_touch, menu_on || g_screen_blank);
    grab(g_fd_fbkbd, menu_on);
}

/* ── fbkeyboard OpenRC service ────────────────────────────────────────────── */
void fbkbd_set(bool start)
{
    char *const a_start[] = { "rc-service", "fbkeyboard", "start", NULL };
    char *const a_stop[]  = { "rc-service", "fbkeyboard", "stop",  NULL };
    if (run_cmd(start ? a_start : a_stop) == 0)
        g_fbkbd_on = start;
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
