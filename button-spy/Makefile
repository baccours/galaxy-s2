CC      = gcc
CFLAGS  = -O2 -Wall -Wextra -std=c11
TARGET  = button-spy
SRCS    = button-spy.c menu.c system.c
OBJS    = $(SRCS:.c=.o)

.PHONY: all clean install

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^

%.o: %.c button-spy.h
	$(CC) $(CFLAGS) -c -o $@ $<

install: $(TARGET)
	install -Dm755 $(TARGET) /usr/local/bin/$(TARGET)
	strip --strip-all /usr/local/bin/$(TARGET)
	install -Dm644 button-spy_openrc /etc/init.d/button-spy
	rc-update add button-spy default
	install -Dm644 button-spy_logrotate /etc/logrotate.d/button-spy

clean:
	rm -f $(TARGET) $(OBJS)
