CC ?= cc
PKG_CONFIG ?= pkg-config
GLIB_COMPILE_RESOURCES ?= glib-compile-resources
INSTALL ?= install
CFLAGS ?= -std=c11 -O2 -Wall -Wextra
PREFIX ?= /usr/local
BINDIR ?= $(PREFIX)/bin
DATADIR ?= $(PREFIX)/share
MANDIR ?= $(DATADIR)/man
DESTDIR ?=

VERSION := $(shell sed -n 's/^\#define QUILLMOTE_VERSION "\(.*\)"/\1/p' app-info.h)
APP_ID := org.quillmote.Quillmote
GTK_CFLAGS = $(shell $(PKG_CONFIG) --cflags gtksourceview-5)
GTK_LIBS = $(shell $(PKG_CONFIG) --libs gtksourceview-5)
SUPPORT_SOURCES := session.c document.c printing.c recovery.c help.c fileio.c build/resources.c
HEADERS := session.h document.h printing.h recovery.h help.h fileio.h app-info.h
OBJECTS := build/session.o build/main.o build/document.o build/printing.o build/recovery.o build/help.o build/fileio.o build/resources.o
GUI_TESTS := tests/appearance tests/themes tests/features tests/workflows tests/editor tests/help tests/memory tests/session tests/file-changes
TESTS := tests/document tests/storage $(GUI_TESTS)
DIST_FILES := Makefile README.md LICENSE CHANGELOG.md CONTRIBUTING.md app-info.h main.c session.c session.h document.c document.h printing.c printing.h recovery.c recovery.h help.c help.h fileio.c fileio.h .gitignore data docs tests/*.c tests/*.h tools/*.sh

all: quillmote

check-deps:
	@$(PKG_CONFIG) --exists 'gtk4 >= 4.12' 'gtksourceview-5 >= 5.0' || { echo 'Quillmote requires GTK >= 4.12 and GtkSourceView >= 5.0 development packages.' >&2; exit 1; }
	@command -v $(GLIB_COMPILE_RESOURCES) >/dev/null || { echo 'Install glib-compile-resources (GLib development tools).' >&2; exit 1; }

quillmote: $(OBJECTS)
	$(CC) $(LDFLAGS) $(OBJECTS) -o $@ $(GTK_LIBS) -ldl $(LDLIBS)

build/%.o: %.c | check-deps
	@mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(GTK_CFLAGS) -MMD -MP -c $< -o $@

build/resources.c: data/quillmote.gresource.xml data/$(APP_ID).svg
	@mkdir -p build
	$(GLIB_COMPILE_RESOURCES) --sourcedir=data --generate-source --target=$@ $<

build/resources.o: build/resources.c | check-deps
	$(CC) $(CPPFLAGS) $(CFLAGS) $(GTK_CFLAGS) -MMD -MP -c $< -o $@

-include $(OBJECTS:.o=.d)

run: quillmote
	./quillmote

$(GUI_TESTS): %: %.c main.c $(SUPPORT_SOURCES) $(HEADERS) tests/ui.h | check-deps
	$(CC) $(CPPFLAGS) $(CFLAGS) $(GTK_CFLAGS) $< $(SUPPORT_SOURCES) $(LDFLAGS) -o $@ $(GTK_LIBS) -ldl $(LDLIBS)

tests/document: tests/document.c document.c document.h | check-deps
	$(CC) $(CPPFLAGS) $(CFLAGS) $(GTK_CFLAGS) tests/document.c document.c $(LDFLAGS) -o $@ $(GTK_LIBS) $(LDLIBS)

tests/storage: tests/storage.c document.c recovery.c fileio.c document.h recovery.h fileio.h | check-deps
	$(CC) $(CPPFLAGS) $(CFLAGS) $(GTK_CFLAGS) tests/storage.c document.c recovery.c fileio.c $(LDFLAGS) -o $@ $(GTK_LIBS) $(LDLIBS)

test: $(TESTS)
	@set -e; for test in $(TESTS); do ./$$test; done

test-cli: quillmote
	./tools/test-cli.sh

check-desktop:
	desktop-file-validate data/$(APP_ID).desktop

install: quillmote
	$(INSTALL) -Dm755 quillmote "$(DESTDIR)$(BINDIR)/quillmote"
	$(INSTALL) -Dm644 data/$(APP_ID).desktop "$(DESTDIR)$(DATADIR)/applications/$(APP_ID).desktop"
	$(INSTALL) -Dm644 data/$(APP_ID).svg "$(DESTDIR)$(DATADIR)/icons/hicolor/scalable/apps/$(APP_ID).svg"
	$(INSTALL) -Dm644 docs/quillmote.1 "$(DESTDIR)$(MANDIR)/man1/quillmote.1"
	$(INSTALL) -Dm644 LICENSE "$(DESTDIR)$(DATADIR)/licenses/quillmote/LICENSE"
	$(INSTALL) -Dm644 README.md "$(DESTDIR)$(DATADIR)/doc/quillmote/README.md"

uninstall:
	rm -f "$(DESTDIR)$(BINDIR)/quillmote" \
		"$(DESTDIR)$(DATADIR)/applications/$(APP_ID).desktop" \
		"$(DESTDIR)$(DATADIR)/icons/hicolor/scalable/apps/$(APP_ID).svg" \
		"$(DESTDIR)$(MANDIR)/man1/quillmote.1" \
		"$(DESTDIR)$(DATADIR)/licenses/quillmote/LICENSE" \
		"$(DESTDIR)$(DATADIR)/doc/quillmote/README.md"

dist:
	@mkdir -p dist
	tar --transform='s,^,quillmote-$(VERSION)/,' -czf dist/quillmote-$(VERSION).tar.gz $(DIST_FILES)
	cd dist && sha256sum quillmote-$(VERSION).tar.gz > quillmote-$(VERSION).tar.gz.sha256

clean:
	rm -f quillmote $(TESTS) $(OBJECTS) $(OBJECTS:.o=.d) build/resources.c

.PHONY: all check-deps run test test-cli check-desktop install uninstall dist clean
