# nvmidi examples

Short scripts to copy from. Each one opens a window, prints what it does, speaks
it through the screen reader, and stops on escape.

Every one starts the same way:

```angelscript
#include "../midi.nvgt"
```

`midi.nvgt` is the wrapper: it carries `#pragma plugin nvmidi` itself, so no
example names the plugin or takes a string apart byte by byte.

All of them read `midi_config.json` from the folder the engine runs in, so
pointing them at another keyboard means editing that file:

    { "match": "nord", "in_port": 0, "out_port": 0 }

`match` is part of the port name, matched case-insensitively; `in_port` and
`out_port` are the indexes to fall back on when the name finds nothing. A file
whose first character is `{` is read as JSON, anything else as the old
`key = value` text, so an old config still works.

- **list_ports.nvgt** — pick the input and the output port with the arrow keys
  and write them to `midi_config.json`. Run this first.
- **echo_monitor.nvgt** — speak every message the device on the input port
  sends. It only listens; octave-doubler.nvgt is the one that sends.
- **octave-doubler.nvgt** — forward the keyboard to another port, an octave up.
  Input and output at once.
- **play_chord.nvgt** — one chord, then every pattern the player knows: spread,
  arpeggio, quick, fast, strum, repeat.
- **music_quickstart.nvgt** — the high level half: durations, patterns, a
  sequence, all through `midi.nvgt` and nothing else.

## Two ways to ask what a message is

The status byte's top half is the kind of message, its bottom half the channel.
`MIDI_NOTE_ON` is the number `0x90` under a readable name - no script has to
write the hex.

A message's fields read as words too: `m.kind`, `m.note`, `m.velocity`,
`m.controller`, `m.program` and the rest. `midi_message_view` in `midi.nvgt`
adds the string ones (`v.note_name`, `v.name`) for a script that wants to print
them.

```angelscript
switch (m.kind) {
case MIDI_NOTE_ON:
	if (m.data2 > 0) output.send_note_on(m.channel, m.data1 + 12, m.data2);
	else output.send_note_off(m.channel, m.data1 + 12, 0); // velocity 0
	break;
case MIDI_NOTE_OFF:
	output.send_note_off(m.channel, m.data1 + 12, m.data2);
	break;
}
```

`m.kind` carries no channel; `m.channel` says which.

The other spelling is one question per message — `m.is_note_on`, `m.is_pedal`,
`m.is_control_change`, all in `doc/API.md`. Reach for a switch when one message
goes to one place (a router, a logger); reach for `is_*` when one question
decides one thing.

`echo_monitor.nvgt` shows both in one file. `octave-doubler.nvgt` is the pure
switch. The other three have no per-message branching, so a switch there would
sit where nothing is being decided.

## Known bug: the chord waits play at machine speed

Status 1 October, unresolved. Reported by the owner, reproduced on his Windows
build three times: `play_chord_wait` and `play_midi_chord_wait` return at once
instead of holding the notes for their length.

What was measured, not guessed:

- `src/nvmidi.cpp:1520` — `play_chord_wait` turns the notes on, calls
  `wait_until(now_ms() + group_length_at(...))`, then releases them.
- `src/nvmidi.cpp:1731` — `wait_until` loops the engine's `wait()` in hops of at
  most 5 ms until `now_ms()` reaches the moment. It gives up silently, with no
  error, if `GetModule(0)` does not hand it the calling script's module, and the
  module lookup is the one part of the wait that has never been checked.
- `src/nvmidi.cpp:717` — `now_ms()` is `GetTickCount64()` on Windows and a
  monotonic clock elsewhere, so the clock itself is not at fault.

The fix has to stop leaning on the engine's `wait` for the wait: drive the
clock from the script and let the caller's own loop be the time, which is what
`tests/` and the background player already do.
