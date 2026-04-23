#include "actions.h"
#include "fsm.h"
#include "menu.h"
#include "sysfs.h"

#include <stdio.h>
#include <string.h>

/* ── Battery ───────────────────────────────────────────────────────────────── */
void action_battery(void)
{
    /* Switch state; render() in the FSM handler will call render_battery_view */
    app_state = STATE_BATTERY_INFO;
}

/* ── Brightness ────────────────────────────────────────────────────────────── */
void action_brightness_enter(void)
{
    app_state = STATE_BRIGHTNESS;
}

/* ── WiFi ──────────────────────────────────────────────────────────────────── */
void action_wifi_toggle(void)
{
    FILE *fp = popen("nmcli -t -f WIFI radio", "r");
    if (!fp) { perror("popen nmcli"); return; }
    char state[16] = {0};
    if (fgets(state, sizeof(state), fp))
        state[strcspn(state, "\r\n")] = '\0';
    pclose(fp);

    if (strncmp(state, "enabled", 7) == 0) {
        char *const a[] = { "nmcli", "radio", "wifi", "off", NULL };
        run_cmd((char *const *)a);
    } else {
        char *const a[] = { "nmcli", "radio", "wifi", "on", NULL };
        run_cmd((char *const *)a);
    }
}

/* ── SSH ───────────────────────────────────────────────────────────────────── */
void action_ssh_restart(void)
{
    char *const a[] = { "rc-service", "sshd", "restart", NULL };
    run_cmd((char *const *)a);
}

/* ── Framebuffer keyboard ──────────────────────────────────────────────────── */
void action_fbkeyboard_toggle(void)
{
    char *const status[] = { "rc-service", "fbkeyboard", "status", NULL };
    char *const start[]  = { "rc-service", "fbkeyboard", "start",  NULL };
    char *const stop[]   = { "rc-service", "fbkeyboard", "stop",   NULL };
    run_cmd(run_cmd((char *const *)status) == 0
            ? (char *const *)stop
            : (char *const *)start);
}

/* ── Exit ──────────────────────────────────────────────────────────────────── */
void action_exit_menu(void)
{
    current_menu = &ROOT_MENU;
    selection    = 0;
    app_state    = STATE_IDLE;
}
