# nvmidi examples

These are demonstrations, not tests. Each one opens a window, speaks what it
does through the screen reader as well as printing it, and stops on Alt+F4.

Every one of them starts with the same two lines, and a script you write should
too:

```angelscript
#include "midi.nvgt"        // the plugin, the constants and the string helpers
#include "nvmidi_ui.nvgt"   // the window helpers - only needed if you want a window
```

`midi.nvgt` is the wrapper: it carries `#pragma plugin nvmidi` itself, so none
of these files names the plugin or builds a string out of bytes by hand. The
window helpers are a separate include on purpose — they are about a window on a
screen, not about MIDI, and a script that only sends notes does not want them.

All of them read `midi_config.json` from this folder, so a different keyboard
only needs that file edited.

    { "match": "nord", "in_port": 0, "out_port": 0 }

`match` is part of the port name, matched case-insensitively; `in_port` and
`out_port` are the indexes to fall back on when the name finds nothing. The
plugin's own reader takes either shape - a file whose first character is `{`
is read as JSON, anything else as the old `key = value` text - so a config
written by hand in the old form still works.

- **list_ports.nvgt** — prints every port and says which one the config picks.
  Run this first when something is not working.
- **echo_monitor.nvgt** — speaks every message the keyboard sends. The quickest
  way to confirm input works end to end. It listens only; octave-doubler.nvgt
  is the one that sends.
- **octave-doubler.nvgt** — forwards the keyboard to another port,
  transposed. Shows input and output at once.
- **play_chord.nvgt** — plays chords and patterns on the keyboard's own sound
  engine, and stays open so the patterns can be compared one after another.
  Needs only one port: the notes go out of the port they came from.
- **music_quickstart.nvgt** — the music API in the fewest lines. Plays in the
  background through `nvmidi_ui.nvgt` while it keeps printing, and stops on
  Alt+F4.

## Known bug: play_chord.nvgt plays at machine speed

Status 1 October, unresolved. Reported by the owner, reproduced on his Windows
build three times.

What it does: the window opens and closes almost at once - the screen reader
does not get the title read out - and every chord and pattern goes by instantly.
Nothing waits. Every other example behaves.

This one file is the only example that lets the plugin do the waiting, through
`play_chord_wait` and `play_midi_chord_wait`. `music_quickstart.nvgt` plays in
the background and counts its own time with `ticks()`, and it is fine.

What was measured, not guessed:

- `src/nvmidi.cpp:1520` - `play_chord_wait` is `play_chord` (which only turns
  the notes on), then `wait_until(now_ms() + group_length_at(...))`, then
  `stop_all_notes()`.
- `src/nvmidi.cpp:1341` - `wait_until` loops `wait()` in hops of at most 5 ms
  until `now_ms()` reaches the moment. One second of chord is about 200 turns
  of that loop.
- `src/nvmidi.cpp:343` - `now_ms()` on Windows is `GetTickCount64()`.

So a one second chord needs that loop to run about 200 times, each time calling
the engine's `wait` through a prepared script context. The symptom - the whole
run over in the time it takes a window to appear - means the loop is not
spending a second there. Whether `wait` returns without delaying, or the nested
context executed from the plugin does not block its caller, is NOT measured.

A one line probe was sent to the owner and did not answer it: the window closed
before the line was spoken, which fits the same symptom (the probe speaks after
`show_window`, and `wait(50)` on the next line apparently does not hold the
window either).

The fix has to remove the plugin's dependence on `wait` for the wait itself -
the shape `nvmidi_ui.nvgt::ui_play_background` already uses: drive `ticks()`
from the script and let the caller's own loop be the clock. Not attempted yet;
the owner asked for the bug to be recorded rather than worked on.

`nvmidi_ui.nvgt` is the shared helper: `speak`, `pump` and background playback.
`#include` it, or copy the one function you need.

The four smoke tests live in `../tests/`, not here, and they are on the wrapper
too — a test that reads the plugin directly would stop proving that the wrapper
still compiles, so the wrapper is what they exercise first.
