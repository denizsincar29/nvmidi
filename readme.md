# nvmidi

MIDI input and output for [NVGT](https://nvgt.dev) — the NonVisual Gaming
Toolkit.

This plugin lets an NVGT script talk to MIDI hardware and software: read a
MIDI keyboard, pad controller or wind controller, and send MIDI to a
synthesiser, a DAW, or a virtual port. It is a thin binding over
[RtMidi](https://github.com/thestk/rtmidi), which supplies the platform layer
— WinMM on Windows, ALSA on Linux.

Everything is exposed to AngelScript under plain names: `midi_input`,
`midi_output`, `midi_message`, `midi_note`, `midi_duration`, `midi_config`,
and a handful of free functions. There is a thin layer that is just the
hardware, and above it a higher level one that plays chords and patterns
without the script building note-on/note-off pairs by hand.

## Building

The plugin is a single shared library, `nvmidi.so` on Linux and `nvmidi.dll`
on Windows.

**Linux**

```
make
```

Needs `g++` and the ALSA development headers (`sudo apt install
libasound2-dev`). The result is `nvmidi.so`, which links against
`libasound.so.2` at runtime.

**Windows**

Install MinGW-w64, then:

```
mingw32-make
```

The result is `nvmidi.dll`, linking against `winmm`.

`src/nvgt_plugin.h` is vendored from NVGT and pins the plugin API version
(currently 5). If NVGT bumps that version, copy the new header in from the
NVGT source tree before rebuilding.

## Installing

Put the built library in the `lib/` directory of your NVGT installation, and
ask for it by name from the script that needs it:

```angelscript
#pragma plugin nvmidi
```

The pragma is not decoration — it is the only thing that loads a plugin. The
engine reads the script's pragmas before it compiles the script, and loads
exactly the names it finds there (`CompileScript` and `PragmaCallback` in
`src/nvgt_angelscript.cpp`); a plugin that nothing asks for is never opened,
however carefully it is placed. Without the line every `midi_*` name fails to
compile with `No matching symbol`, which reads like a broken library rather
than a missing request.

Loading is per script: each `.nvgt` that uses these functions needs its own
`#pragma plugin nvmidi` line, at the top of the file.

The library itself is found by the operating system, because the engine asks
for it by bare name — `SDL_LoadObject("nvmidi.dll")`. On Windows NVGT sets
that search path to its own `lib/` folder explicitly (`SetDllDirectoryW` in
`src/nvgt.cpp`); on Linux nothing adds `lib/` to the search path unless the
engine was linked with an rpath for it, so a copy beside the engine binary is
the placement that always works. Either way it is the engine's install
directory, never the folder the script happens to sit in.

`scripts/get_nvmidi.nvgt` does the download and installation for you: it
fetches the newest release and installs it into `lib/` next to the running
NVGT.

## Shipping a compiled game

Two halves matter here, and NVGT's own manual ("Compiling your project for
distribution") states both.

The first: once you compile, the library is looked for next to the game's
executable — either directly in that folder or in a `lib` folder inside it.
The engine's own `lib/` is no longer involved, because a compiled game does
not use the engine's installation.

The second is the one that bites. NVGT's bundling step does **not** copy
plugin libraries into the bundle by default: `build.shared_library_excludes`
defaults to `"plist TrueAudioNext GPUUtilities systemd_notify sqlite git2
curl"`, so the shared libraries for plugins are excluded unless the author
says otherwise. A game built with default settings therefore ships without
`nvmidi.dll` and dies at startup with no explanation. If you bundle, override
that property in your project's configuration file so it no longer excludes
what you use — or copy the library beside the executable yourself.

### Saying why it failed

Honest answer first, because the obvious approach does not work.

`#pragma plugin nvmidi` is processed *before the script is compiled*. If the
library cannot be loaded, the engine stops there — `ERROR: failed to load
plugin` — and **nothing in your script runs**, neither `preglobals()` nor
`main()`. A script cannot report that its own plugin is missing, because the
plugin's absence is what stops the script.

Measured on the engine: with `#pragma plugin nvmidi` and no loadable library,
the run ends at the pragma even when the script declares a `midi_first_error()`
of its own to catch exactly that. A script *can* shadow a plugin's name when
the plugin is simply absent and no pragma asks for it — but that is not the
case you have.

What you can do instead:

**Check for the plugin without asking the engine for it at load time.** Leave
the pragma out of the main script, load the library yourself, and keep the
pragma inside a file that is only compiled once the library is known to be
there. That is more moving parts than most games need.

**Ship a plugin that cannot fail to load.** This is the practical one and it
is a property of the build, not the script — see *Building*: a plugin built
with `MIDI_BACKEND=dummy` needs nothing but libc and libstdc++, so the loader
always takes it, and the script learns "no MIDI here" from the API instead of
from a startup that never happens.

**Tell the player what to install.** A missing library at ship time is a
packaging mistake, and the fix is the `build.shared_library_excludes`
paragraph above rather than a runtime message.

`midi_first_error()` stays in the API for the case where the plugin *did*
load: it reports the first thing that went wrong, and is empty while nothing
has. NVGT's `preglobals()` is still the right hook for the checks it was
designed for — the manual's own example is `SOUND_AVAILABLE` and
`SCREEN_READER_AVAILABLE`, engine-side features that exist regardless of any
plugin.

## Reading a MIDI keyboard

The short version: create an input, open a port, then drain the queue once
per frame from your main loop.

```angelscript
midi_input@ in = midi_input_create();

void main() {
	// List what is plugged in, so you can pick a port number.
	for (uint i = 0; i < midi_input_port_count(); i++) {
		screen_reader_speak(midi_input_port_name(i));
	}
	if (!in.open(0)) { // first input port
		screen_reader_speak("could not open the keyboard: " + midi_last_error());
		return;
	}

	while (true) {
		midi_message@ m;
		while (in.next_message(m)) {
			// m is a midi_message; m.to_string() reads like
			// "note on, channel 1, note 60, velocity 100"
			if ((m.status & 0xf0) == 0x90 && m.data2 > 0) {
				screen_reader_speak("note " + m.data1);
			}
		}
		wait(10); // never spin without sleeping
	}
}
```

`next_message` takes messages off the queue in the order the hardware sent
them. Always drain it in a loop until it returns false, rather than reading
one message per frame — a fast player generates messages quicker than 60 a
second.

## Picking a port by name

Port numbers move around when a device is replugged or a virtual port is
installed, so a test program is better off looking for its keyboard by name.
The match ignores case, and a small text file next to the script says what to
look for — so someone testing another keyboard edits the file, not the code:

    match = nord          # part of the port name, case insensitive
    port = 0              # index to fall back on

```angelscript
midi_config@ config = midi_config_create();
config.load_if_present("midi_config.txt");

int port = config.find_input_port();
if (port < 0) {
	screen_reader_speak("nothing matched, and the fallback port is empty");
	return;
}
screen_reader_speak("Using " + config.describe()); // "Nord Piano 6"
in.open_config(config);
```

`describe()` names the port it picked and says when it fell back to the index,
which is what you want spoken rather than a bare number. The free functions
`midi_find_input_port("nord")` and `midi_find_output_port("nord")` do the same
search without a file.

## Playing chords

`midi_note` and `midi_duration` are what the high level layer speaks. A note
carries its pitch, velocity, channel and length, and the length is written in
whichever unit you think in:

```angelscript
array<midi_note@>@ notes = array<midi_note@>();
midi_note@ n = nvmidi_note_create(60, 100);          // middle C, velocity 100
n.length = midi_duration(1.0, MIDI_BEATS);  // one beat at the default 120 bpm
notes.insert_last(n);
notes.insert_last(nvmidi_note_create(midi_note_number("E4"), 100));
notes.insert_last(nvmidi_note_create(midi_note_number("G4"), 100));

out.play_chord_wait(notes);                 // sounds, then returns
out.play_midi_chord_wait(notes, "arpeggio");
```

`play_chord` sends the notes and returns at once, leaving the release to a
timer; `play_chord_wait` holds until the chord is over. Same split for
`play_note`. `play_midi_chord(notes, pattern)` lays a group out in time —
`spread`, `arpeggio`, `quick`, `fast`, `sequence`, `repeat`, `strum` or plain
`chord`.

### Tempo

A length written in beats or bars needs a tempo, and repeating it on every
note is noise. Each music class carries one, and `duration()` builds a length
already resolved against it:

```angelscript
in.tempo = 96.0;                                  // bpm, 120 by default
midi_note@ n = nvmidi_note_create(60, 100);
n.length = in.duration(1.0, MUSIC_BEATS);         // a beat at 96, not at 120
n.length = in.duration(0.5, MUSIC_BARS);          // half a bar at the same tempo
n.length = in.duration(96.0, MUSIC_TICKS);        // ticks ignore the tempo
n.length = in.duration(1.0, MUSIC_BEATS_120);     // explicit: always 120
```

Milliseconds ignore the tempo entirely. A `MUSIC_BEATS_90`, `_100`, `_120` or
`_140` constant carries its own tempo and beats whatever the class is set to,
which is what you want for a fixture that must sound the same on every run.

A keyboard with its own sound engine can be played without any synthesis on
this side: `midi_input` has the same playing functions and the same tempo, and
sends the notes back out of the port they came from.

```angelscript
in.open_config(config);
in.tempo = 96.0;
in.play_chord_wait(notes);   // the Nord makes the sound, not this program
in.play_midi_chord_wait(notes, "arpeggio");
```

## Sending MIDI

```angelscript
midi_output@ out = midi_output_create();
if (!out.open(0)) {
	screen_reader_speak(midi_last_error());
	return;
}
out.send_note_on(1, 60, 100);   // channel 1, middle C, velocity 100
wait(500);
out.send_note_off(1, 60);
```

`all_notes_off()` stops everything on all sixteen channels, which is worth
calling on shutdown so a panic does not leave a stuck note sounding.

## API

**Free functions**

- `uint midi_input_port_count()` / `uint midi_output_port_count()` — how many
  ports the machine has.
- `string midi_input_port_name(uint port)` / `midi_output_port_name(uint)` —
  the name of one port, or `""` if out of range.
- `string[]@ midi_input_port_names()` / `midi_output_port_names()` — every
  name at once.
- `string midi_api_name()` — which backend is in use, e.g. `"ALSA"` or
  `"Windows MultiMedia"`. Useful in a bug report.
- `string midi_last_error()` — why the last call failed; empty on success.
- `string midi_message_name(const midi_message&in m)` — human-readable
  description of a message.
- `midi_input@ midi_input_create()` / `midi_output@ midi_output_create()` —
  make an object. The plugin owns it, so you do not delete it yourself.
- `int midi_find_input_port(const string&in substring)` /
  `midi_find_output_port` — the first port whose name contains the substring,
  ignoring case; -1 when nothing matches.
- `int midi_note_number(const string&in name)` — `"C4"`, `"F#3"`, `"Bb5"` to a
  MIDI note number, -1 when the name is not understood. `midi_note_name(60)`
  goes the other way.
- `midi_config@ midi_config_create()`, `midi_duration midi_duration_create()` (by value),
  `midi_note@ nvmidi_note_create()` — factories.
- `MIDI_MS`, `MIDI_TICKS`, `MIDI_BEATS`, `MIDI_BARS` — the duration units.
  `MUSIC_MS`, `MUSIC_TICKS`, `MUSIC_BEATS`, `MUSIC_BARS` are the same four
  values under the name the music classes use, and `MUSIC_BEATS_90`,
  `MUSIC_BEATS_100`, `MUSIC_BEATS_120`, `MUSIC_BEATS_140` are beats at that
  tempo regardless of what the class is set to.

**midi_message**

Fields `status`, `data1`, `data2` (all `uint8`), `channel` (`int`, 1..16, or
0 for a message that carries no channel), and `timestamp` (`double`, seconds
since the port opened). `to_string()` gives the readable form.

**midi_config** — `bool load(const string&in path)` and
`bool load_if_present(path)` read a file of `key = value` lines (`#` comments,
keys `match` and `port`); `find_input_port()` / `find_output_port()` return the
index to open; `describe()` says in words which port that is; `get_last_port()`
reports where the search ended. The `match` and `port` members can be set from
the script instead of a file. With no file the defaults are `"nord"` and 0.

**midi_note** — fields `pitch`, `velocity`, `channel` and `length` (a
`midi_duration`). `duration_ms()` gives the length in milliseconds and
`to_string()` names the note, e.g. `"C4 (60), velocity 100, channel 1"`.

**midi_duration** — fields `amount`, `unit`, `tempo` and `ppq`; `to_ms()`
converts once, using 120 bpm and 96 ppq unless the object says otherwise. A
beat is a quarter note and a bar is four of them. A duration made by `tempo`-
aware code (`n.duration()`) already has the class tempo written into it, so
`to_ms()` and a later playback agree.

**midi_input**

- `bool open(uint port, const string&in name = "nvmidi")`, plus
  `open_by_name(substring)` and `open_config(config)`. With any of them,
  `void close()`, `bool is_open()`, `int get_port()`,
  `string get_port_name()`.
- `bool next_message(midi_message&out)` — take the oldest queued message.
  Returns false when the queue is empty.
- `bool has_message()`, `uint get_pending()`, `void clear()`.
- `play_chord(notes)` / `play_chord_wait(notes)` / `play_note(note)` /
  `play_note_wait(note)` — play through the keyboard's own engine, since the
  notes go back out of the port they came from.
- `play_midi_chord(notes, pattern)` / `play_midi_chord_wait(notes, pattern)` /
  `play_sequence(notes)` — the same patterns `midi_output` offers, played on
  the keyboard rather than on a second device.
- `double tempo` — beats per minute, 120 by default, and
  `midi_duration duration(double amount, int unit)` — a length already
  resolved against that tempo.
- `uint stop_all_notes()`, `uint get_active_notes()`.
- `void set_ignore_sysex(bool)` / `bool get_ignore_sysex()` — default true.
  Sysex dumps are large and rarely useful in a game.
- `void set_ignore_timing(bool)` / `bool get_ignore_timing()` — default true.
  Drops clock and active-sensing bytes, which arrive continuously and are
  almost never what you want.

Changing either filter takes effect immediately on an open port.

**midi_output**

- `bool open(uint port, const string&in name = "nvmidi")`, `void close()`,
  `bool is_open()`, `int get_port()`, `string get_port_name()`.
- `bool send(uint status, uint data1, uint data2)` — raw bytes.
- `bool send_packed(uint packed)` — the three bytes packed low byte first.
- `send_note_on(channel, note, velocity)`,
  `send_note_off(channel, note, velocity = 0)`,
  `send_control_change(channel, controller, value)`,
  `send_program_change(channel, program)`,
  `send_pitch_bend(channel, value)`,
  `send_aftertouch(channel, note, pressure)`,
  `send_channel_pressure(channel, pressure)`,
  `send_sysex(const string&in data)`.
- `all_notes_off()`, `reset()`.
- Playing, the same four as `midi_input`, plus
  `play_midi_chord(notes, pattern)` / `play_midi_chord_wait(notes, pattern)`
  and `play_sequence(notes)`.
- `double tempo` and `midi_duration duration(double amount, int unit)` — the
  same pair `midi_input` has.
- `uint stop_all_notes()`, `uint get_active_notes()` — what the high level
  layer is currently holding sounding.

Channels are 1..16 everywhere. `send_pitch_bend` takes 0..16383 with 8192 as
the centre. The `send_note_*` and `send_control_change` helpers take a
channel and add the status byte for you; `send` and `send_packed` do not.

## Threading

MIDI input arrives on RtMidi's own thread, while your script runs on NVGT's.
The plugin bridges them with a lock-protected queue: the callback appends,
your script drains. That is why messages are read with `next_message` rather
than delivered to a script callback — calling into AngelScript from a foreign
thread is not safe.

The queue holds 4096 messages. If a script stops draining it, the oldest are
dropped rather than memory growing without bound.

## Examples

`examples/` holds four scripts, all of which read `midi_config.txt` from that
folder: `list_ports` (what is plugged in, and what the config picks),
`echo_monitor` (speaks every incoming message), `keyboard_to_synth` (forwards
one port to another, transposed) and `play_chord` (chords and patterns on the
keyboard's own sound engine).

## Installing the dll

`scripts/get_nvmidi.nvgt` fetches `nvmidi.dll` from this repository's releases
and installs it into the `lib/` directory of the running NVGT — one of the
folders NVGT searches for plugins. The script itself carries no
`#pragma plugin` line, because it calls none of the plugin's functions; it only
puts the file in place. The script that then uses MIDI is the one that needs
the pragma:

    nvgt scripts/get_nvmidi.nvgt           newest release
    nvgt scripts/get_nvmidi.nvgt v0.7.0    a named release

It uses nothing but nvgt's own script API — its http client, its file and
screen reader functions — so there is no separate downloader to install. The
transfer is streamed into a file opened binary, so the dll never passes
through a text encoding, and it lands under a temporary name first: a
connection that dies halfway cannot leave a truncated dll for nvgt to load on
the next run.

A dll that is currently loaded cannot be overwritten on Windows, so before it
downloads anything the script copies the installed copy onto itself. That
copy is the one test available for "is this file locked by a running nvgt": if
even a self copy fails, something has the dll open, and the script says so and
stops rather than replacing half of it — close nvgt and run it again.

This repository is private, so the short address
`releases/latest/download/nvmidi.dll` answers 404 — GitHub does not serve
release assets of a private repository to an anonymous request. The script
asks the api instead (`api.github.com/repos/<owner>/<repo>/releases/...`),
reads the signed asset address out of the json it returns, and downloads from
there; the signed address itself needs no token.

## Full API

`doc/api.md` is the reference: every type, every method, the error model and
the threading rules.

## What is not supported

- No MIDI file parsing or playback.
- No virtual port creation on Windows; `is_virtual_port`/`set_virtual_port`
  are accepted but only do anything on platforms RtMidi can support.
- Timestamps come from RtMidi and are seconds since the port was opened, not
  wall-clock time.

## Credits

Backed by RtMidi, copyright Gary P. Scavone. NVGT is copyright Sam Tupy.
See `license.md`.
