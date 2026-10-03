# nvmidi

MIDI input and output for [NVGT](https://nvgt.dev) — the NonVisual Gaming
Toolkit.

Read a MIDI keyboard, pad controller or wind controller from an NVGT script;
send MIDI to a synthesiser, a DAW or a virtual port.

## Install

Clone the repository:

    git clone https://github.com/denizsincar29/nvmidi.git

Install the plugin for the NVGT engine you are using:

    nvgt nvmidi/scripts/get_nvmidi.nvgt

Then copy `midi.nvgt` from the repository into your project's folder and
include it:

```angelscript
#include "midi.nvgt"
```

That is the whole of the setup. Run the installer again whenever you want a
newer release. If you would rather build the plugin yourself, see
[TECHNICAL.md](TECHNICAL.md).

If you see

    No matching symbol

then the plugin is not installed — the installer step did not run, or it
landed in a different engine's folder. Run the installer again.

## Reading a keyboard

```angelscript
#include "midi.nvgt"

midi_input@ port = midi_input_create();
midi_message m;

void main() {
	// Say what is plugged in, so you have a port number to use.
	for (uint i = 0; i < midi_input_port_count(); i++)
		screen_reader_speak(midi_input_port_name_text(i));

	if (!port.open(0, "keyboard")) {
		screen_reader_speak("could not open the keyboard: " + midi_last_error_text());
		return;
	}

	while (true) {
		while (port.next_message(m)) {
			midi_message_view v(m);
			if (v.is_note_on())
				screen_reader_speak(v.note_name() + " velocity " + v.velocity());
		}
		wait(10); // never spin without sleeping
	}
}
```

`m.is_note_on`, `m.is_note_off`, `m.is_control_change`, `m.controller`,
`m.is_pedal`, `m.pedal_type`, `m.pitch_bend`, `m.program`, `m.aftertouch` and
`m.channel_pressure` are properties of the message, so a script never takes the
status byte apart by hand. `m.is_note_on` is false for a note-on that arrived
with velocity 0 — that spelling means the note was released — and
`m.is_note_off` is true for both spellings of a release.

Drain the queue in a loop until `next_message` returns false, rather than
reading one message per frame — a fast player generates messages quicker than
60 a second.

## Playing notes and chords

```angelscript
#include "midi.nvgt"

midi_output@ out = midi_output_create();
out.open(0, "synth");

midi_output_play_note(out, "E4", 100);              // sounds, then moves on

midi_note@ long_e = midi_note_named("E4", 100);      // sounds for one beat,
long_e.length = out.duration(1.0, MIDI_BEATS); // then returns
out.play_note_wait(long_e);

array<midi_note@>@ notes = array<midi_note@>();
notes.insert_last(midi_note_named("C4", 100));
notes.insert_last(midi_note_named("E4", 100));
notes.insert_last(midi_note_named("G4", 100));

out.play_chord_wait(notes);                   // sounds, then returns
out.play_midi_chord_wait(notes, "arpeggio");
```

`midi_note_number("E4")` does the same job from a name, for a script that would
rather read the name than a constant, and `midi_note_pitch_text(64)` goes the
other way and gives back "E4" for a number.

`play_midi_chord(notes, pattern)` lays a group out in time — `spread`,
`arpeggio`, `quick`, `fast`, `sequence`, `repeat`, `strum` or `chord`. The same
functions exist on `midi_input`, where the notes go back out of the port they
came from and the keyboard's own sound engine makes the sound, so a keyboard
with no software synthesiser beside it can still be played.

## Examples

The `examples/` folder holds short, runnable scripts — `echo_monitor.nvgt`,
`play_chord.nvgt`, `list_ports.nvgt` and others. Read them in order and you
have the whole API.

## Where the rest is

- **[doc/API.md](doc/API.md)** — the reference: every type, every method, the
  error model.
- **[TECHNICAL.md](TECHNICAL.md)** — building the plugin, shipping a compiled
  game, the full API surface, threading, and why text crosses the plugin
  boundary the way it does. Read this one if you are changing the plugin or
  packaging it with a game.
- **`scripts/get_nvmidi.nvgt`** — the installer. It also answers
  `nvgt scripts/get_nvmidi.nvgt --check`, which says whether the plugin you
  have is the newest release without changing anything.

## What is not supported

No MIDI file parsing or playback. No virtual port creation on Windows —
`is_virtual_port` and `set_virtual_port` are accepted but only do anything on
platforms RtMidi can support. Timestamps come from RtMidi and count seconds
from the moment the port was opened, not wall-clock time.

## Credits

Backed by RtMidi, copyright Gary P. Scavone. NVGT is copyright Sam Tupy. See
[license.md](license.md).
