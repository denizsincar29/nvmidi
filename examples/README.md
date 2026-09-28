# nvmidi examples

All of them read `midi_config.txt` from this folder, so a different keyboard
only needs that file edited.

    match = nord      # part of the port name, case insensitive
    port = 0          # index to fall back on when the name finds nothing

- **list_ports.nvgt** — prints every port and says which one the config picks.
  Run this first when something is not working.
- **echo_monitor.nvgt** — speaks every message the keyboard sends. The quickest
  way to confirm input works end to end.
- **keyboard_to_synth.nvgt** — forwards the keyboard to another port,
  transposed. Shows input and output at once.
- **play_chord.nvgt** — plays chords and patterns on the keyboard's own sound
  engine. Needs only one port: the notes go out of the port they came from.

Press escape to stop any of them.
