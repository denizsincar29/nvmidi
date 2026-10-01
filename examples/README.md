# nvmidi examples

These are demonstrations, not tests. Each one opens a window, speaks what it
does through the screen reader as well as printing it, and stops on Alt+F4.

All of them read `midi_config.txt` from this folder, so a different keyboard
only needs that file edited.

    match = nord      # part of the port name, case insensitive
    port = 0          # index to fall back on when the name finds nothing

- **list_ports.nvgt** — prints every port and says which one the config picks.
  Run this first when something is not working.
- **echo_monitor.nvgt** — speaks every message the keyboard sends, and transposes
  it up an octave on the way back out. The quickest way to confirm input and
  output both work end to end.
- **keyboard_to_synth.nvgt** — forwards the keyboard to another port,
  transposed. Shows input and output at once.
- **play_chord.nvgt** — plays chords and patterns on the keyboard's own sound
  engine. Needs only one port: the notes go out of the port they came from.
- **music_quickstart.nvgt** — the music API in the fewest lines. Plays in the
  background through `nvmidi_ui.nvgt` while it keeps printing, and stops on
  Alt+F4.

`nvmidi_ui.nvgt` is the shared helper: `speak`, `pump` and background playback.
`#include` it, or copy the one function you need.

The four smoke tests live in `../tests/`, not here.
