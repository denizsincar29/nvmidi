# nvmidi

MIDI input and output for [NVGT](https://nvgt.dev) — the NonVisual Gaming
Toolkit.

This plugin lets an NVGT script talk to MIDI hardware and software: read a
MIDI keyboard, pad controller or wind controller, and send MIDI to a
synthesiser, a DAW, or a virtual port. It is a thin binding over
[RtMidi](https://github.com/thestk/rtmidi), which supplies the platform layer
— WinMM on Windows, ALSA on Linux.

Everything is exposed to AngelScript under plain names: `midi_input`,
`midi_output`, `midi_message`, and a handful of free functions.

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

Put the built library where NVGT looks for plugins — next to `nvgt.exe` on
Windows, or in the plugins directory on Linux. Restart NVGT; the functions
below become available to every script.

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

**midi_message**

Fields `status`, `data1`, `data2` (all `uint8`), `channel` (`int`, 1..16, or
0 for a message that carries no channel), and `timestamp` (`double`, seconds
since the port opened). `to_string()` gives the readable form.

**midi_input**

- `bool open(uint port, const string&in name = "nvmidi")` — open a port, and
  `void close()`, `bool is_open()`, `int get_port()`,
  `string get_port_name()`.
- `bool next_message(midi_message&out)` — take the oldest queued message.
  Returns false when the queue is empty.
- `bool has_message()`, `uint get_pending()`, `void clear()`.
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

## What is not supported

- No MIDI file parsing or playback.
- No virtual port creation on Windows; `is_virtual_port`/`set_virtual_port`
  are accepted but only do anything on platforms RtMidi can support.
- Timestamps come from RtMidi and are seconds since the port was opened, not
  wall-clock time.

## Credits

Backed by RtMidi, copyright Gary P. Scavone. NVGT is copyright Sam Tupy.
See `license.md`.
