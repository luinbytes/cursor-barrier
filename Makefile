CC      = gcc
CFLAGS  = -O2 -Wall -Wextra $(shell pkg-config --cflags libevdev)
LDFLAGS = $(shell pkg-config --libs libevdev)
PREFIX  = /usr/local

TARGET  = cursor-barrier
SRC     = cursor-barrier.c

.PHONY: all clean install uninstall

all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

install: $(TARGET)
	install -Dm755 $(TARGET) $(DESTDIR)$(PREFIX)/bin/$(TARGET)

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/$(TARGET)

clean:
	rm -f $(TARGET)
