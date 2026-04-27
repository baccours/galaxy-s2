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
	install -Dm644 button-spy.openrc /etc/init.d/button-spy
	rc-update add button-spy default

clean:
	rm -f $(TARGET) $(OBJS)
