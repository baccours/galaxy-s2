CC      = gcc
CFLAGS  = -O2 -Wall -Wextra -Wpedantic -std=c11
TARGET  = pmenu
SRC     = pmenu.c

.PHONY: all clean install

all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) -o $@ $^

install: $(TARGET)
	install -Dm755 $(TARGET) /usr/local/bin/$(TARGET)
	install -Dm644 pmenu.openrc /etc/init.d/pmenu
	rc-update add pmenu default

clean:
	rm -f $(TARGET)
