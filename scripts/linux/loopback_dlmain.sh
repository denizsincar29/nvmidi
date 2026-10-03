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
#              status file it is supposed to write, through the same
#              write_status the windows build uses; it starts a thread and
#              that thread does its first steps; the file it builds its own
#              name from is the file that was loaded.
#   not      - winmm, the registry and the Drivers32 message protocol are
#              windows-only, so ModMessage, OpenDriver and the port count are
#              not touched here at all. On windows the driver is loaded by
#              winmm from a registry value; here nothing loads it but this
#              script, and its registry write is a stub that answers
#              not-found on purpose.
#
# So a pass here narrows the windows fault to the winmm side - and a failure
# here is the first load of this file that ever returned its own diagnosis
# instead of a stack trace.
set -eu

CC="${CC:-cc}"
command -v "$CC" >/dev/null || {
	echo "no C compiler: \$CC='$CC' is not on PATH" >&2
	exit 1
}

here=$(cd -- "$(dirname -- "$0")" && pwd)
root=$(cd -- "$here/../.." && pwd)
work="${WORK:-$(mktemp -d)}"
keep="${KEEP:-0}"

driver="$root/tools/winmm_loopback/nvmidi_loopback.c"
loader="$here/loopback_dlmain.c"
# The driver names its status file after the module it was loaded as, which is
# the running executable here (the shim answers GetModuleFileNameA with
# /proc/self/exe). So the file lands beside the binary, not in the repository -
# and that is deliberate, not an accident of the shim: the same rule puts it
# beside the dll on windows, and the point is to read the file the driver
# itself chose rather than one this script decided on.
status_for() { printf '%s' "${1%.*}.status"; }

[ -f "$driver" ] || { echo "no driver source at $driver" >&2; exit 2; }
[ -f "$loader" ] || { echo "no loader at $loader" >&2; exit 2; }

echo "--- compiler"
"$CC" --version | head -1

echo "--- what is under test"
echo "driver: $driver ($(wc -c < "$driver") bytes)"
echo "loader: $loader"
grep -n 'PROCESS_ATTACH\|PROCESS_DETACH' "$driver" | sed 's/^/  /'

# The two runs are one binary each and they are exactly the two questions
# nobody has ever been able to ask the windows runner: does this function
# come back on its own, and does the thread it starts do anything before the
# process ends.
# The -I is for the driver's own include, and it is not decoration. The driver
# says #include "nvmidi_win_compat.h", and a quoted include is resolved first
# against the directory of the file the directive sits in - which here is
# tools/winmm_loopback/, and the header is there, so the form that looks
# correct is correct. What it is *not* resolved against is the directory of
# loopback_dlmain.c, so nothing about this loader's own -I scripts/linux
# reaches it. That is the same mechanism as before this header existed: the
# driver's quoted include of the shim was on the windows side of the sentinels
# and only that side ever read it. The -I is here so the build does not depend
# on the compiler agreeing about which directory a quoted include belongs to
# when the source was reached through another file; the header lives in that
# directory and the flag says so out loud.
driver_inc="$root/tools/winmm_loopback"

echo "--- attempt 1: DllMain, attach and return"
status=$(status_for "$work/attach")
rm -f "$status"
LOOPBACK_MODE=attach "$CC" -O2 -Wall -I "$driver_inc" -o "$work/attach" "$loader" -lpthread
timeout 60 "$work/attach" || { echo "attach run failed" >&2; exit 1; }

# DllMain overwrites its own status file with every write (fopen "wb"), and
# its last write is ATTACH=returning, so the earlier marks are gone by now.
# The copy taken between the two runs is the only place the first marks
# survive, and it is taken from disk rather than from the driver's output.
if [ -f "$status" ]; then
	echo "--- status after attach (the only marks DllMain left)"
	sed 's/^/  | /' "$status"
	cp "$status" "$work/attach.status.copy"
else
	echo "the driver wrote no status file at all" >&2
	exit 1
fi

echo "--- attempt 2: the publish thread on a real thread, 3s"
status=$(status_for "$work/pthread")
rm -f "$status"
LOOPBACK_MODE=pthread "$CC" -O2 -Wall -I "$driver_inc" -o "$work/pthread" "$loader" -lpthread
LOOPBACK_MODE=pthread timeout 60 "$work/pthread"
if [ -f "$status" ]; then
	echo "--- status after the thread ran"
	sed 's/^/  | /' "$status"
else
	echo "the thread wrote no status file at all" >&2
	exit 1
fi

echo "--- verdict"
echo "attach status: $(wc -c < "$work/attach.status.copy") byte(s)"
echo "thread status: $(wc -c < "$status") byte(s)"
echo "the thread run reached the driver's registry stub $(grep -c 'registry' "$status" || true) time(s) - not a windows result"

[ "$keep" = "1" ] || rm -rf "$work"
