# pmenu — postmarketOS hardware-button menu
## Samsung Galaxy S2 GT-I9100

A lightweight (~300 LOC), dependency-free C program that turns the
physical buttons into a full navigation system for a headless pmOS install.

---

### Architecture

```
┌─────────────────────────────────────────────────────┐
│                     main loop                        │
│             epoll on event0 + event1                 │
└──────────────┬──────────────────────────────────────┘
               │ input_event (EV_KEY)
               ▼
        handle_key(code, value)
               │
     ┌─────────┴──────────┐
     │   State Machine     │
     │  ┌──────────────┐   │
     │  │ NORMAL mode  │   │  MENU btn → open menu
     │  │              │   │  POWER    → toggle blank
     │  │              │   │  HOME     → toggle fbkbd
     │  └──────┬───────┘   │
     │         │ MENU btn  │
     │  ┌──────▼───────┐   │
     │  │  MENU mode   │   │  VOL+/- → navigate
     │  │              │   │  POWER  → select
     │  │              │   │  BACK   → back / close
     │  └──────────────┘   │
     └─────────────────────┘
               │
     ┌─────────▼──────────────┐
     │  Data-driven Menu Tree  │
     │                         │
     │  Root                   │
     │  ├─ Networking ──────►  │
     │  │   ├─ Toggle WiFi     │
     │  │   └─ Toggle BT       │
     │  ├─ Power ───────────►  │
     │  │   ├─ Reboot          │
     │  │   └─ Power Off       │
     │  └─ Exit Menu           │
     └─────────────────────────┘
```

---

### Button map

| Button   | Normal mode          | Menu open         | Screen blank      |
|----------|----------------------|-------------------|-------------------|
| POWER    | Toggle screen blank  | **Select** entry  | Unblank screen    |
| MENU     | Open menu            | Close menu        | *(ignored)*       |
| BACK     | *(ignored)*          | Back / close menu | *(ignored)*       |
| HOME     | Toggle fbkeyboard    | *(ignored)*       | *(ignored)*       |
| VOL +    | *(ignored)*          | Cursor up         | *(ignored)*       |
| VOL -    | *(ignored)*          | Cursor down       | *(ignored)*       |

---

### Input devices

| Node          | Device                  | Grab strategy                        |
|---------------|-------------------------|--------------------------------------|
| `/dev/input/event0` | gpio-keys         | Grabbed at startup (always)          |
| `/dev/input/event1` | tm2-touchkey      | Grabbed at startup (always)          |
| `/dev/input/event2` | Touchscreen       | Grabbed when menu open OR blank      |
| `/dev/input/event3` | fbkeyboard        | Grabbed when menu open               |

---

### Build & install

```sh
# On the device (or cross-compile)
gcc -O2 -Wall -o pmenu pmenu.c

# Install manually
cp pmenu /usr/local/bin/
chmod 755 /usr/local/bin/pmenu

# Install as OpenRC service
cp pmenu.openrc /etc/init.d/pmenu
chmod 755 /etc/init.d/pmenu
rc-update add pmenu default
rc-service pmenu start
```

**Cross-compile example (from x86 host):**
```sh
arm-linux-musleabihf-gcc -O2 -Wall -static -o pmenu pmenu.c
```

---

### Extending the menu

Add entries by editing the static `MenuItem` arrays in `pmenu.c`:

```c
/* Leaf action */
static MenuItem g_tools_submenu[] = {
    { "Run htop",  action_htop,  NULL, 0 },
    { "Run dmesg", action_dmesg, NULL, 0 },
};

/* Then add to root */
static MenuItem g_root_menu[] = {
    { "Networking", NULL, g_net_submenu,   2 },
    { "Power",      NULL, g_power_submenu, 2 },
    { "Tools",      NULL, g_tools_submenu, 2 },  /* ← new */
    { "Exit Menu",  action_exit, NULL, 0 },
};
```

No dynamic allocation is used; the menu tree is entirely static.

---

### Dependencies

- Linux kernel with `evdev` and `EVIOCGRAB` support (standard)
- `rfkill` for WiFi/Bluetooth toggling
- `fbkeyboard` OpenRC service (already present on pmOS)
- Nothing else — no ncurses, no libraries beyond libc

---

### Signals

`SIGTERM` / `SIGINT` / `SIGHUP` → clean shutdown (releases all grabs,
restores terminal, unblanks screen).
