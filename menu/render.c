#include "render.h"
#include "fsm.h"
#include "menu.h"
#include "sysfs.h"
#include "config.h"

#include <stdio.h>

/* ── Private view renderers ────────────────────────────────────────────────── */
static void render_menu_view(void)
{
    /* Breadcrumb: reverse-walk parent chain to print root → current */
    const Menu *path[16];
    int depth = 0;
    for (const Menu *m = current_menu; m && depth < 16; m = m->parent)
        path[depth++] = m;
    for (int i = depth - 1; i >= 0; i--) {
        if (i < depth - 1) fputs(" > ", stdout);
        fputs(path[i]->title, stdout);
    }
    fputs("\r\n\r\n", stdout);

    for (uint8_t i = 0; i < current_menu->count; i++) {
        bool selected = (i == selection);
        bool has_sub  = (current_menu->items[i].submenu != NULL);
        if (selected)
            printf("\033[7m > %-22s%s\033[0m\r\n",
                   current_menu->items[i].label, has_sub ? " >" : "  ");
        else
            printf("   %-22s%s\r\n",
                   current_menu->items[i].label, has_sub ? " >" : "  ");
    }
    fputs("\r\n[Vol+/-]: Nav  [Power]: Select  [Back]: Back\r\n", stdout);
}

static void render_brightness_view(void)
{
    printf("--- Brightness ---\r\n\r\n"
           " Level: %u / %d\r\n [",
           current_brightness, BRIGHTNESS_MAX);
    for (int i = 0; i < BRIGHTNESS_MAX; i++)
        putchar(i < current_brightness ? '#' : '-');
    fputs("]\r\n\r\n[Vol+/-]: Change  [Power/Back]: Done\r\n", stdout);
}

static void render_battery_view(void)
{
    char buf[16] = "??";
    read_sysfs(BATTERY_FILE, buf, sizeof(buf));
    printf(" Battery: %s%%\r\n\r\n [Any key to return]\r\n", buf);
}

/* ── Public entry point ────────────────────────────────────────────────────── */
void render(void)
{
    fputs("\033[H\033[J", stdout); /* clear screen */
    switch (app_state) {
        case STATE_MENU:         render_menu_view();       break;
        case STATE_BRIGHTNESS:   render_brightness_view(); break;
        case STATE_BATTERY_INFO: render_battery_view();    break;
        case STATE_IDLE:         /* nothing to draw */     break;
        default:                                           break;
    }
    fflush(stdout);
}
