# winmm loopback driver

A MIDI output device that hands every message it receives to its own input
side, so one process can send and another can receive without a cable and
without installing anything.

## Why

The windows e2e job needs to observe a message leaving the plugin's process.
winmm has no virtual ports, and both vendor loopback drivers (LoopBe1,
loopMIDI) are Inno setups that need an interactive desktop the hosted runner
does not have — measured twice in this job's own log. A user mode driver is
never installed, so it has none of those problems.

## What it is

A user mode winmm driver: a DLL with a `DriverProc` entry point and a
`DRIVERENTRY` table naming its devices, loaded by the process that wants it and
called by winmm. No kernel code, no signing, nothing written into the system
directory. Windows has supported this since 3.x; the `.drv` files in
`system32` are the same shape.

Two devices, in this order:

    0  nvmidi loopback   output   the plugin opens this
    1  nvmidi loopback   input    the listener opens this

Messages go into a ring when written, and the input side copies them out into
whatever `midiInAddBuffer` armed. Same process, so the test's listener and the
plugin have to run in one process — the ci job runs `nvgt` itself as the
listener side rather than as a second program, which is what it already does.

## Build

    bash build.sh

Needs `x86_64-w64-mingw32-gcc` (mingw-w64), which the windows runner has.

## Wire into a machine by hand

Copy `nvmidi-loopback.exe` anywhere, then in an elevated shell:

    reg add "HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Drivers32" /v midi8 /t REG_SZ /d "C:\path\nvmidi-loopback.exe" /f

The entry is a `Drivers32` value whose name is `midiN` for the first free N;
windows already fills `midi1`..`midi3` and often more, so check what is there
before picking one. `midi8` is a guess that the ci job verifies by looking for
the device in the port list afterwards.
