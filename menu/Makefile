# Makefile — GT-I9100 pmOS menu

CC      := gcc
CFLAGS  := -O2 -Wall -Wextra -std=c11
TARGET  := gt-menu
PREFIX  := /usr/local/bin

SRCS    := main.c fsm.c menu.c actions.c render.c sysfs.c
OBJS    := $(SRCS:.c=.o)
DEPS    := $(SRCS:.c=.d)

# ── Primary targets ─────────────────────────────────────────────────────────
.PHONY: all clean install uninstall

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^

# ── Compile each .c → .o, generate .d dependency files alongside ─────────────
%.o: %.c
	$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

# Include generated dependency files (won't error if missing on first build)
-include $(DEPS)

# ── Housekeeping ─────────────────────────────────────────────────────────────
clean:
	rm -f $(OBJS) $(DEPS) $(TARGET)

install: $(TARGET)
	install -m 755 $(TARGET) $(PREFIX)/$(TARGET)

uninstall:
	rm -f $(PREFIX)/$(TARGET)
