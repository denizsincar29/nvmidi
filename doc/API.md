# nvmidi API reference

Everything below is registered into the AngelScript engine by the plugin's
entry point, so it is available to any script once the library is installed.

## Types

### midi_message

One MIDI message, either received from a device or about to be sent.

| member | type | meaning |
| --- | --- | --- |
| `status` | `uint8` | command byte, e.g. `0x90` for note on |
| `data1` | `uint8` | first parameter — note number, controller, program |
| `data2` | `uint8` | second parameter — velocity, controller value |
| `channel` | `int` | 1..16, or 0 when the message carries no channel |
| `timestamp` | `double` | seconds since the port was opened |

Methods: `to_string_byte_count()` and `to_string_byte(index)` give a readable
description; `midi_message_name_byte_count(m)` / `midi_message_name_byte(m, i)`
do the same as a free function. The implicit conversion operator means a
`midi_message` can still be concatenated straight into a string — the engine
does that conversion itself, on its own side of the boundary, where its strings
are intact. See [Strings arrive as bytes](#strings-arrive-as-bytes).

For pitch bend the two data bytes combine into a 14-bit value:
`(data2 << 7) | data1`, which runs 0..16383 with 8192 as centre.

### midi_duration

A length, written in whichever unit the script thinks in; the plugin converts
it once, when the note is played.

| member | type | meaning |
| --- | --- | --- |
| `amount` | `double` | how many units — 0.5 is allowed, so half a beat is `0.5` |
| `unit` | `int` | one of `MIDI_MS`, `MIDI_TICKS`, `MIDI_BEATS`, `MIDI_BARS` |
| `tempo` | `double` | beats per minute, used by the beat and bar units. Defaults to 120 |
| `ppq` | `double` | pulses per quarter note, used by the tick unit. Defaults to 96 |

- `double to_ms() const` — the length in milliseconds, after conversion
- `int to_string_byte_count() const`, `int to_string_byte(uint index) const`
  — e.g. `"2 beats at 120 bpm"`; also the implicit
  conversion, so a duration can be concatenated into a string

Factories: `midi_duration_create()`, `midi_duration_create(amount, unit)` and
`midi_duration_create(amount, unit, tempo)`.

A beat is one quarter note and a bar is four beats — the usual 4/4 assumption,
stated because the low level port has no time signature to ask. A script that
keeps its own tempo map converts to milliseconds itself and passes `MIDI_MS`.

### midi_note

One note of a chord. A handle type: `midi_note@ n = nvmidi_note_create(60, 100);`
points at the object rather than copying it, so changing `n.velocity` later
really changes the note that is played.

| member | type | meaning |
| --- | --- | --- |
| `pitch` | `int` | MIDI note number, 60 is middle C, 0..127 |
| `velocity` | `int` | 0..127 |
| `channel` | `int` | 1..16 |
| `length` | `midi_duration` | how long the note sounds |

- `double duration_ms() const` — the length in milliseconds
- `int to_string_byte_count() const`, `int to_string_byte(uint index) const`
  — e.g. `"C4 (60), velocity 100, channel 1"`

Factories: `nvmidi_note_create()`, `nvmidi_note_create(pitch, velocity, channel)`
and `nvmidi_note_create_ms(pitch, velocity, duration_ms)`.

Two free functions name pitches both ways: `int midi_note_number("C4")` takes
`C`, `F#3`, `Bb5` and returns -1 for anything it does not understand, and
`midi_note_pitch_name_byte_count(60)` / `_byte(60, index)` goes back the other way.

### midi_config

Which port to open, read from a small text file so a tester edits the file
rather than the code.

    match = nord          # substring of the port name, case insensitive
    port = 0              # index to fall back on

`#` starts a comment and both keys are optional. With no file the defaults
stand: `match` is `"nord"`, `port` is 0.

- `bool load(const string&in path)` — a missing file is an error
- `bool load_if_present(const string&in path)` — a missing file is not
- `int find_input_port() const` / `int find_output_port() const` — the index
  to open, or -1 when neither the name nor the fallback index finds anything
- `int get_last_port() const` — where the last search ended
- `int describe_byte_count() const`, `int describe_byte(index) const` — the port
  the last search settled on, in words: `"Nord Piano 6 MIDI 2"` when the name
  matched, `"MOTU M Series MIDI In 0 (fallback port 0)"` when the search fell
  back to the index. Taken from the list that was searched, so an output search
  names an output port. Meant to be spoken; a string cannot cross this boundary
  back into the script, which is why it is a byte pair
- `int get_path_byte_count() const`, `int get_path_byte(index) const` — the file
  the configuration was loaded from, as bytes

Members `string match` and `int port` can also be set from the script instead
of a file. Create one with `midi_config_create()`.

### midi_input

Reference type, no reference counting — the plugin owns the object, so a
script never deletes it. Create one with `midi_input_create()`.

Ports

- `bool open(uint port, const string&in name = "nvmidi")`
- `bool open_by_name(const string&in substring, const string&in name = "nvmidi")`
- `bool open_config(midi_config@ config, const string&in name = "nvmidi")`
- `void close()`
- `bool is_open() const`
- `int get_port() const` — index of the open port, `-1` when closed
- `int get_port_name_byte_count() const`, `int get_port_name_byte(uint index) const`

Reading

- `bool next_message(midi_message&out) const` — takes the oldest queued
  message off the queue; false when empty
- `bool has_message() const`
- `uint get_pending() const`
- `void clear()`

Playing

- `bool play_chord(array<midi_note@>@ notes)`
- `bool play_chord_wait(array<midi_note@>@ notes)`
- `bool play_note(const midi_note&in note)`
- `bool play_note_wait(const midi_note&in note)`
- `bool play_midi_chord(array<midi_note@>@ notes, const string&in pattern)`
- `bool play_midi_chord_wait(array<midi_note@>@ notes, const string&in pattern)`
- `bool play_sequence(array<midi_note@>@ notes)`
- `uint stop_all_notes()`
- `uint get_active_notes() const`

These send the notes back out of the port they came from, so a keyboard with a
built in sound engine makes them heard with no synthesis on this side. RtMidi
treats an input and an output port as two connections even when they are the
same socket, so an output is opened for the duration of the chord. The pattern
names and the schedule behind them are the ones documented under `midi_output`
below — same player, same behaviour.

Tempo

- `double tempo` — beats per minute, 120 by default
- `midi_duration duration(double amount, int unit) const` — a length already
  resolved against this tempo

A note whose length is written in beats, bars or ticks is converted at this
tempo when it is played, so `in.tempo = 96.0` followed by
`in.duration(1.0, MUSIC_BEATS)` is one beat at 96 rather than at the 120
default. Milliseconds ignore the tempo. An explicitly qualified unit —
`MUSIC_BEATS_90`, `_100`, `_120`, `_140` — carries its own tempo and wins over
the class one.

Filters, applied through RtMidi's own `ignoreTypes`. Both default to true.

- `void set_ignore_sysex(bool)` / `bool get_ignore_sysex() const`
- `void set_ignore_timing(bool)` / `bool get_ignore_timing() const`

### midi_output

Reference type, no reference counting. Create one with
`midi_output_create()`.

Ports — the same six methods as `midi_input`, including `open_config`.

Sending

- `bool send(uint status, uint data1, uint data2)` — raw bytes, no
  interpretation
- `bool send_packed(uint packed)` — the same three bytes packed into one
  integer, lowest byte first
- `void send_note_on(uint channel, uint note, uint velocity)`
- `void send_note_off(uint channel, uint note, uint velocity = 0)`
- `void send_control_change(uint channel, uint controller, uint value)`
- `void send_program_change(uint channel, uint program)`
- `void send_pitch_bend(uint channel, uint value)`
- `void send_aftertouch(uint channel, uint note, uint pressure)`
- `void send_channel_pressure(uint channel, uint pressure)`
- `void send_sysex(const string&in data)`
- `void all_notes_off()`
- `void reset()`

The `send_*` helpers take a 1-based channel and build the status byte for
you. `send` and `send_packed` do not: pass the full status byte, including
the channel in its low nibble.

Playing

- `bool play_chord(array<midi_note@>@ notes)` — every note at once, released
  when the length of the first note is over. Returns as soon as the notes are
  out; the release happens on a timer, which only fires while the script keeps
  running, so a script that ends right after the call cuts the chord short
- `bool play_chord_wait(array<midi_note@>@ notes)` — the same, but the notes
  are released before it returns
- `bool play_note(const midi_note&in note)` / `bool play_note_wait(...)` — one
  note with the same split
- `uint stop_all_notes()` — releases everything the high level layer is
  holding, returns how many that was
- `uint get_active_notes() const`
- `double tempo` and `midi_duration duration(double amount, int unit) const` —
  the same pair `midi_input` has, documented above

Patterns

- `bool play_midi_chord(array<midi_note@>@ notes, const string&in pattern)`
- `bool play_midi_chord_wait(array<midi_note@>@ notes, const string&in pattern)`
- `bool play_sequence(array<midi_note@>@ notes)` — one after another, each for
  its own length, so a scale sounds as a scale

Pattern names: `chord`, `spread`, `arpeggio`, `quick`, `fast`, `sequence`,
`repeat`, `strum`. Each compiles down to a schedule of note ons and releases
that one player runs, so both entry points behave identically — `play_midi_chord`
returns once the last note has been released, and `play_midi_chord_wait` is the
same call with the intent spelled out. A script that wants to do something
while a pattern plays should call the non waiting variants or drive the raw
`send_*` calls from its own clock.

Notes that share a moment go out together: a chord sends all of its note ons in
one batch, and from then on each note is released at its own moment plus its
own length. That is what lets an arpeggio keep its earlier notes ringing while
the later ones arrive.

## Strings arrive as bytes

This engine publishes no string factory, so a string returned by a plugin is
written into memory the engine is not holding a string in. It does not fail
loudly — it corrupts the process and shows up later as a stack overrun.
Measured on a windows runner, one process, one second, the same value read both
ways: the byte path gave `nvmidi/Windows MultiMedia`, the string path gave `0u??z`.

So every string surface is a pair: a count and an indexed byte.

    int midi_api_name_byte_count()      // length of the text
    int midi_api_name_byte(uint index)  // one byte, 0..255, or -1

`-1` means there is no byte at that index. It is not the same as a zero byte —
a name may contain one — so the end of the text stays distinguishable from a
byte inside it.

```angelscript
// See TECHNICAL.md - never walk a table with substr() on this engine. In short:
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
string s = "";
for (int i = 0; i < midi_api_name_byte_count(); i++) s += char_of(midi_api_name_byte(i));
```

Object methods follow the same shape: `get_port_name_byte_count()` /
`get_port_name_byte(index)`, `describe_byte_count()` / `describe_byte(index)`,
`to_string_byte_count()` / `to_string_byte(index)`. The `examples/` folder has
a four-line helper for each.


## Free functions

- `uint midi_input_port_count()`
- `uint midi_output_port_count()`
- `int midi_input_port_name_byte_count(uint port)`, `int midi_input_port_name_byte(uint port, uint index)`
- `int midi_output_port_name_byte_count(uint port)`, `int midi_output_port_name_byte(uint port, uint index)`
- `int midi_find_input_port(const string&in substring)` — first port whose name
  contains the substring, ignoring case; -1 when nothing matches
- `int midi_find_output_port(const string&in substring)`
- `int midi_api_name_byte_count()`, `int midi_api_name_byte(uint index)` — the active backend, e.g. `"ALSA"`
- `int midi_note_number(const string&in name)` — `"C4"` to 60, -1 when unknown
- `int midi_note_pitch_name_byte_count(int pitch)`, `int midi_note_pitch_name_byte(int pitch, uint index)`
- `int midi_last_error_byte_count()`, `int midi_last_error_byte(uint index)` — a count of 0 when the last call succeeded
- `int midi_message_name_byte_count(const midi_message&in m)`, `int midi_message_name_byte(const midi_message&in m, uint index)`
- `midi_input@ midi_input_create()`
- `midi_output@ midi_output_create()`
- `midi_config@ midi_config_create()`
- `midi_duration midi_duration_create()`, `midi_duration midi_duration_create(double amount, int unit)`,
  `midi_duration midi_duration_create(double amount, int unit, double tempo)` - by value: a
  `midi_duration` is a value type and can never be a handle
- `midi_note@ nvmidi_note_create()`

Unit constants, so a script never has to remember 0..3: `MIDI_MS`,
`MIDI_TICKS`, `MIDI_BEATS`, `MIDI_BARS`. `MUSIC_MS`, `MUSIC_TICKS`,
`MUSIC_BEATS` and `MUSIC_BARS` are the same four values under the name the
music classes use. `MUSIC_BEATS_90`, `MUSIC_BEATS_100`, `MUSIC_BEATS_120` and
`MUSIC_BEATS_140` are beats at that tempo, and a length built with one of them
keeps that tempo whatever the class is set to.

## Errors

RtMidi reports failure by throwing. The plugin catches everything at the
boundary and turns it into a return value, so a script never dies from a
disconnected device:

- `open` and `send*` return `false` on failure.
- The specific reason is in `midi_last_error_byte_count()` / `_byte()`, cleared at the start of
  every call.

Functions that return a value rather than a status — `midi_input_port_name`
and friends — return an empty string or zero on failure, again with the
reason in `midi_last_error_byte(uint index)`.

## Playing without blocking the script

`play_chord` and `play_note` return immediately and let the release happen on
a timer; the timer runs inside the `wait()` the script already calls in its
loop, so a game keeps its frame rate while a chord rings. Their `_wait`
siblings instead run to the end of the note before returning, which is what a
test or a step by step progression wants.

The blocking wait is built out of a deadline and short `wait()` slices rather
than one long sleep, so a script that is drawing or reading keys keeps going.

## Threading

Incoming messages are delivered on RtMidi's thread and copied into a
lock-protected queue; `next_message` moves them to your script's thread.
AngelScript objects are never touched from the callback, and the notes you
pass to the playing functions are copied out of the array before any of them
is sent, so nothing the script owns is read from another thread.

The queue holds 4096 messages. When it overflows the oldest message is
discarded, so a script that stops draining loses history rather than
memory. Call `clear()` after a pause to drop a backlog you no longer care
about.
