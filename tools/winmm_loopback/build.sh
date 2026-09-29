#!/usr/bin/env bash
# Build the winmm loopback driver. Produces nvmidi-loopback.exe, here by
# default and wherever -o says otherwise.
#
# -o exists because the ci build job runs in build/ and the e2e job looks for
# the result at the repository path: an output location that only lived here
# made the e2e report the driver as not built at all, which reads as a compile
# failure and is not one.
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

OUT="nvmidi-loopback.exe"
if [ "${1:-}" = "-o" ]; then
	[ -n "${2:-}" ] || { echo "-o needs a path" >&2; exit 2; }
	# Resolved against the directory the caller is standing in, before the cd
	# above moves us. A relative -o that meant "right here" would silently
	# write into tools/winmm_loopback/ instead, and the caller - the ci job -
	# would go looking at the path it asked for and find nothing there.
	case "$2" in
		/*) OUT="$2" ;;
		*)  OUT="$OLDPWD/$2" ;;
	esac
fi

CC="${CC:-x86_64-w64-mingw32-gcc}"
command -v "$CC" >/dev/null || { echo "no $CC on PATH" >&2; exit 1; }

mkdir -p "$(dirname "$OUT")"
"$CC" -O2 -shared -municode -o "$OUT" nvmidi_loopback.c \
	-lwinmm -DUNICODE -D_UNICODE
echo "built $(wc -c < "$OUT") bytes at $OUT"

