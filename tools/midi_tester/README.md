# nvmidi-tester

A fake MIDI keyboard for developers who do not have one. It opens a virtual
MIDI output called **nvmidi virtual** and sends what you type to it, so a game
that listens to MIDI input can be tested without hardware.

Download a binary from the `midi-tester` workflow (Actions, Artifacts) or from
a `tester-*` release.

## Windows

Download `nvmidi-tester-windows.zip` and unpack it. Keep `nvmidi-tester.exe`
and `Windows.Devices.Midi2.dll` in the same folder, then run the exe.

- **Windows 11 25H2 or newer:** nothing to install. The exe creates the
  virtual port `nvmidi virtual` through Windows MIDI Services and prints the
  name the game will list as a MIDI input. The port exists while the exe runs.
- **Anything older:** Windows has no built-in virtual ports. The exe says so
  and asks you to download [loopMIDI](https://www.tobias-erichsen.de/software/loopmidi.html).
  Install it, create a port named `nvmidi virtual`, run the exe again. It sends
  into that port and the game reads the same port as an input.
- `--loopmidi` skips the built-in way and goes straight to loopMIDI.
- The built-in way sends channel messages only (notes, controllers, pedals,
  program change, pitch bend, pressure). SysEx and system messages need loopMIDI.

Linux (ALSA) and macOS (CoreMIDI) create the port themselves.

## Use

    nvmidi-tester          Enter plays a C E G chord for half a second
    nvmidi-tester -a       stdin is one JSON message per line
    nvmidi-tester -l       list output ports
    nvmidi-tester -p NAME  use the existing port whose name contains NAME
    nvmidi-tester -n NAME  name of the virtual port (default nvmidi virtual)
    nvmidi-tester -c 3     channel 1..16, default 1

Without `-a`, type `help` for the commands: `sustain on`, `sustain off`,
`soft on`, `sostenuto on`, `cc 1 64`, `note C4 90 300`, `on C4`, `off C4`,
`chord C4 E4 G4`, `pc 5`, `bend 4000`, `channel 2`, `velocity 80`, `panic`,
`quit`. A bare note name such as `D#3` plays that note. Everything it prints
is a plain line of text.

## Automation mode (`-a`)

One JSON object per line (or an array of objects). Channels are 1..16 and
middle C is `C4` = 60, the same as the nvmidi plugin. Errors go to stderr, the
program keeps running.

    {"type":"note_on","note":"C4","velocity":100,"duration":500}
    {"type":"note_off","note":60}
    {"type":"chord","notes":["C4","E4","G4"],"duration":500}
    {"type":"sustain","value":"on"}
    {"type":"soft","value":"off"}
    {"type":"cc","control":67,"value":127}
    {"type":"program_change","program":5}
    {"type":"pitch_bend","value":-2000}
    {"type":"aftertouch","note":"C4","pressure":60}
    {"type":"channel_pressure","pressure":60}
    {"type":"panic"}
    {"bytes":[144,60,100]}

`duration` (milliseconds) on a `note_on` or `chord` sends the note off for you.
Other pedal types: `sostenuto`, `damper`, `modulation`, `volume`, `expression`,
`pan`. Their `value` can be `on`, `off`, `true`, `false` or a number.

On exit every pending note is released, pedals go up and all-notes-off is sent
on all 16 channels, so nothing is left ringing in the game.

## Build

    cd tools/midi_tester && make        # Linux, macOS, or Windows with MinGW

Linux needs `libasound2-dev`. The MinGW build on Windows has no built-in virtual
port, only the loopMIDI way. The full Windows build is the MSVC one in
`.github/workflows/midi-tester.yml`, which needs the Windows.Devices.Midi2
package from the microsoft/MIDI releases page.
