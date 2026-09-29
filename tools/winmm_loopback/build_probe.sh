#!/usr/bin/env bash
# Build one of the probe binaries. Not part of the plugin's build - this is the
# measurement apparatus, kept next to the driver it measures because the two
# have to be read together.
#
#   build_probe.sh late  <outdir>   late_midi.dll   - publishes a midi slot at
#                                   attach and reports what winmm answers
#   build_probe.sh exe   <outdir>   probe_exe.exe   - links winmm
#   build_probe.sh noexe <outdir>   probe_noexe.exe - does not link winmm
#
# The two executables are the same source and differ only in whether winmm is
# among their imports, which is the variable under test: a program that imports
# winmm has it loaded before its own DllMain finishes, one that does not pulls
# it in at the first call instead.
#
# -municode is not used: these have no wmain and utf8 argv would be a
# difference between the two builds for no reason. LTO and stripping are off so
# that a fault address can be read against a disassembly.
set -euo pipefail

CC=${CC:-gcc}
mode=${1:?mode: late|exe|noexe}
outdir=${2:?output directory}

# Resolved from this script's own location, not from the caller's directory.
# The step runs bash from the repository root, so a bare "late_midi.c" is asked
# to be a file in the root, and gcc says so: cc1.exe: fatal error: late_midi.c:
# No such file or directory (measured, run 36585143426). Same class of mistake
# as the driver's own -o handling one file over, and it is fixed the same way.
here=$(cd -- "$(dirname -- "$0")" && pwd)

# -static: every one of these is a mingw image, and a mingw image that is not
# static links libwinpthread-1.dll, libgcc_s_seh-1.dll and libstdc++-6.dll by
# name. Those sit in the same directory as the cross compiler on the runner,
# which is not on the dll search path, so a probe that is not static fails to
# start at all - and a process that never starts writes nothing anywhere. That
# is one of the two ways the matrix step can print no probe output and still
# look like it ran (measured: run 36585811967, four binaries built, zero
# LATE_PROBE lines and zero probe_* lines). Static linking removes the
# question rather than answering it.
FLAGS="-O1 -g -fno-omit-frame-pointer -DUNICODE -D_UNICODE -static"
LIBS="-lwinmm -luser32 -lgdi32"

mkdir -p "$outdir"

case "$mode" in
  late)
    # A dll, so it can stand in for python's winmm, and an EXE-style name for
    # the second copy. Both are the same file.
    "$CC" $FLAGS -shared -o "$outdir/late_midi.dll" "$here/late_midi.c" $LIBS -Wl,--out-implib,"$outdir/liblate_midi.dll.a" -Wl,--export-all-symbols
    cp "$outdir/late_midi.dll" "$outdir/late_midi_as_exe.dll"
    ;;
  exe)
    # -Wl,--out-implib keeps the export address table. The other two probes get
    # one through -Wl,--out-implib on the late build; this one did not, and
    # that absence is what made the two executables unable to report on
    # themselves. Measured on run 36594797122: both wrote
    #   ATTACH dllmain=0 done=0 env=ok dll=(unset)
    # which cannot all be true of a single copy of the statics - env=ok says
    # GetEnvironmentVariableA ran and returned a character count, the write to
    # g_path sits in the same block under the same condition, and main still
    # reads it empty. With the export table present the executable's DllMain is
    # reached through the import descriptor instead, which is the form the
    # loader resolves inside the image, and the statics are then shared.
    "$CC" $FLAGS -o "$outdir/probe_exe.exe" "$here/probe_exe.c" $LIBS \
      -Wl,--out-implib,"$outdir/libprobe_exe.a"
    ;;
  noexe)
    # No -lwinmm on the link line, so no import table entry names winmm, and
    # PROBE_MANUAL_WINMM makes the source load it with LoadLibrary and call it
    # through pointers instead. The loader then pulls winmm in at the first
    # call, which is after our dll has attached - the variable under test.
    #
    # Two earlier attempts are recorded here because both failed for different
    # reasons and the second one looks like it should have worked:
    #   -Wl,--disable-auto-import-winmm is not a flag ld has. It answered
    #   "unrecognized option" (run 36585436551); no per-dll form exists, the
    #   granular equivalent is __declspec(dllimport) on the symbols themselves.
    #   Dropping -lwinmm alone does not leave the calls resolvable either: the
    #   linker reported "undefined reference to `__imp_midiOutOpen`" for all
    #   four (run 36585591000). mingw's default libraries do not carry winmm.
    "$CC" $FLAGS -DPROBE_MANUAL_WINMM -o "$outdir/probe_noexe.exe" "$here/probe_exe.c" -luser32 -lgdi32
    ;;
  *)
    echo "unknown mode: $mode" >&2; exit 2 ;;
esac

ls -l "$outdir"

# What each binary actually imports. -static is there because a probe that
# cannot start is indistinguishable from a probe that ran and wrote nothing,
# and the import table is the whole reason: dumpbin is not installed on the
# runner but llvm-objdump usually is, and grep for a bare dll name finds it in
# the import table wherever the section sits.
if command -v llvm-objdump >/dev/null 2>&1; then
  for f in "$outdir"/*.exe "$outdir"/*.dll; do
    [ -e "$f" ] || continue
    echo "--- imports of $(basename "$f") ---"
    llvm-objdump -p "$f" 2>/dev/null | grep -i "DLL Name" || echo "(none listed)"
  done
fi
