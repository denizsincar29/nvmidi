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

FLAGS="-O1 -g -fno-omit-frame-pointer -DUNICODE -D_UNICODE"
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
    "$CC" $FLAGS -o "$outdir/probe_exe.exe" "$here/probe_exe.c" $LIBS
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
