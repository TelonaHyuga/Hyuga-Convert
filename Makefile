# Hyuga Convert — Makefile
# Copyright (C) 2026 Telona Hyuga
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Arch Linux: sudo pacman -S --needed base-devel gtk3 ffmpeg

VERSION      := 1.0.0
CC           ?= cc
PKG_CONFIG   ?= pkg-config
GLIB_COMPILE_RESOURCES ?= glib-compile-resources

PREFIX       ?= /usr/local
BINDIR       ?= $(PREFIX)/bin
DATADIR      ?= $(PREFIX)/share
APPDIR       := $(DATADIR)/applications
ICONDIR      := $(DATADIR)/icons/hicolor/scalable/apps

GTK_CFLAGS   := $(shell $(PKG_CONFIG) --cflags gtk+-3.0)
GTK_LIBS     := $(shell $(PKG_CONFIG) --libs gtk+-3.0)

CPPFLAGS     += -DVERSION=\"$(VERSION)\" -DAPP_ID=\"com.hyuga.HyugaConvert\"
CFLAGS       ?= -O2 -g
CFLAGS       += -std=c11 -Wall -Wextra -Wpedantic -Wshadow -Wformat=2
CFLAGS       += $(GTK_CFLAGS)
LDLIBS       += $(GTK_LIBS)

BUILD_DIR    := build
SRC          := src/main.c src/media.c
OBJ          := $(SRC:src/%.c=$(BUILD_DIR)/%.o)
RES_C        := $(BUILD_DIR)/resources.c
RES_OBJ      := $(BUILD_DIR)/resources.o

# Nautilus integration is optional at build time. Nautilus itself depends on
# libnautilus-extension, so it is normally present on a desktop system.
NAUTILUS_PC  := libnautilus-extension-4
HAVE_NAUTILUS := $(shell $(PKG_CONFIG) --exists $(NAUTILUS_PC) && echo yes)
ifeq ($(HAVE_NAUTILUS),yes)
NAUTILUS_CFLAGS := $(shell $(PKG_CONFIG) --cflags $(NAUTILUS_PC))
NAUTILUS_LIBS   := $(shell $(PKG_CONFIG) --libs $(NAUTILUS_PC))
NAUTILUS_EXT_DIR := $(shell $(PKG_CONFIG) --variable=extensiondir $(NAUTILUS_PC))
NAUTILUS_OBJ    := $(BUILD_DIR)/hyuga-nautilus.o
NAUTILUS_EXT    := $(BUILD_DIR)/libhyuga-nautilus.so
endif

DEPFILES     := $(OBJ:.o=.d) $(RES_OBJ:.o=.d) $(NAUTILUS_OBJ:.o=.d)

.PHONY: all clean install uninstall test run

all: hyuga-convert $(NAUTILUS_EXT)

hyuga-convert: $(OBJ) $(RES_OBJ)
	$(CC) $(OBJ) $(RES_OBJ) $(LDFLAGS) $(LDLIBS) -o $@

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

$(RES_C): data/resources.gresource.xml data/style.css data/hyuga-convert.svg | $(BUILD_DIR)
	$(GLIB_COMPILE_RESOURCES) --sourcedir=data --target=$@ \
		--generate-source --c-name=hyuga_resources $<

$(BUILD_DIR)/%.o: src/%.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c $< -o $@

$(RES_OBJ): $(RES_C) | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -Wno-overlength-strings -MMD -MP -c $< -o $@

$(NAUTILUS_OBJ): src/nautilus/hyuga-nautilus.c src/input_formats.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(NAUTILUS_CFLAGS) -fPIC \
		-MMD -MP -c $< -o $@

$(NAUTILUS_EXT): $(NAUTILUS_OBJ)
	$(CC) -shared -fPIC -o $@ $< $(LDFLAGS) $(NAUTILUS_LIBS)

test: hyuga-convert
	tests/smoke.sh

run: hyuga-convert
	./hyuga-convert

install: hyuga-convert $(NAUTILUS_EXT)
	install -d "$(DESTDIR)$(BINDIR)"
	install -d "$(DESTDIR)$(APPDIR)"
	install -d "$(DESTDIR)$(ICONDIR)"
	install -m 755 hyuga-convert "$(DESTDIR)$(BINDIR)/hyuga-convert"
	install -m 644 data/hyuga-convert.desktop "$(DESTDIR)$(APPDIR)/hyuga-convert.desktop"
	install -m 644 data/hyuga-convert.svg "$(DESTDIR)$(ICONDIR)/hyuga-convert.svg"
ifneq ($(NAUTILUS_EXT),)
	install -d "$(DESTDIR)$(NAUTILUS_EXT_DIR)"
	install -m 755 $(NAUTILUS_EXT) "$(DESTDIR)$(NAUTILUS_EXT_DIR)/libhyuga-nautilus.so"
endif
	@if [ -z "$(DESTDIR)" ]; then \
		if command -v gtk-update-icon-cache >/dev/null 2>&1; then \
			gtk-update-icon-cache -q -t -f "$(DATADIR)/icons/hicolor" >/dev/null 2>&1 || true; \
		fi; \
		if command -v update-desktop-database >/dev/null 2>&1; then \
			update-desktop-database -q "$(APPDIR)" >/dev/null 2>&1 || true; \
		fi; \
	fi

uninstall:
	rm -f "$(DESTDIR)$(BINDIR)/hyuga-convert"
	rm -f "$(DESTDIR)$(APPDIR)/hyuga-convert.desktop"
	rm -f "$(DESTDIR)$(ICONDIR)/hyuga-convert.svg"
ifneq ($(NAUTILUS_EXT),)
	rm -f "$(DESTDIR)$(NAUTILUS_EXT_DIR)/libhyuga-nautilus.so"
endif

clean:
	rm -rf $(BUILD_DIR) hyuga-convert

-include $(DEPFILES)
