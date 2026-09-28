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

Methods: `to_string()` returns a readable description, and the implicit
conversion operator means a `midi_message` can be concatenated straight into
a string. `midi_message_name(m)` does the same as a free function.

For pitch bend the two data bytes combine into a 14-bit value:
`(data2 << 7) | data1`, which runs 0..16383 with 8192 as centre.

### midi_input

Reference type, no reference counting — the plugin owns the object, so a
script never deletes it. Create one with `midi_input_create()`.

Ports

- `bool open(uint port, const string&in name = "nvmidi")`
- `void close()`
- `bool is_open() const`
- `int get_port() const` — index of the open port, `-1` when closed
- `string get_port_name() const`

Reading

- `bool next_message(midi_message&out) const` — takes the oldest queued
  message off the queue; false when empty
- `bool has_message() const`
- `uint get_pending() const`
- `void clear()`

Filters, applied through RtMidi's own `ignoreTypes`. Both default to true.

- `void set_ignore_sysex(bool)` / `bool get_ignore_sysex() const`
- `void set_ignore_timing(bool)` / `bool get_ignore_timing() const`

### midi_output

Reference type, no reference counting. Create one with
`midi_output_create()`.

Ports — the same five methods as `midi_input`.

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

## Free functions

- `uint midi_input_port_count()`
- `uint midi_output_port_count()`
- `string midi_input_port_name(uint port)`
- `string midi_output_port_name(uint port)`
- `string[]@ midi_input_port_names()`
- `string[]@ midi_output_port_names()`
- `string midi_api_name()` — the active backend, e.g. `"ALSA"`
- `string midi_last_error()` — empty when the last call succeeded
- `string midi_message_name(const midi_message&in m)`
- `midi_input@ midi_input_create()`
- `midi_output@ midi_output_create()`

## Errors

RtMidi reports failure by throwing. The plugin catches everything at the
boundary and turns it into a return value, so a script never dies from a
disconnected device:

- `open` and `send*` return `false` on failure.
- The specific reason is in `midi_last_error()`, cleared at the start of
  every call.

Functions that return a value rather than a status — `midi_input_port_name`
and friends — return an empty string or zero on failure, again with the
reason in `midi_last_error()`.

## Threading

Incoming messages are delivered on RtMidi's thread and copied into a
lock-protected queue; `next_message` moves them to your script's thread.
AngelScript objects are never touched from the callback.

The queue holds 4096 messages. When it overflows the oldest message is
discarded, so a script that stops draining loses history rather than
memory. Call `clear()` after a pause to drop a backlog you no longer care
about.
