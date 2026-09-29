# Contributing

Quillmote is a simple, solid text editor. Changes should make everyday editing easier without adding an IDE, accounts, services, or a plugin framework.

## Development

Use a C11 compiler, GNU Make, pkg-config, GTK 4.12 or newer, GtkSourceView 5, and GLib development tools. Build with `make`. The application is in `main.c`; file encoding, printing, recovery, session persistence, and Help/About live in separate modules. Shared application metadata is in `app-info.h`.

Run `make test` in a graphical session and `make test-cli check-desktop` before submitting changes. The GUI tests open temporary windows and use temporary files and preferences; printing checks export PDFs. Explain the change and how you verified it. Include steps to reproduce bugs and the version from `quillmote --version`.

Preserve unsaved work across cancellation and errors. Keep long operations responsive. Add meaningful regression coverage for document lifecycle changes, and use the existing native widgets and system appearance.

The memory test checks repeated file loads, document object destruction, and RAM release after tab closure. It uses a 32 MiB file by default. For a larger manual check, run `QUILLMOTE_MEMORY_TEST_MIB=128 make test TESTS=tests/memory` (32–1024 MiB). On Linux with glibc it also checks that resident memory falls after closing; GTK and allocator caches mean it need not return to exactly the startup value.

## Preparing a release

1. Update the version in `app-info.h` and the changelog.
2. Run a clean build, the full tests, and desktop-file validation.
3. Stage installation with `make install DESTDIR=/tmp/quillmote-stage PREFIX=/usr` and verify the installed paths and launcher.
4. Run `make dist`, extract the source archive into a fresh directory, and build it there.
5. Share the source archive and its SHA-256 file from `dist/`. Publish or tag only after reviewing the result.

The source archive includes the MIT license, sources, tests, documentation, and desktop assets. It excludes binaries, local preferences, Git metadata, and generated build files. Builds use system libraries and do not download dependencies. A compiled Linux binary needs compatible versions of GTK and GtkSourceView on the target system.
