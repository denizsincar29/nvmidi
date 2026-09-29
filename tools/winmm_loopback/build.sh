#!/usr/bin/env bash
# Build the winmm loopback driver. Produces nvmidi-loopback.exe in this
# directory.
#
# The extension is .exe and not .dll on purpose: it is a user mode winmm
# driver, the same thing as the .drv files windows itself ships, and winmm is
# pointed at the file by path rather than by module name, so the extension only
# has to be one the loader accepts. A .drv is what the older tools expect and a
# .exe is what the ci job can download and hand to LoadLibrary without
# questions. The file is the same either way.
#
# Needs mingw-w64 (x86_64-w64-mingw32-gcc) on PATH. On the windows runner it is
# already there.
set -eu
cd "$(dirname "$0")"

CC="${CC:-x86_64-w64-mingw32-gcc}"
command -v "$CC" >/dev/null || { echo "no $CC on PATH" >&2; exit 1; }

"$CC" -O2 -shared -municode -o nvmidi-loopback.exe nvmidi_loopback.c \
	-lwinmm -DUNICODE -D_UNICODE
echo "built $(wc -c < nvmidi-loopback.exe) bytes"
