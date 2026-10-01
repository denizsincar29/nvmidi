# nvmidi

MIDI input and output for [NVGT](https://nvgt.dev) — the NonVisual Gaming
Toolkit.

Read a MIDI keyboard, pad controller or wind controller from an NVGT script;
send MIDI to a synthesiser, a DAW or a virtual port. It is a thin binding over
[RtMidi](https://github.com/thestk/rtmidi), which supplies the platform layer —
WinMM on Windows, ALSA on Linux.

## Installing

Download `scripts/get_nvmidi.nvgt` from this repository, then run it with the
NVGT engine:

    nvgt get_nvmidi.nvgt

That fetches the newest release and puts the library in the `lib/` folder of
the engine you are running, which is where NVGT looks for plugins. Nothing else
needs downloading — the script uses NVGT's own http, file and screen reader
functions.

Then ask for the plugin from the script that needs it, on a line of its own at
the top of the file:

```angelscript
#pragma plugin nvmidi
```

The pragma is the whole of the installation as far as your script is concerned:
NVGT reads a script's pragmas before compiling it and loads exactly the names it
finds there. Without that line every `midi_*` name fails to compile with
`No matching symbol`, which reads like a broken library rather than a missing
request.

## Reading a keyboard

```angelscript
#pragma plugin nvmidi
#include "nvmidi_string.nvgt"

midi_input@ in = midi_input_create();

void main() {
	// Say what is plugged in, so you have a port number to use.
	for (uint i = 0; i < midi_input_port_count(); i++)
		screen_reader_speak(midi_input_port_name_text(i));

	if (!in.open(0)) {
		screen_reader_speak("could not open the keyboard: " + midi_last_error_text());
		return;
	}

	while (true) {
		midi_message m;
		while (in.next_message(m)) {
			if (m.is_note_on)
				screen_reader_speak(midi_note_name(m) + " velocity " + m.data2);
		}
		wait(10); // never spin without sleeping
	}
}
```

That is the high level reading of a message: `m.is_note_on`, `m.is_note_off`,
`m.is_control_change`, `m.controller`, `m.is_pedal`, `m.pedal_type`,
`m.pitch_bend`, `m.program`, `m.aftertouch` and `m.channel_pressure` are all
properties of the message, so a script never takes the status byte apart by
hand. `m.is_note_on` is false for a note-on that arrived with velocity 0 —
that spelling means the note was released, and `m.is_note_off` is true for
both spellings of a release.

Drain the queue in a loop until `next_message` returns false, rather than
reading one message per frame — a fast player generates messages quicker than
60 a second.

## Playing a chord

`midi_note` carries a pitch, a velocity and a length, and the length can be
written in whichever unit you think in. `nvmidi_constants.nvgt` names the
pitches, so the same code reads as notes rather than as numbers:

```angelscript
#include "nvmidi_constants.nvgt"

array<midi_note@>@ notes = array<midi_note@>();
midi_note@ n = nvmidi_note_create(NOTE_C4, 100);   // middle C, velocity 100
n.length = midi_duration(1.0, MIDI_BEATS);         // one beat at 120 bpm
notes.insert_last(n);
notes.insert_last(nvmidi_note_create(NOTE_E4, 100));
notes.insert_last(nvmidi_note_create(NOTE_G4, 100));

out.play_chord_wait(notes);                        // sounds, then returns
out.play_midi_chord_wait(notes, "arpeggio");
```

`midi_note_number("E4")` does the same job from a name, for a script that would
rather read the name than a constant.

`play_midi_chord(notes, pattern)` lays a group out in time — `spread`,
`arpeggio`, `quick`, `fast`, `sequence`, `repeat`, `strum` or `chord`. The same
functions exist on `midi_input`, where the notes go back out of the port they
came from and the keyboard's own sound engine makes the sound, so a keyboard
with no software synthesiser beside it can still be played.

## Strings arrive as bytes

Text coming **out** of the plugin never arrives as a string. Every one of those
surfaces is a pair — a byte count and an indexed byte — because the plugin and
the engine are built by different compilers and a C++ string is a different
shape in each of them; one side handing the other a string reads the wrong
fields and takes the process down. A count and a byte at a time crosses safely
because both sides agree on what an `int` is.

`nvmidi_string.nvgt` puts the text back together, so a script asks for a name
and gets a string:

```angelscript
#include "nvmidi_string.nvgt"

screen_reader_speak(midi_input_port_name_text(0));
screen_reader_speak(midi_last_error_text());
screen_reader_speak(midi_note_name(m));
```

`-1` from the indexed call means "no byte there", which is not the same as a
zero byte; that is how the end of the text is found. The bytes carry no
encoding — a port name with accents in it loses them, because the plugin
reports MIDI bytes rather than characters in a codepage.

Text going **in** is fine: `midi_find_input_port("nord")`,
`midi_note_number("C4")` and `open_by_name(substring)` all take ordinary
strings, because the engine builds a string and hands it *to* the plugin,
which is the direction that works.

Why it is this way, and what the alternative costs, is in
[TECHNICAL.md](TECHNICAL.md).

## Where the rest is

- **[TECHNICAL.md](TECHNICAL.md)** — building the library, shipping a compiled
  game, the full API surface, threading, the examples and the smoke tests.
  Read this one if you are changing the plugin or packaging it with a game.
- **[doc/API.md](doc/API.md)** — the reference: every type, every method, the
  error model.
- **`scripts/get_nvmidi.nvgt`** — the installer. It also answers
  `nvgt get_nvmidi.nvgt --check`, which says whether the library you have is
  the newest release without changing anything, and writes `last download.txt`
  beside the library it installs so a folder can always say which release it
  holds.

## What is not supported

No MIDI file parsing or playback. No virtual port creation on Windows —
`is_virtual_port` and `set_virtual_port` are accepted but only do anything on
platforms RtMidi can support. Timestamps come from RtMidi and count seconds
from the moment the port was opened, not wall-clock time.

## Credits

Backed by RtMidi, copyright Gary P. Scavone. NVGT is copyright Sam Tupy. See
[license.md](license.md).
