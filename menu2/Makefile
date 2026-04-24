CC      = gcc
CFLAGS  = -O2 -Wall -Wextra -std=c11
TARGET  = main
SRCS    = main.c menu.c system.c
OBJS    = $(SRCS:.c=.o)

.PHONY: all clean install

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^

%.o: %.c main.h
	$(CC) $(CFLAGS) -c -o $@ $<

install: $(TARGET)
	install -Dm755 $(TARGET) /usr/local/bin/$(TARGET)
	install -Dm644 pmenu.openrc /etc/init.d/menu
	rc-update add menu default

clean:
	rm -f $(TARGET) $(OBJS)
