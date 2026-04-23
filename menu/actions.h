#ifndef ACTIONS_H
#define ACTIONS_H

/* Called by menu items — signature must match ActionFn: void (*)(void) */
void action_battery(void);
void action_brightness_enter(void);
void action_wifi_toggle(void);
void action_ssh_restart(void);
void action_fbkeyboard_toggle(void);
void action_exit_menu(void);

#endif /* ACTIONS_H */
