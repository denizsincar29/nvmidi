/* nvmidi.h - MIDI input and output for NVGT
 *
 * Exposes RtMidi to Angelscript: enumerating MIDI ports, opening input and
 * output connections, sending raw MIDI messages, and a queue of incoming
 * messages that the script drains from the main loop.
 *
 * Copyright (c) 2026 Дениз
 * Licensed under the same terms as NVGT itself (zlib-style, see license.md).
 */

#pragma once

#include <string>
#include <vector>
#include <deque>
#include <mutex>

// The export macro for the free functions further down, and the only reason
// this header includes another one.
//
// It is included rather than defined here because the decision it encodes -
// dllexport on Windows, default visibility on ELF, nothing for a static build -
// is NVGT's to make and nvgt_plugin.h already makes it, under a name meant for
// exactly this. A second copy of that #ifdef in this file would be a second
// answer to the same question, and the two would drift the moment either
// platform grew a third case.
//
// The dependency runs one way. nvgt_plugin.h does not include this file and
// does not know it exists; nvmidi.cpp includes both, in the order it does
// because this file needs the macro before it reaches the declarations below.
#include "nvgt_plugin.h"

class asIScriptEngine;
class CScriptArray;
class midi_config;

// A single MIDI message as received from or sent to a device.
// status is the command byte (note on/off, control change, ...), data1 and
// data2 are the two parameter bytes, and channel is the low nibble of status.
struct midi_message {
	unsigned char status;
	unsigned char data1;
	unsigned char data2;
	int channel; // 1..16
	double timestamp; // seconds since the input port was opened
	// std::string, and the type says asOBJ_POD anyway. That pair is a lie and
	// it is what has been killing the engine: a std::string has a destructor,
	// so this type is NOT plain old data, and the engine trusts asOBJ_POD
	// enough to skip the destructor it would otherwise have called. AngelScript
	// only asks a subtype for a default constructor when the flags say
	// asOBJ_VALUE and not asOBJ_POD (scriptarray.cpp:142) - so the moment
	// `array<midi_message>` appears, the engine tries to build one by copying
	// bytes and never runs the constructor this string never got.
	//
	// It also explains the errors that looked like they were about the
	// declaration: "Expected ';'" and "Expected expression value" after
	// `midi_message m;`, on whichever line the next statement happened to sit.
	// The engine cannot promise a default constructor it was told does not
	// exist, so the declaration itself is refused.
	// Returns a pointer into a per-object buffer rather than a std::string -
	// not a design choice, just the shape this one had before the string
	// question came up, and it is not registered either way.
	//
	// Nothing string-shaped is handed to the engine any more, in this type or
	// any other. This engine publishes no string factory, so a plugin-returned
	// string arrives in the wrong layout and corrupts the process; the text
	// goes out as a length plus indexed bytes instead. See the note above the
	// byte definitions in nvmidi.cpp for the measurement that forced it.
	const char* to_string() const;
	// The same text as bytes, and the form that is registered.
	int to_string_byte_count() const;
	int to_string_byte(unsigned int index) const;
private:
	mutable std::string buffer; // holds the text the returned pointer points at
};
// ---------------------------------------------------------------------------
// midi_duration - how long something lasts, built from a unit and a count
// ---------------------------------------------------------------------------
//
// A length the script writes in whichever unit it thinks in, and the plugin
// converts once. Beats and bars need a tempo, which only the caller knows, so
// they carry a tempo field; milliseconds and ticks do not use it.
//
// The low level port does not know what a beat is at a given moment either,
// so the conversion is deliberately simple: a beat is one quarter note, and a
// bar is four beats, the usual 4/4 assumption. A script that keeps its own
// tempo map converts to milliseconds itself and passes MS.

enum midi_duration_unit {
	MIDI_UNIT_MS = 0,
	MIDI_UNIT_TICKS = 1,
	MIDI_UNIT_BEATS = 2,
	MIDI_UNIT_BARS = 3
};

// Together with MIDI_UNIT_* so that a script can write the unit and the tempo
// it belongs to in one word: MUSIC_BEATS instead of (MIDI_BEATS, 120).
enum midi_music_unit {
	MUSIC_MS = 0,
	MUSIC_TICKS = 1,
	MUSIC_BEATS = 2,
	MUSIC_BARS = 3,
	MUSIC_BEATS_120 = 4,
	MUSIC_BEATS_90 = 5,
	MUSIC_BEATS_100 = 6,
	MUSIC_BEATS_140 = 7
};

// What midi_output::send_transposed did with the message it was handed. A
// plain bool would not carry the difference between "sent, and it was a note"
// and "sent, but note numbers are not what this message has", and a script
// counting what came back from an echo needs that difference.
enum midi_transpose_result {
	// Sent with its note number moved.
	TRANSPOSE_SENT = 0,
	// Sent unchanged: a message with no note number in it.
	TRANSPOSE_PASSED_THROUGH = 1,
	// Not sent: moving the note would put it outside 0..127.
	TRANSPOSE_OUT_OF_RANGE = 2,
	// Not sent: no output port is open, or the driver refused the message.
	TRANSPOSE_FAILED = 3
};

// The tempo an explicit MUSIC_BEATS_* constant carries, 0 for the constants
// that simply mean "the unit, at whatever tempo the class is set to".
double music_unit_tempo(int unit);

// Works out what a duration's amount really is in milliseconds, given the
// class tempo that stands in for the 120 default. Shared by midi_duration and
// the music class so both convert identically.
double midi_duration_to_ms(double amount, int unit, double tempo, double ppq);

struct midi_duration {
	// How many units this length is made of. Fractional beats and bars are
	// allowed, a half beat is 0.5.
	double amount;
	int unit; // one of the MIDI_UNIT_* values
	// Beats per minute, used by the beats and bars units. Defaults to 120.
	// The music class overwrites it when a note is played, so a duration made
	// there does not have to be written at the class tempo by hand.
	double tempo;
	// Pulses per quarter note, used by the ticks unit. Defaults to 96, which
	// is what most MIDI files assume.
	double ppq;

	std::string to_string() const;
	int to_string_byte_count() const;
	int to_string_byte(unsigned int index) const;
	// The length in milliseconds, after the unit conversion.
	double to_ms() const;

	midi_duration();
	midi_duration(double amount, int unit);
	midi_duration(double amount, int unit, double tempo);

	// Assignment from the parts of another duration. The default copy
	// assignment only exists in C++; without this, a script could read a
	// duration property but never write one, and note.length = duration(...)
	// would not compile.
	midi_duration& opAssign(const midi_duration& other);

	// The same, taking the parts back out of a duration that has to be
	// converted first - midi_note::duration_ms is registered as a double
	// living inside a midi_duration, and a script writes it directly.
	midi_duration& opAssign(double amount);
};

// These return by value. A duration is registered as a plain value type, and
// a value type can never be a handle in Angelscript (see asCDataType::MakeHandle
// in the SDK: only asOBJ_REF, asOBJ_TEMPLATE_SUBTYPE, asOBJ_ASHANDLE and
// asOBJ_FUNCDEF can carry a @). An earlier version returned midi_duration*
// and registered them as returning midi_duration@, which is a registration
// Angelscript rejects outright.
midi_duration midi_duration_create();
midi_duration midi_duration_create_full(double amount, int unit);
midi_duration midi_duration_create_tempo(double amount, int unit, double tempo);

// Placement-new wrappers for the Angelscript construct and destruct
// behaviours. asCALL_CDECL_OBJLAST hands these the address of the object being
// built as the last parameter, so each takes one argument more than the
// constructor it calls.
void midi_duration_default_construct(midi_duration* self);
void midi_duration_construct(midi_duration* self, double amount, int unit);
void midi_duration_construct_tempo(midi_duration* self, double amount, int unit, double tempo);
void midi_duration_destruct(midi_duration* self);

// ---------------------------------------------------------------------------
// midi_note - one note, with its pitch and its length
// ---------------------------------------------------------------------------
//
// One note of a chord, as the high level layer speaks it.
// pitch follows the MIDI convention, 60 is middle C, 0..127.
struct midi_note {
	int pitch;
	int velocity;
	int channel; // 1..16
	// How long the note sounds. When a whole chord is played, the length of
	// the first note is the one that counts; the others are ignored.
	midi_duration length;
	std::string to_string() const;
	int to_string_byte_count() const;
	int to_string_byte(unsigned int index) const;
	// The length in milliseconds, which is what the layer below actually
	// needs. Handy for a script that wants to show or print it.
	double duration_ms() const;
	// The same, but the tempo argument wins over the one written into the
	// length itself. The music class uses this to play a note written as
	// "one beat" at the tempo the class is currently set to.
	double duration_ms_at(double tempo) const;

	midi_note();
	midi_note(int pitch);
	midi_note(int pitch, int velocity);
};

midi_note* midi_note_create();
// Overloads so the constructor form midi_note(60) / midi_note(60, 100) can be
// registered as an object or a global function: an overloaded name cannot be
// resolved by asFUNCTION, only by asFUNCTIONPR with the parameter list.
midi_note* midi_note_create_pitch(int pitch);
midi_note* midi_note_create_velocity(int pitch, int velocity);
midi_note* midi_note_create_full(int pitch, int velocity, int channel);
// Takes the length straight in milliseconds, the unit every clock agrees on.
midi_note* midi_note_create_ms(int pitch, int velocity, double duration_ms);

// Copies the notes that go to a music class, resolving every length against
// that class's tempo: read_notes overloads the tempo from the class while
// read_notes_at leaves the tempo written into each note in charge.
bool read_notes(CScriptArray& notes, double tempo, std::vector<midi_note>& out);
bool read_notes_at(CScriptArray& notes, std::vector<midi_note>& out);

// One MIDI message queued for the script to read.
// Messages arrive on RtMidi's own thread, so they are buffered here and
// handed to the script only when it asks (next_message / poll).
class midi_input {
public:
	midi_input();
	~midi_input();

	// Opens the input port at the given index of the enumerated device list.
	bool open(unsigned int port, const std::string& name = "nvmidi");
	// Opens the first port whose name contains the substring, ignoring case.
	bool open_by_name(const std::string& substring, const std::string& name = "nvmidi");
	// Opens whatever the configuration points at.
	bool open_config(midi_config* config, const std::string& name = "nvmidi");
	// Plays the notes itself, on the device the port belongs to: the notes are
	// sent back out and the keyboard's own sound engine makes them heard. For
	// a keyboard with a built in synth this is the shortest way to sound a
	// chord without writing any synthesis. Blocks until the chord is over.
	bool play_chord(CScriptArray& notes);
	bool play_chord_wait(CScriptArray& notes);
	bool play_note(const midi_note& note);
	bool play_note_wait(const midi_note& note);
	// The same patterns midi_output offers, played through the keyboard's own
	// engine: the notes go back out of the port they came from, so a script
	// that only ever talks to one device needs this class alone.
	bool play_midi_chord(CScriptArray& notes, const std::string& pattern);
	bool play_midi_chord_wait(CScriptArray& notes, const std::string& pattern);
	// The notes one after another, each for its own length.
	bool play_sequence(CScriptArray& notes);
	// A duration written at this class's tempo: a chord whose length is
	// "one beat" is one beat at the tempo set here, not always at 120.
	midi_duration duration(double amount, int unit) const;
	// Beats per minute, used when a note's length is written in beats, bars or
	// ticks. Milliseconds ignore it. Defaults to 120.
	double tempo;
	// Releases everything this port is holding sounding, returns how many.
	unsigned int stop_all_notes();
	unsigned int get_active_notes() const;
	void close();
	bool is_open() const;
	int get_port() const; // index of the open port, -1 when closed
	std::string get_port_name() const;
	int get_port_name_byte_count() const;
	int get_port_name_byte(unsigned int index) const;

	// True when at least one message is waiting.
	bool has_message() const;
	// Number of messages waiting to be read.
	unsigned int get_pending() const;
	// Takes the oldest message off the queue. Returns false when empty.
	bool next_message(midi_message& out);
	// Discards every queued message.
	void clear();

	// When true (default), the port is opened with RtMidi's ignore_sysex flag,
	// so large system-exclusive dumps never reach the queue.
	void set_ignore_sysex(bool value);
	bool get_ignore_sysex() const;
	// When true (default), timing clock and active sensing bytes are dropped.
	void set_ignore_timing(bool value);
	bool get_ignore_timing() const;

	// Called from RtMidi's thread; must not touch Angelscript objects.
	void push_message(const unsigned char* bytes, size_t count, double delta);

private:
	void* midi_in; // RtMidiIn*, kept as void* so RtMidi headers stay out of here
	int port_index;
	bool ignore_sysex;
	bool ignore_timing;
	mutable std::mutex queue_mutex;
	std::deque<midi_message> queue;
	unsigned int queue_limit;
	double opened_at;
	// Set while the high level layer is playing, so a callback that arrives
	// meanwhile is tolerated rather than treated as a surprise.
	bool playing;
	// The notes the high level layer is holding sounding.
	std::vector<midi_note> sounding;
};

// An open MIDI output port.
class midi_output {
public:
	midi_output();
	~midi_output();

	bool open(unsigned int port, const std::string& name = "nvmidi");
	// Opens the first port whose name contains the substring, ignoring case.
	bool open_by_name(const std::string& substring, const std::string& name = "nvmidi");
	// Opens whatever the configuration points at.
	bool open_config(midi_config* config, const std::string& name = "nvmidi");
	void close();
	bool is_open() const;
	int get_port() const;
	std::string get_port_name() const;
	int get_port_name_byte_count() const;
	int get_port_name_byte(unsigned int index) const;

	// -----------------------------------------------------------------
	// High level playing
	// -----------------------------------------------------------------
	//
	// These build the note on / note off pairs for you and keep track of what
	// is still sounding, so a chord cannot be left hanging by a mistake in
	// the script.

	// Plays a chord: every note at once, then they are released when the
	// length of the first note is over.
	//
	// play_chord returns as soon as the notes are out and leaves the release
	// to a timer — the script keeps running, which is what an interactive
	// program wants. The timer only fires while the script is still running,
	// so a script that ends right after the call cuts the chord short.
	//
	// play_chord_wait instead waits for the whole chord here and releases the
	// notes before returning. The obvious function to use for a test or for
	// playing a progression step by step.
	//
	// Both return false when there is no open port or the array is empty,
	// with midi_last_error() saying which.
	bool play_chord(CScriptArray& notes);
	bool play_chord_wait(CScriptArray& notes);

	// One note, with the same split: play_note returns at once and schedules
	// the release, play_note_wait returns when the note has finished.
	bool play_note(const midi_note& note);
	bool play_note_wait(const midi_note& note);

	// A duration written at this class's tempo: music.duration(1.0, MUSIC_BEATS)
	// is one beat at whatever music.tempo currently is, so the tempo does not
	// have to be repeated on every note.
	midi_duration duration(double amount, int unit) const;
	// Beats per minute, used when a note's length is written in beats, bars or
	// ticks. Milliseconds ignore it. Defaults to 120.
	double tempo;

	// Releases everything the high level layer has sounding right now, and
	// returns how many notes that was.
	unsigned int stop_all_notes();
	// How many notes of the high level layer are sounding right now.
	unsigned int get_active_notes() const;

	// Plays the notes according to a named pattern: "chord", "spread",
	// "arpeggio", "quick", "fast", "sequence", "repeat", "strum".
	//
	// Both wait: the sequence of note ons and releases is run to its end
	// before returning, so the script continues exactly when the last note
	// has been released. A script that wants to do something while a pattern
	// plays should call the non waiting variants or drive the raw send_*
	// calls from its own clock.
	bool play_midi_chord(CScriptArray& notes, const std::string& pattern);
	bool play_midi_chord_wait(CScriptArray& notes, const std::string& pattern);

	// Plays the notes one after another, each for its own length. Blocking,
	// so a scale sounds as a scale and not as a chord.
	bool play_sequence(CScriptArray& notes);

	// Sends one message. The three bytes are the raw MIDI bytes, so
	// send(0x90, 60, 100) is a note-on on channel 1.
	bool send(unsigned int status, unsigned int data1, unsigned int data2);
	// Sends a message whose bytes were packed into one integer, low byte first.
	bool send_packed(unsigned int packed);
	void send_note_on(unsigned int channel, unsigned int note, unsigned int velocity);
	void send_note_off(unsigned int channel, unsigned int note, unsigned int velocity = 0);
	void send_control_change(unsigned int channel, unsigned int controller, unsigned int value);
	void send_program_change(unsigned int channel, unsigned int program);
	void send_pitch_bend(unsigned int channel, unsigned int value); // 0..16383, 8192 is centre
	void send_aftertouch(unsigned int channel, unsigned int note, unsigned int pressure);
	void send_channel_pressure(unsigned int channel, unsigned int pressure);
	void send_sysex(const std::string& data);
	// Sends one message the way it arrived, moved by `semitones`, and returns
	// what it did. Only note numbers move: every other kind of message is sent
	// unchanged, because a pedal or a wheel is the performer telling the
	// instrument something and shifting it would be a different feature.
	//
	// A note that would land outside 0..127 is dropped rather than wrapped, and
	// reported as TRANSPOSE_OUT_OF_RANGE. Wrapping would turn the top of the
	// keyboard into the bottom, which sounds like a fault because it is one.
	//
	// The message is the one midi_input::next_message fills, so the shape of an
	// echo is: while (in.next_message(m)) out.send_transposed(m, 12);
	//
	// A note-on with velocity 0 goes out as a note-off: a keyboard that
	// releases its keys that way would otherwise leave every one of them
	// sounding for the rest of the session.
	int send_transposed(const midi_message& message, int semitones);
	// Stops every sounding note on all 16 channels.
	void all_notes_off();
	void reset();

	bool is_virtual_port() const { return virtual_port; }
	void set_virtual_port(bool value) { virtual_port = value; }

private:
	void* midi_out; // RtMidiOut*
	int port_index;
	bool virtual_port;
	// Send helper shared by the high level functions; builds a note on or a
	// note off from a midi_note without going through the script.
	void note_on(const midi_note& note);
	void note_off(const midi_note& note);
	// Releases one note and forgets it, so stop_all_notes() does not send a
	// second note off for the same key.
	void release_one(const midi_note& note);
	// The chord the high level layer is currently holding sounding.
	std::vector<midi_note> sounding;
};

// The names nvgt registers with Angelscript, so the script can reach this
// file's own facts. They are declared here because nvmidi.cpp is one
// translation unit and defines them far below its first use, and they are
// deliberately NOT exported: a script reaches them through the engine, which
// calls them in-process, and nothing outside the library has any use for a
// mangled std::string-returning name. The byte interface below is what an
// outside caller such as the ctypes test uses, and `midi_export_api_name_byte`
// is how it reaches this same string without going through the engine.
std::string midi_api_name();
std::string midi_input_port_name(unsigned int port);
std::string midi_output_port_name(unsigned int port);

// Free functions registered with Angelscript.
//
// These carry NVGT_PLUGIN_EXPORT because a script is not the only caller any
// more. nvgt is one host of this file and Python is another: scripts/test.py
// loads the library with ctypes and drives the same layer nvgt drives, so it
// can be told "press a key now" and report what arrived, on a machine where
// nvgt itself is not installed. That halves what has to be true before a
// listening test means anything.
//
// Export is not an earlier idea rejected: on ELF the free functions were
// already reachable, because only extern "C" declarations give visibility
// ("default") to a symbol - these are C++ mangled and stayed in .dynsym. The
// macro is what makes the same call work on Windows, where nothing is exported
// unless it is named. Without it the compiled .dll exports nvgt_plugin and
// nvgt_plugin_version and nothing else, and a ctypes test against a real
// Windows build fails on a missing attribute with every other check passing.
//
// The names below are also given a C entry point rather than only a mangled
// one, so a loader does not have to reproduce the C++ types to name them.
#define NVGT_PLUGIN_EXPORT extern "C" NVGT_PLUGIN_EXPORT_MACRO

NVGT_PLUGIN_EXPORT unsigned int midi_input_port_count();
NVGT_PLUGIN_EXPORT unsigned int midi_output_port_count();
NVGT_PLUGIN_EXPORT bool midi_supports_virtual_ports();

// The same three facts, as bytes, for a caller that cannot receive a
// std::string across the ABI - a ctypes test, or any other host that was not
// compiled with the same standard library. The pattern is the one this plugin
// already uses for script arrays: byte_count() asks the length, byte(i) asks
// for one character, and a name that does not exist answers count 0 rather
// than a negative, so a caller never has to distinguish "no such port" from a
// failure. Both are NUL safe: the count is the real length, so an embedded NUL
// does not silently truncate what the caller reads.
NVGT_PLUGIN_EXPORT unsigned int midi_export_output_port_name_byte_count(unsigned int port);
NVGT_PLUGIN_EXPORT int midi_export_output_port_name_byte(unsigned int port, unsigned int index);
NVGT_PLUGIN_EXPORT unsigned int midi_export_input_port_name_byte_count(unsigned int port);
NVGT_PLUGIN_EXPORT int midi_export_input_port_name_byte(unsigned int port, unsigned int index);
NVGT_PLUGIN_EXPORT unsigned int midi_export_api_name_byte_count();
NVGT_PLUGIN_EXPORT int midi_export_api_name_byte(unsigned int index);

// This file's own name for its midi backend, "alsa", "winmm" or "dummy".
// The script sees it through midi_api_name(); this is the same string without
// going through the engine, so a test can refuse a build that cannot play a
// note before it opens anything.
NVGT_PLUGIN_EXPORT unsigned int midi_export_backend_byte_count();
NVGT_PLUGIN_EXPORT int midi_export_backend_byte(unsigned int index);

// Whether that backend can create a virtual port at all. False on Windows and
// on the dummy build, where RtMidi's openVirtualPort does nothing at all.
bool midi_supports_virtual_ports();

// Case insensitive search for a port whose name contains something.
// Returns the index of the first match, or -1 when nothing matches. The
// whole search is done on lower cased copies, so "nord" finds "Nord Piano 6".
int midi_find_input_port(const std::string& substring);
int midi_find_output_port(const std::string& substring);
// Return a fresh array<string>; the script owns the reference.
CScriptArray* midi_input_port_names();
CScriptArray* midi_output_port_names();

// Factories: the classes are reference types without reference counting, so
// the script creates them through these and the plugin keeps ownership.
midi_input* midi_input_create();
midi_output* midi_output_create();

// Message of the most recent failure, empty when the last call succeeded.
std::string midi_last_error();

// Builds a human readable name for a message, e.g. "note on, channel 1, note 60, velocity 100".
std::string midi_message_name(const midi_message& m);

// ---------------------------------------------------------------------------
// Port configuration
// ---------------------------------------------------------------------------
//
// Reads a small text file that says which port to look for. The format is one
// "key = value" per line, # starts a comment:
//
//     match = nord          # substring of the port name, case insensitive
//     port = 0              # fall back to this index
//
// Both keys are optional. When the file is missing or unreadable the config
// keeps its defaults: match "nord", port 0. The examples ship such a file
// next to the script, so someone testing another keyboard edits the file
// instead of the code.
class midi_config {
public:
	midi_config();

	// Loads path, replacing the current values. Returns false when the file
	// cannot be read, with the reason in midi_last_error(); the defaults
	// stay in place. A key that is not understood is reported and skipped,
	// so one typo does not throw the whole config away.
	bool load(const std::string& path);
	// Same, but a missing file is not an error: the defaults simply stand.
	bool load_if_present(const std::string& path);

	// Looks for the configured substring, case insensitively, and falls back
	// to the configured index when nothing matches. Returns the index of the
	// port to open, or -1 when neither the name nor the index finds anything.
	int find_input_port() const;
	int find_output_port() const;

	// The index the search ended on last time, -1 when it found nothing.
	int get_last_port() const { return last_port; }
	// "Nord Piano 6" when the search matched by name, "port 0 (fallback)"
	// when it fell back to the index. Meant to be spoken by a screen reader.
	std::string describe() const;
	int describe_byte_count() const;
	int describe_byte(unsigned int index) const;
	std::string get_path() const { return path; }
	int get_path_byte_count() const { return (int)path.size(); }
	int get_path_byte(unsigned int index) const {
		return index < path.size() ? (int)(unsigned char)path[index] : -1;
	}

	// The substring searched for, "nord" unless the file says otherwise.
	std::string match;
	int port; // index used when the substring finds nothing

private:
	int pick(bool input);
	std::string path;
	int last_port;
};

midi_config* midi_config_create();

// Turns a note name such as "C4", "F#3" or "Bb5" into a MIDI note number.
// Returns -1 when the name is not understood.
int midi_note_number(const std::string& name);

// ---------------------------------------------------------------------------
// High level layer
// ---------------------------------------------------------------------------
//
// The playing functions live on midi_input and midi_output and take arrays of
// notes. Angelscript declares those as array<midi_note@>@: the elements are
// handles to midi_note written by the script (midi_note(60, 100) is enough),
// and the array is owned by the script while the call runs. The plugin never
// keeps a reference to it and never touches it from another thread.


// Trampoline handed to RtMidi; defined in nvmidi.cpp.
void midi_input_callback(double delta, std::vector<unsigned char>* message, void* user_data);

// What one pass of registration did, returned rather than logged: the two
// numbers answer questions a log line cannot be counted for by a caller, and
// plugin_main prints them. `calls` is every registration call made and
// `refused` is how many the engine rejected, whatever the code - a refusal
// count with no denominator reads as "22 of everything was refused" and as
// "22 of 900 was", which are different findings.
struct registration_result {
	int calls;
	int refused;
	int registered;	// plugin types the engine answered to their own name afterwards
};
registration_result register_nvmidi(asIScriptEngine* engine);





