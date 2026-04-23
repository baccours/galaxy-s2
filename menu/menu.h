#ifndef MENU_H
#define MENU_H

#include <stdint.h>

/* ── Types ─────────────────────────────────────────────────────────────────── */
typedef struct Menu Menu;
typedef void (*ActionFn)(void);

typedef struct {
    const char *label;
    ActionFn    action;     /* Non-NULL → leaf: call on select   */
    const Menu *submenu;    /* Non-NULL → node: navigate into    */
} MenuItem;

struct Menu {
    const char     *title;
    const MenuItem *items;
    uint8_t         count;
    const Menu     *parent; /* NULL for root                     */
};

/* ── Navigation state (owned by menu.c, read by render.c/fsm.c) ───────────── */
extern const Menu  *current_menu;
extern uint8_t      selection;

/* ── Menu tree roots (defined in menu.c) ─────────────────────────────────── */
extern const Menu ROOT_MENU;
extern Menu       NETWORK_MENU;   /* non-const: parent linked at runtime */

/* ── Navigation API ───────────────────────────────────────────────────────── */
void menu_init(void);    /* link parent pointers — call once in main() */
void menu_select(void);  /* execute highlighted item                    */
void menu_back(void);    /* go to parent, or exit if already at root    */

#endif /* MENU_H */
