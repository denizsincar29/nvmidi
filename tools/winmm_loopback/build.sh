#!/usr/bin/env bash
# Build the winmm loopback driver. Produces nvmidi.dll, here by default and
# wherever -o says otherwise.
#
# The name is nvmidi.dll and not nvmidi-loopback.dll, and that is not
# cosmetic. A winmm driver registers its own module path under
# CurrentVersion\Drivers32 and winmm then loads *that* image and calls into
# it. It is a second copy of the file, in the same process, and both copies
# run the same DllMain into the same publish path - so the loader hands the
# second one the first one's base, GetModuleFileNameW answers the same path
# for both, and OpenDriver then finds the slot already open and answers
# HDRVR(-1) for the *driver's own* call into its sibling image.
#
# That is the whole of the open-failed diagnosis. Measured on run 36577168077:
# the status read "open-failed slot=midi1 error=0". error=0 is not an error
# being hidden, it is the honest absence of one - winmm had nothing to report
# because it had in fact already opened the file. The driver had published its
# port, and the plugin was pointed at a different library.
#
# Two different files in one process is the defect; the name is the fix. With
# one name there is no second path to register, so there is no second module,
# and OpenDriver on the slot is then a call winmm can actually serve.
#
# -o exists because the ci build job runs in build/ and the e2e job looks for
# the result at the repository path: an output location that only lived here
# made the e2e report the driver as not built at all, which reads as a compile
# failure and is not one.
#
# A user mode winmm driver is the same thing as the .drv files windows itself
# ships, and winmm reaches it by the path in the registry rather than by
# module name, so the extension only has to be one the loader accepts. .dll is
# what it is, and it is also the name the plugin already looks for.
#
# Needs mingw-w64 (x86_64-w64-mingw32-gcc) on PATH. On the windows runner it is
# already there.
set -eu
cd "$(dirname "$0")"

OUT="nvmidi.dll"
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

