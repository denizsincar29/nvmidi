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

Windows is built with MSVC — run `scripts\build_win_msvc.bat` — and not with
MinGW, and that is a measurement rather than a preference.

`nvgt.exe` on Windows is an MSVC build: it loads `msvcp_win.dll` and
`ucrtbase.dll`. A MinGW build of this plugin carries libstdc++'s `std::string`
while the engine carries MSVC's, and the two do not agree on where a string
keeps its data. Measured on 2026-10-01, with a MinGW-built `nvmidi.dll`
installed into a real nvgt:

    midi_find_input_port("nord")       -> -1    (the port is there)
    midi_note_number("C4")             -> -1    (C4 is 60)
    midi_config::load_if_present(...)  -> access violation
    what the plugin received for "nord": size=0 data=0x64726f6e capacity=4

That last line is the whole story. MSVC keeps a short string's characters at
offset 0, its length at offset 16 and its capacity at offset 24; libstdc++
expects the pointer at offset 0, the length at offset 8 and its buffer at
offset 16. The plugin was reading a *length* out of the middle of the text, so
every string argument arrived empty or as garbage - and a pointer-shaped
argument that reaches `std::ifstream` faults, which is what the crash was. The
same source built with MSVC receives `size=4 first byte=110` for "nord",
answers 60 for "C4", and returns true from `load_if_present()`.

`scripts\build_win_msvc.bat` does three things beyond calling the compiler:

- finds the C++ tools through `vswhere`;
- generates an import library from the *running engine's* export table
  (`dumpbin /exports nvgt.exe`, then `lib /def:`), because MSVC wants the
  Angelscript runtime symbols (`asAllocMem`, `asGetLibraryOptions`, ...) at
  link time, where MinGW leaves them undefined for the loader to fill in;
- links `winmm`, `ole32`, `setupapi` and `ksuser`, and writes `nvmidi.dll`.

Set `NVGT_HOME` to the folder holding `nvgt.exe` if it is not on `PATH`. The
MinGW path in the `Makefile` still builds, and the library it produces still
enumerates ports and still moves MIDI bytes; it just cannot receive a string.
Do not ship it.

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
the run ends at the pragma even when the script declares its own `bytes_of_last_error()`
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

`midi_first_error_byte_count()` / `midi_first_error_byte(index)` stay in the API for the case where the plugin *did*
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
		screen_reader_speak(bytes_of_input_port_name(i));
	}
	if (!in.open(0)) { // first input port
		screen_reader_speak("could not open the keyboard: " + bytes_of_last_error());
		return;
	}

	while (true) {
		midi_message m;
		while (in.next_message(m)) {
			// m is a midi_message; bytes_of_message(m) reads like
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
screen_reader_speak("Using " + bytes_of_config(config)); // "Nord Piano 6"
in.open_config(config);
```

`describe_byte_count()` names the port it picked and says when it fell back to the index,
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
	screen_reader_speak(bytes_of_last_error());
	return;
}
out.send_note_on(1, 60, 100);   // channel 1, middle C, velocity 100
wait(500);
out.send_note_off(1, 60);
```

`all_notes_off()` stops everything on all sixteen channels, which is worth
calling on shutdown so a panic does not leave a stuck note sounding.


## Strings arrive as bytes

Every string this plugin would hand to a script instead comes over as a length
and an indexed byte, and the script puts the text back together itself.

That is not a style choice. This engine publishes no string factory, so a
plugin that returns a string writes its bytes into memory the engine is not
holding a string in. It does not fail loudly; it corrupts the process, and
surfaces later as a stack overrun. Measured on a windows runner in one process,
in the same second, reading the same value both ways:

    byte path    nvmidi/Windows MultiMedia  (25 bytes, exact)
    string path  0u??z               (garbage)

So the rule for this plugin: **integers cross the boundary, strings do not.**

One qualifier, added 2026-10-01 and worth a line of its own: that rule is about
text coming *out*. Text going *in* is fine. The `const string&in` parameters
these functions take - `midi_find_input_port("nord")`, `midi_note_number("C4")`,
`open_by_name`, `midi_config::load` - do arrive intact, measured on a machine
with a real Nord attached, once the library is built the way *Windows* above
describes. What has no way out is only the *return* value, because the engine
publishes no string factory and a plugin cannot build a string the engine will
accept.

For every string surface there is a pair — a count and an indexed byte:

    int midi_api_name_byte_count()          // how long the text is
    int midi_api_name_byte(uint index)      // one byte, 0..255, or -1

`-1` means "there is no byte at that index", which is not the same as a zero
byte: a port name may legitimately contain one, so the end of the text has to
be distinguishable from a byte inside it.

Read it back into a script string like this:

```angelscript
// A whole byte back as one character. Read only the two ranges this engine
// reads correctly - measured, find("1") is 1 and find("A") is 10 - and name the
// punctuation a backend name is made of one byte at a time. Never walk a table
// with substr(): measured, index 32 answers 'W', index 65 answers '-', and
// substr(67, 1) is already empty on a literal whose length() is 68. Bounds are
// numbers, not character literals - `b >= '0'` does not compile here.
const string byte_letters = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz/._-+ ";
const string digits = "0123456789";
string char_of(uint b) {
	if (b >= 48 && b <= 57) return digits.substr(b - 48, 1);
	if (b >= 65 && b <= 90) return byte_letters.substr(b - 65 + 10, 1);
	if (b >= 97 && b <= 122) return byte_letters.substr(b - 97 + 36, 1);
	if (b == 47) return "/";
	if (b == 46) return ".";
	if (b == 95) return "_";
	if (b == 45) return "-";
	if (b == 43) return "+";
	if (b == 32) return " ";
	return ".";
}

string bytes_of_api_name() {
	string s = "";
	for (int i = 0; i < midi_api_name_byte_count(); i++) {
		s += char_of(midi_api_name_byte(i));
	}
	return s;
}

screen_reader_speak("MIDI backend: " + bytes_of_api_name());
```

Every `examples/*.nvgt` file carries this helper; copy the one you need.

The shape is the same everywhere. Where a global has a pair, `midi_api_name`
above does; where an object method does, it is spelled `get_port_name_byte_count()`
and `get_port_name_byte(uint index)` on the object, or `describe_byte_count()`
on a config. `examples/` has a helper for each one, and they are four lines
each — copy whichever you need.

If a future engine grows a string factory, the plugin prints a warning at
registration and these pairs become optional rather than necessary. Until a
run says otherwise, they are the only form measured to survive.

## API

**Free functions**

- `uint midi_input_port_count()` / `uint midi_output_port_count()` — how many
  ports the machine has.
- `int midi_input_port_name_byte_count(uint port)` and
  `midi_output_port_name_byte_count(uint)` — the length of one port's name, 0
  if the index is out of range; `_byte(port, index)` gives one byte, or -1.
- `int midi_api_name_byte_count()` / `midi_api_name_byte(uint index)` — which
  backend is in use, e.g. `"ALSA"` or `"Windows MultiMedia"`. Useful in a bug
  report.
- `int midi_last_error_byte_count()` / `midi_last_error_byte(uint index)` —
  why the last call failed; a count of 0 on success.
- `int midi_message_name_byte_count(const midi_message&in m)` and
  `midi_message_name_byte(m, index)` — human-readable description of a
  message.

See "Strings arrive as bytes" below for the three lines that turn a pair back
into a string.
- `midi_input@ midi_input_create()` / `midi_output@ midi_output_create()` —
  make an object. The plugin owns it, so you do not delete it yourself.
- `int midi_find_input_port(const string&in substring)` /
  `midi_find_output_port` — the first port whose name contains the substring,
  ignoring case; -1 when nothing matches.
- `int midi_note_number(const string&in name)` — `"C4"`, `"F#3"`, `"Bb5"` to a
  MIDI note number, -1 when the name is not understood.
  `midi_note_pitch_name_byte_count(pitch)` / `_byte(pitch, index)` go the other
  way, for a note number.
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
since the port opened). `to_string_byte_count()` and `to_string_byte(index)`
give the readable form; the `to_string()` that returns it as a string is not
registered.

**midi_config** — `bool load(const string&in path)` and
`bool load_if_present(path)` read a file of `key = value` lines (`#` comments,
keys `match` and `port`); `find_input_port()` / `find_output_port()` return the
index to open; `describe_byte_count()` / `describe_byte(index)` say in words
which port that is; `get_last_port()` reports where the search ended. The `match` and `port` members can be set from
the script instead of a file. With no file the defaults are `"nord"` and 0.

**midi_note** — fields `pitch`, `velocity`, `channel` and `length` (a
`midi_duration`). `duration_ms()` gives the length in milliseconds and
`to_string_byte_count()` / `to_string_byte(index)` name the note, e.g.
`"C4 (60), velocity 100, channel 1"`.

**midi_duration** — fields `amount`, `unit`, `tempo` and `ppq`; `to_string_byte_count()` / `to_string_byte(index)` describe the length; `to_ms()`
converts once, using 120 bpm and 96 ppq unless the object says otherwise. A
beat is a quarter note and a bar is four of them. A duration made by `tempo`-
aware code (`n.duration()`) already has the class tempo written into it, so
`to_ms()` and a later playback agree.

**midi_input**

- `bool open(uint port, const string&in name = "nvmidi")`, plus
  `open_by_name(substring)` and `open_config(config)`. With any of them,
  `void close()`, `bool is_open()`, `int get_port()`,
  `get_port_name_byte_count()` / `get_port_name_byte(index)`.
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
- `int sustain`, `int sostenuto`, `int soft` — the value of CC 64, 66 and 67
  as they arrive from the port, 0..127, and 0 until the instrument sends one.
  Read-only: a script asks whether the pedal is down instead of watching the
  queue for the controller itself.
- `uint stop_all_notes()`, `uint get_active_notes()`.
- `void set_ignore_sysex(bool)` / `bool get_ignore_sysex()` — default true.
  Sysex dumps are large and rarely useful in a game.
- `void set_ignore_timing(bool)` / `bool get_ignore_timing()` — default true.
  Drops clock and active-sensing bytes, which arrive continuously and are
  almost never what you want.

Changing either filter takes effect immediately on an open port.

**midi_output**

- `bool open(uint port, const string&in name = "nvmidi")`, `void close()`,
  `bool is_open()`, `int get_port()`, `get_port_name_byte_count()` /
  `get_port_name_byte(index)`.
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
- `int sustain`, `int sostenuto`, `int soft` — the same three pedals, readable
  *and* writable here: assigning sends that controller on channel 1 and keeps
  the value, so `midi_out.sustain = 127` is the pedal going down and `= 0` is it
  coming up. Values are clamped to 0..127.
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

Four more are smoke tests rather than demonstrations. The three that need
nobody open the instrument's own output port and ripple C-E-G-E before their
first check, so the person sitting at the piano hears a run begin:

- `test_lowlevel` - the whole `midi_output` surface, the port functions and the
  byte pairs, one line of `TEST <name> PASS|FAIL` per call and a `RESULT` line
  at the end;
- `test_music` - `midi_note` and `midi_duration` fields, every unit constant,
  `tempo`, and `play_note` / `play_chord` / `play_midi_chord` / `play_sequence`
  with every pattern name;
- `test_config` - `midi_config`: load, the two searches, `get_last_port`,
  `describe` and `get_path`. Run it from a directory holding
  `midi_config.txt` (`match=zzzznope`, `port=1`) and `midi_config_named.txt`
  (`match=nord`, `port=0`) - the header says so too.

- `test_input_live` - the one that does need a person. It ripples C-E-G-E on
  middle C to say a run has started, then the *same ripple an octave down,
  twice* to say "play the instrument now", listens for 30 seconds, prints and
  counts every message that comes out of it, and sends each note back an octave
  up. A low ripple once more at the end says the turn is over. Measured on
  2026-10-01: 193 messages in, 97 note-ons, 193 events echoed, 0 failures.

Each writes `apitest.log` next to itself as it goes, because the engine buffers
`print()` until a clean exit and a crash would otherwise take the whole
transcript with it.

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
