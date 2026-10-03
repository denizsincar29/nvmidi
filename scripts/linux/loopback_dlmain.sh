#!/usr/bin/env bash
# Run the loopback driver's DllMain - the only DllMain in this repository -
# as what it is on linux: a shared library's initialiser.
#
# Why this exists. The windows runner starts the driver exactly once per run
# and then reads its wreck; anything learned there costs a full e2e cycle and
# no single failure can be told apart from a build that simply went stale. The
# linux job has the same file, thirty runs a minute and a compiler on hand.
#
# What it can and cannot see, in the step's own output so nobody reads more
# into a pass than is there:
#
#   covered  - DllMain runs to completion and returns TRUE; it writes the
#              status file it is supposed to write, from the same
#              write_status the windows build uses; it starts its thread and
#              that thread lives past the load; the module path it builds
#              from the loader's answer points at the copy that was loaded.
#   not      - winmm, the registry and the Drivers32 message protocol are
#              windows-only, so ModMessage, OpenDriver and the port count
#              are not touched here at all. On windows the driver is loaded
#              by winmm from a registry value; here it is never loaded by
#              anything but this script.
#
# So a pass here narrows the windows fault to the winmm side - and a failure
# here is the first load of this file that ever returned its own diagnosis
# instead of a stack trace.
set -eu

# This script has never been run on the machine it was written on: the
# Author's container has no C compiler at all, only a Rust toolchain with no
# C front end, and no root to add one. So the honest state of it is "written,
# reasoned through, and first compiled by the runner it runs on". Every line
# that could only be settled by a compiler is called out where it sits; this
# comment is the general warning that there is no local run behind the rest.
# The step runs it with the same shell and the same -e, so a compile error is
# a red job and not a silent pass.
#
# The compiler is CC, and the script says which one it picked, because the
# shim has to be linked against a driver source that expects mingw's headers
# in a few places and there is no second place to look for that.
CC="${CC:-cc}"
command -v "$CC" >/dev/null || {
	echo "no C compiler: \$$CC='$CC' is not on PATH" >&2
	echo "the linux runner has gcc; this script is a no-op anywhere else" >&2
	exit 1
}
echo "--- compiler"
"$CC" --version | head -1
echo "shim built for: $(${CC} -dumpmachine 2>/dev/null || echo unknown)"

here=$(cd -- "$(dirname -- "$0")" && pwd)
root=$(cd -- "$here/../.." && pwd)
work="${WORK:-$(mktemp -d)}"
keep="${KEEP:-0}"

driver="$root/tools/winmm_loopback/nvmidi_loopback.c"
loader="$here/loopback_dlmain.c"

[ -f "$driver" ] || { echo "no driver source at $driver" >&2; exit 2; }
[ -f "$loader" ] || { echo "no loader at $loader" >&2; exit 2; }

# The loader is this file's only compile unit and includes the driver source
# directly, so there is no second .c to keep in sync and no header to invent.
# -DUNICODE -D_UNICODE match the windows build: the status lines are wide
# strings there and that is the path being exercised.
echo "--- what is under test"
echo "driver: $driver ($(wc -c < "$driver") bytes)"
echo "loader: $loader"
grep -n 'PROCESS_ATTACH\|PROCESS_DETACH' "$driver" | sed 's/^/  /'

# A run without file handles open, to answer the one question the windows
# runaway cannot: does this file's DllMain come back on its own. It is a
# separate process, so a DllMain that hangs takes this line and the rest of
# the script is never reached.
echo "--- attempt: DllMain and return (no handles held)"
"$CC" -O2 -Wall -Wextra -o "$work/return" "$loader" -ldl -DUNICODE -D_UNICODE
if ! timeout 60 "$work/return" "$root/tools/winmm_loopback/nvmidi.dll.linux"; then
	echo "the loader could not dlopen the built driver; nothing below can mean anything"
	exit 1
fi

# The real attempt, from the same loader, holding the image the way winmm
# holds a device for the lifetime of the process.
echo "--- attempt: DllMain, thread held open"
"$CC" -O2 -Wall -Wextra -o "$work/hold" "$loader" -ldl -DUNICODE -D_UNICODE -DHOLD_OPEN
timeout 60 "$work/hold" "$root/tools/winmm_loopback/nvmidi.dll.linux"

echo "--- the file the driver wrote, straight from disk"
status="$root/tools/winmm_loopback/nvmidi.dll.linux.status"
if [ -f "$status" ]; then
	echo "$status: $(wc -c < "$status") byte(s)"
	sed 's/^/  | /' "$status"
else
	echo "$status: <no file>"
	echo "the driver did not write its status file at all"
	exit 1
fi

if [ "$keep" != "1" ]; then rm -rf "$work"; fi
