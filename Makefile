CC      = gcc
CFLAGS  = -O2 -Wall -Wextra $(shell pkg-config --cflags libevdev)
LDFLAGS = $(shell pkg-config --libs libevdev)
PREFIX  = /usr/local

TARGET  = cursor-barrier
SRC     = cursor-barrier.c

.PHONY: all clean install uninstall test

all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

install: $(TARGET)
	install -Dm755 $(TARGET) $(DESTDIR)$(PREFIX)/bin/$(TARGET)

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/$(TARGET)

clean:
	rm -f $(TARGET)

test:
	$(CC) -Wall -Wextra -o test_match tests/test_match.c $(shell pkg-config --cflags --libs libevdev)
	./test_match
	rm -f test_match
