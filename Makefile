CC ?= cc
CFLAGS ?= -std=c11 -O2 -Wall -Wextra
GTK_CFLAGS := $(shell pkg-config --cflags gtksourceview-5)
GTK_LIBS := $(shell pkg-config --libs gtksourceview-5)
SUPPORT_SOURCES := document.c printing.c recovery.c
HEADERS := document.h printing.h recovery.h

all: quillmote

quillmote: main.c $(SUPPORT_SOURCES) $(HEADERS)
	$(CC) $(CFLAGS) $(GTK_CFLAGS) main.c $(SUPPORT_SOURCES) -o $@ $(GTK_LIBS) -ldl

run: quillmote
	./quillmote

test: tests/document tests/appearance tests/features tests/workflows tests/editor
	./tests/document
	./tests/appearance
	./tests/features
	./tests/workflows
	./tests/editor

tests/editor: tests/editor.c main.c $(SUPPORT_SOURCES) $(HEADERS)
	$(CC) $(CFLAGS) $(GTK_CFLAGS) tests/editor.c $(SUPPORT_SOURCES) -o $@ $(GTK_LIBS) -ldl

tests/workflows: tests/workflows.c main.c $(SUPPORT_SOURCES) $(HEADERS)
	$(CC) $(CFLAGS) $(GTK_CFLAGS) tests/workflows.c $(SUPPORT_SOURCES) -o $@ $(GTK_LIBS) -ldl

tests/features: tests/features.c main.c $(SUPPORT_SOURCES) $(HEADERS)
	$(CC) $(CFLAGS) $(GTK_CFLAGS) tests/features.c $(SUPPORT_SOURCES) -o $@ $(GTK_LIBS) -ldl

tests/document: tests/document.c document.c document.h
	$(CC) $(CFLAGS) $(GTK_CFLAGS) tests/document.c document.c -o $@ $(GTK_LIBS)

tests/appearance: tests/appearance.c main.c $(SUPPORT_SOURCES) $(HEADERS)
	$(CC) $(CFLAGS) $(GTK_CFLAGS) tests/appearance.c $(SUPPORT_SOURCES) -o $@ $(GTK_LIBS) -ldl

clean:
	rm -f quillmote tests/appearance tests/document tests/features tests/workflows tests/editor

.PHONY: all run test clean
