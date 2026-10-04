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

For the file to be found at all it has to sit where the script was started
from, not where the script lives. That is the one thing that decides whether
your `in_port` and `out_port` are honoured or quietly skipped, and the plugin
cannot check it: it is handed a name, and a missing name is not an error. Run
`list_ports.nvgt` when the ports come up wrong on every example at once.

`sound_probe.nvgt` answers the other half, the one with no file in it: nine
seconds, four tones, a second of silence between each, a number spoken before
every one. When some examples sound and others do not, and the config is
already known good, this is what separates the plugin's own clock from the
port.

- **list_ports.nvgt** — pick the input and the output port with the arrow keys
  and write them to `midi_config.json`. Run this first.
- **echo_monitor.nvgt** — speak every message the device on the input port
  sends. It only listens; octave_up_forwarder.nvgt is the one that sends.
- **octave_up_forwarder.nvgt** — forward the keyboard to another port, an
  octave up.
  Input and output at once. The is_* form, no switch.
- **player.nvgt** — the two ways to sound a note: the background player, and
  sending the messages by hand.
- **play_chord.nvgt** — one chord, then every pattern the player knows: spread,
  arpeggio, quick, fast, strum, repeat.
- **music_quickstart.nvgt** — the high level half: durations, patterns, a
  sequence, all through `midi.nvgt` and nothing else.
- **sound_probe.nvgt** — is it the plugin's clock or the port? Four tones with
  a spoken count, a second of silence between each. A script can count on it
  from `sound_probe_count()`, so it asserts instead of just being listened to.

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
	if (m.is_note_off) output.send_note_off(m.channel, m.data1 + 12, 0);
	else output.send_note_on(m.channel, m.data1 + 12, m.data2);
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
decides one thing. `TECHNICAL.md` tells the whole story of the status byte.

No example has to pick one and stay there. `echo_monitor.nvgt` does both in one
file — the switch says the message in words, the `is_*` test decides whether it
is a pedal. `octave_up_forwarder.nvgt` is the other extreme: a switch would sit where
nothing is being decided, so it asks questions instead.

## Where the details live

These files stay small on purpose. The long version of the format — what the
status byte is, why a note on at velocity 0 is a release, how the waiting
works — is in `TECHNICAL.md`; every property and method is in `doc/API.md`.

## The chord waits that played at machine speed

Reported 1 October, fixed 4 October. `play_chord_wait` and
`play_midi_chord_wait` returned at once instead of holding the notes for their
length: the phrase was spoken and every note went on and off inside the same
millisecond.

The cause was in `wait_until`, which resolves the script's own `void wait(int)`
through `GetModule(0)`. The engine does not promise that the module being run is
the first one it holds, and a few commits back — around the enumeration-object
retraction in `dd73e1d` — it stopped being so. The lookup missed, the function
handle stayed null, and the loop took its silent way out before waiting for
anything: no error, no note of it, everything at once.

`wait_until` now searches every module the engine knows, by declaration, with
`GetModule(0)` kept as the fast path it usually is. When the function is not
found at all, it says so on stderr rather than playing the phrase at speed:
that silence is what made the failure invisible in the first place, and it is
worth more than the few lines it costs.

Worth knowing when writing your own waits: `play_until`, `play_pattern` and
friends work the same way — they call the script's `wait`, so a script that
overrides it keeps control of the clock.
