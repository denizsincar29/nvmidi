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

A script then asks for the plugin with one include:

```angelscript
#include "midi.nvgt"
```

`midi.nvgt` carries the `#pragma plugin nvmidi` line itself and pulls in the
constants and the string helpers, so that one line is the whole of the setup.
You never write the pragma and never import the plugin by name. The compiler
resolves the include relative to the file that carries it, so `midi.nvgt` has
to be reachable from your script — keep it beside the script, or beside the
`lib/` folder the installer wrote.

Underneath, the pragma is the installation: NVGT reads a script's pragmas
before compiling it and loads exactly the names it finds there. Without it
every `midi_*` name fails to compile with `No matching symbol`, which reads
like a broken library rather than a missing request. That is why the include is
not optional even though it looks like one more line.

## The wrapper

`midi.nvgt` is a thin script layer over the plugin. It changes nothing about
what the plugin does; it removes the two things every script was otherwise
writing for itself.

It wraps the awkward types in classes, so a function that hands back a message
hands back something a script can read:

```angelscript
midi_message_view v(m);
if (v.is_note_on()) screen_reader_speak(v.note_name() + " velocity " + v.velocity());
```

The view answers the same questions the message does — `is_note()`,
`is_note_on()`, `is_note_off()`, `is_control_change()`, `is_pedal()`, `note()`,
`velocity()`, `controller()`, `pedal_type()`, `pitch_bend()`, `program()`,
`aftertouch()`, `channel_pressure()`, `note_name()`, `name()` — as calls rather
than as properties, because they are forwarding to the message and a script
class cannot turn a function into a property for its caller.

It does the same for text, so a name is a string rather than a byte count and
an index:

```angelscript
screen_reader_speak(midi_input_port_name_text(0));
screen_reader_speak(midi_last_error_text());
screen_reader_speak(midi_message_text(m));
```

And it lets a note be named rather than numbered, on both the input and the
output side:

```angelscript
midi_output_play_note(out, "E4", 100);
midi_output_play_chord(out, "C4 E4 G4".split(" "), 100);
midi_input_play_note(in, "C5", 100);   // back out of the keyboard's own port
```

Those are free functions rather than methods of the port, because AngelScript
cannot add methods to a class the plugin has already registered — the port
classes belong to the plugin, and a script cannot extend them. The chord
functions take the same `array<midi_note@>` the plugin does; for a named chord
that means splitting a string into an array first, as above.

Everything the plugin documented before still works — this is a wrapper, not a
replacement — but a new script should not need to touch the byte tables at all.

## Reading a keyboard

```angelscript
#include "midi.nvgt"

midi_input@ in = midi_input_create();
midi_message m;

void main() {
	// Say what is plugged in, so you have a port number to use.
	for (uint i = 0; i < midi_input_port_count(); i++)
		screen_reader_speak(midi_input_port_name_text(i));

	if (!in.open(0)) {
		screen_reader_speak("could not open the keyboard: " + midi_last_error_text());
		return;
	}

	while (true) {
		while (in.next_message(m)) {
			midi_message_view v(m);
			if (v.is_note_on)
				screen_reader_speak(v.note_name() + " velocity " + v.velocity());
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
written in whichever unit you think in. The wrapper reaches the pitches by name,
so the same code reads as notes rather than as numbers:

```angelscript
#include "midi.nvgt"

array<midi_note@>@ notes = array<midi_note@>();
midi_note@ n = midi_note_named("C4", 100);   // middle C, velocity 100
n.length = midi_duration(1.0, MIDI_BEATS);   // one beat at 120 bpm
notes.insert_last(n);
notes.insert_last(midi_note_named("E4", 100));
notes.insert_last(midi_note_named("G4", 100));

out.play_chord_wait(notes);                  // sounds, then returns
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

## Strings arrive as bytes

Text coming **out** of the plugin never arrives as a string. Every one of those
surfaces is a pair — a byte count and an indexed byte — because the plugin and
the engine are built by different compilers and a C++ string is a different
shape in each of them; one side handing the other a string reads the wrong
fields and takes the process down. A count and a byte at a time crosses safely
because both sides agree on what an `int` is.

`midi.nvgt` puts the text back together, so a script asks for a name and gets
a string. The functions are in `nvmidi_string.nvgt`, which `midi.nvgt` includes
for you — you never include it yourself:

```angelscript
#include "midi.nvgt"

screen_reader_speak(midi_input_port_name_text(0));
screen_reader_speak(midi_last_error_text());
screen_reader_speak(midi_message_text(m));       // off the message itself
screen_reader_speak(midi_note_pitch_text(64));   // "E4", from a pitch
```

Two spellings exist for the port name, and the difference is which question you
are asking. `midi_input_port_name_text(0)` answers for port number 0, whether
or not anything is open. `midi_input_name_text(in)` answers for the object you
are holding — the port it actually opened — which is what a script that just
failed to open something already knows.

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
