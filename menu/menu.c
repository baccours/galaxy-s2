#include "menu.h"
#include "actions.h"
#include "fsm.h"    /* action_exit_menu needs app_state */

/* ── Navigation state ─────────────────────────────────────────────────────── */
const Menu *current_menu = NULL;
uint8_t     selection    = 0;

/* ── Forward declarations (actions defined in actions.c) ──────────────────── */
/* All ActionFn pointers below are resolved at link time.                      */

/* ── Networking submenu ────────────────────────────────────────────────────── */
static const MenuItem NETWORK_ITEMS[] = {
    { "Toggle WiFi", action_wifi_toggle, NULL },
    { "Restart SSH",  action_ssh_restart, NULL },
};
Menu NETWORK_MENU = {
    .title  = "Networking",
    .items  = NETWORK_ITEMS,
    .count  = (uint8_t)(sizeof(NETWORK_ITEMS) / sizeof(NETWORK_ITEMS[0])),
    .parent = NULL  /* linked in menu_init() */
};

/* ── Root menu ─────────────────────────────────────────────────────────────── */
static const MenuItem ROOT_ITEMS[] = {
    { "Check Battery",     action_battery,           NULL          },
    { "Adjust Brightness", action_brightness_enter,  NULL          },
    { "Networking",        NULL,                     &NETWORK_MENU },
    { "Toggle Keyboard",   action_fbkeyboard_toggle, NULL          },
    { "Exit Menu",         action_exit_menu,         NULL          },
};
const Menu ROOT_MENU = {
    .title  = "GT-I9100 pmOS",
    .items  = ROOT_ITEMS,
    .count  = (uint8_t)(sizeof(ROOT_ITEMS) / sizeof(ROOT_ITEMS[0])),
    .parent = NULL
};

/* ── API ───────────────────────────────────────────────────────────────────── */
void menu_init(void)
{
    NETWORK_MENU.parent = &ROOT_MENU;
    current_menu = &ROOT_MENU;
    selection    = 0;
}

void menu_select(void)
{
    const MenuItem *item = &current_menu->items[selection];
    if (item->submenu) {
        current_menu = item->submenu;
        selection    = 0;
    } else if (item->action) {
        item->action();
    }
}

void menu_back(void)
{
    if (current_menu->parent) {
        current_menu = current_menu->parent;
        selection    = 0;
    } else {
        action_exit_menu();
    }
}
