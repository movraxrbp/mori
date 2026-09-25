CC ?= cc
PKG_CONFIG ?= pkg-config
LUA ?= lua5.4
PREFIX ?= /usr
CPPFLAGS += -D_POSIX_C_SOURCE=200809L
CFLAGS ?= -O2
CFLAGS += -std=c11 -Wall -Wextra -Wpedantic
WM_CFLAGS = $(shell $(PKG_CONFIG) --cflags x11 xrandr $(LUA))
WM_LIBS = $(shell $(PKG_CONFIG) --libs x11 xrandr $(LUA))

BAR_CFLAGS = $(shell $(PKG_CONFIG) --cflags x11 xft fontconfig $(LUA))
BAR_LIBS = $(shell $(PKG_CONFIG) --libs x11 xft fontconfig $(LUA))

WM_SRC = src/geometry.c src/mori.c src/lua.c src/ipc.c src/protocol.c
WM_HEADERS = src/geometry.h src/mori.h src/ipc.h src/protocol.h

all: mori mori-ipc
all: mori-bar
mori: $(WM_SRC) $(WM_HEADERS)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WM_CFLAGS) -o $@ $(WM_SRC) $(LDFLAGS) $(WM_LIBS)
mori-ipc: src/client.c src/protocol.c src/protocol.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ src/client.c src/protocol.c $(LDFLAGS)
mori-bar: src/bar.c src/bar.h src/protocol.c src/protocol.h
	$(CC) $(CPPFLAGS) $(CFLAGS) $(BAR_CFLAGS) -o $@ src/bar.c src/protocol.c $(LDFLAGS) $(BAR_LIBS)
install: all
	install -Dm755 mori $(DESTDIR)$(PREFIX)/bin/mori
	install -Dm755 mori-ipc $(DESTDIR)$(PREFIX)/bin/mori-ipc
	install -Dm755 mori-bar $(DESTDIR)$(PREFIX)/bin/mori-bar
	install -Dm644 config/bar.lua $(DESTDIR)$(PREFIX)/share/mori/bar.lua
	install -Dm644 config/config.lua $(DESTDIR)$(PREFIX)/share/mori/config.lua
	install -Dm644 mori.desktop $(DESTDIR)$(PREFIX)/share/xsessions/mori.desktop
uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/mori-bar
	rm -f $(DESTDIR)$(PREFIX)/share/mori/bar.lua
	rm -f $(DESTDIR)$(PREFIX)/bin/mori
	rm -f $(DESTDIR)$(PREFIX)/bin/mori-ipc
	rm -f $(DESTDIR)$(PREFIX)/share/mori/config.lua
	rmdir $(DESTDIR)$(PREFIX)/share/mori 2>/dev/null || true
	rm -f $(DESTDIR)$(PREFIX)/share/xsessions/mori.desktop
clean:
	rm -f mori mori-ipc mori-bar
.PHONY: all install uninstall clean
