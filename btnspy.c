#include <linux/input.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <poll.h>
#include <string.h>
#include <signal.h>

/** Device Nodes
  * event0 : gpio-keys (Power, Vol, Home)
  * event1 : tm2-touchkey (Back, Menu)
  * event2 : Atmel maXTouch Touchscreen
  * event3 : fbkeyboard
 **/
#define BTN_DEV "/dev/input/event0"
#define TCH_KEY "/dev/input/event1"
#define TCH_SCN "/dev/input/event2"
#define FB_BLANK "/sys/class/graphics/fb0/blank"
#define BRIGHTNESS_FILE "/sys/class/backlight/spi3.0/brightness"

// Menu Config
#define MENU_SIZE 5
int in_menu = 0;
int selection = 0;
int adjusting_brightness = 0;
int current_brightness = 8; // Default
volatile sig_atomic_t keep_running = 1;

const char *options[] = {
    "Check Battery Status",
    "Adjust Brightness",
    "Toggle WiFi (nmcli)",
    "Restart SSH Service",
    "Exit Menu"
};

// Signal handler for clean exit
void handle_sig(int sig) { keep_running = 0; }

void set_brightness(int level) {
    if (level < 0) level = 0;
    if (level > 24) level = 24;
    current_brightness = level;
    
    int fd = open(BRIGHTNESS_FILE, O_WRONLY);
    if (fd >= 0) {
        char buf[8];
        int len = snprintf(buf, sizeof(buf), "%d", level);
        write(fd, buf, len);
        close(fd);
    }
}

void draw_menu(int sel) {
    // \033[H = Move to top left, \033[J = Clear from cursor down
    printf("\033[H\033[J");
    
    if (adjusting_brightness) {
        printf("--- Brightness Adjustment ---\r\n\r\n");
        printf(" Level: %d / 24,\r\n", current_brightness);
        printf(" [");
        for(int i=0; i<24; i++) printf(i < current_brightness ? "#" : "-");
        printf("]\r\n\r\n");
        printf("[Vol +/-]: Change  [Power]: Save/Back");
    } else {
        printf("--- GT-I9100 pmOS Menu ---\r\n\r\n");
        for (int i = 0; i < MENU_SIZE; i++) {
            if (i == sel) {
                // \033[7m Invert colors for selection
                printf("\033[7m > %s \033[0m\r\n", options[i]);
            } else {
                printf("   %s \r\n", options[i]);
            }
        }
        printf("\r\n[Vol +/-]: Nav  [Power]: Select  [Home]: Kbd");
    }
    fflush(stdout);
}

int main() {
    struct pollfd fds[3];
    struct input_event ev;
    struct timespec start_time, end_time;
    int fb_fd = -1;

    // Set up signals
    signal(SIGINT, handle_sig);
    signal(SIGTERM, handle_sig);

    // Initialize fds
    fb_fd = open(FB_BLANK, O_RDWR);
    fds[0].fd = open(BTN_DEV, O_RDONLY | O_NONBLOCK);
    fds[1].fd = open(TCH_KEY, O_RDONLY | O_NONBLOCK);
    fds[2].fd = open(TCH_SCN, O_RDONLY | O_NONBLOCK);

    if (fds[0].fd < 0 || fds[1].fd < 0 || fds[2].fd < 0 || fb_fd < 0) {
        perror("Failed to open device nodes");
        goto cleanup;
    }

    /* GRAB all devices to prevent kernel VT from waking up */ 
    ioctl(fds[0].fd, EVIOCGRAB, 1);
    ioctl(fds[1].fd, EVIOCGRAB, 1);
    ioctl(fds[2].fd, EVIOCGRAB, 1);

    for(int i=0; i<3; i++) 
        fds[i].events = POLLIN;

    while (keep_running && poll(fds, 3, -1) > 0) {
        
        /* Handle Physical Buttons (event0) */
        if (fds[0].revents & POLLIN) {
            while (read(fds[0].fd, &ev, sizeof(ev)) > 0) {
                if (ev.type == EV_KEY) {
                    
                    // Logic for Navigation (Vol Up/Down)
                    if (in_menu && ev.value == 1) {
                        if (ev.code == 115) { // Vol Up
                            if (adjusting_brightness) {
                                set_brightness(current_brightness + 1);
                            } else {
                                selection = (selection > 0) ? selection - 1 : MENU_SIZE - 1;
                            }
                            draw_menu(selection);
                            continue;
                        } else if (ev.code == 114) { // Vol Down
                            if (adjusting_brightness) {
                                set_brightness(current_brightness - 1);
                            } else {
                                selection = (selection < MENU_SIZE - 1) ? selection + 1 : 0;
                            }
                            draw_menu(selection);
                            continue;
                        }
                    }

                    // Home Button (Toggle fbkeyboard)
                    if (ev.code == 352 && ev.value == 1) { // Home Button
                        system("sudo rc-service fbkeyboard status >/dev/null && sudo rc-service fbkeyboard stop || sudo rc-service fbkeyboard start");
                    }

                    // Power Button Logic (Modified to handle menu selection)
                    if (ev.code == 116) { // Power Button
                        if (ev.value == 1) { // Pressed 
                            clock_gettime(CLOCK_MONOTONIC, &start_time);
                        } else if (ev.value == 0) { // Released
                            clock_gettime(CLOCK_MONOTONIC, &end_time);
                            double diff = end_time.tv_sec - start_time.tv_sec;
                            
                            if (diff >= 5) {
                                system("poweroff");
                            } else if (diff >= 2) {
                                system("reboot");
                            } else {
                                if (in_menu) {
                                    if (adjusting_brightness) {
                                        adjusting_brightness = 0;
                                        draw_menu(selection);
                                    } else {
                                        switch(selection) {
                                            case 0: 
                                                printf("\r\nBattery: "); fflush(stdout);
                                                system("cat /sys/class/power_supply/battery/capacity"); 
                                                sleep(2);
                                                draw_menu(selection);
                                                break;
                                            case 1: 
                                                adjusting_brightness = 1; 
                                                draw_menu(selection);
                                                break;
                                            case 2: 
                                                system("nmcli radio wifi $(nmcli radio wifi | grep -q enabled && echo off || echo on)"); 
                                                break;
                                            case 3: 
                                                system("sudo rc-service sshd restart"); 
                                                break;
                                            case 4: 
                                                in_menu = 0; 
                                                printf("\033[H\033[J"); 
                                                break;
                                        }
                                    }
                                } else {
                                    // Manual Toggle Blanking
                                    char state, val;
                                    if (pread(fb_fd, &state, 1, 0) > 0) { // Read from offset 0
                                        val = (state == '0') ? '1' : '0';
                                        pwrite(fb_fd, &val, 1, 0); // Write to offset 0
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }

        /* Handle Touch Keys (event1) and Touchscreen (event2) */
        if (fds[1].revents & POLLIN) {
            while (read(fds[1].fd, &ev, sizeof(ev)) > 0) {
                if (ev.type == EV_KEY && ev.value == 1) {
                    if (ev.code == 139) { // Menu Key
                        write(fb_fd, "0", 1);
                        in_menu = !in_menu;
                        adjusting_brightness = 0;
                        if (in_menu) draw_menu(selection);
                        else printf("\033[H\033[J");
                    }
                    if (ev.code == 158) { // Back Key
                        if (adjusting_brightness) {
                            adjusting_brightness = 0;
                            draw_menu(selection);
                        } else {
                            //system("pkill -TERM whiptail htop top ping");
                            in_menu = 0;
                            printf("\r\nInterrupted.\r\n");
                        }
                    }
                }
            }
        }
        
        if (fds[2].revents & POLLIN) {
            while (read(fds[2].fd, &ev, sizeof(ev)) > 0);
        }
    }

cleanup:
    printf("\nReleasing devices and exiting...\n");
    if (fds[0].fd >= 0) { ioctl(fds[0].fd, EVIOCGRAB, 0); close(fds[0].fd); }
    if (fds[1].fd >= 0) { ioctl(fds[1].fd, EVIOCGRAB, 0); close(fds[1].fd); }
    if (fds[2].fd >= 0) { ioctl(fds[2].fd, EVIOCGRAB, 0); close(fds[2].fd); }
    if (fb_fd >= 0) close(fb_fd);
    return 0;
}
