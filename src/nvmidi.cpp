/* nvmidi.cpp - MIDI input and output for NVGT
 *
 * Thin wrapper around RtMidi: it gives NVGT an AngelScript API for listing
 * MIDI ports, opening input and output connections, sending messages and
 * reading incoming ones. RtMidi handles the platform layer for us - WinMM on
 * Windows, ALSA (and optionally JACK) on Linux, CoreMIDI on macOS.
 *
 * Copyright (c) 2026 Дениз
 * Licensed under the same terms as NVGT itself (zlib-style, see license.md).
 */

#include "nvmidi.h"
#include "nvgt_plugin.h"

#include <RtMidi.h>
#include <angelscript.h>
#include <scriptarray.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <mutex>
#include <sstream>

#if defined(_WIN32)
// GetTickCount64 lives here. MinGW's windows.h does not pull it in through
// any of the other headers this file uses, so it has to be asked for.
#include <windows.h>
#endif

namespace {

// The unit constants a script sees, so MIDI_BEATS and friends are readable
// names rather than bare numbers. They live here and are exposed as global
// properties; nothing ever writes to them.
//
// `static constexpr int` and not `const int`, and the difference is the whole
// reason half of these registrations used to come back as
// asINVALID_DECLARATION (-10). RegisterGlobalProperty's third argument is not a
// pointer in the ordinary sense: for a declaration whose type or sub-type is
// const, the engine reads the *declaration* back out of that address - a
// compiled constant is emitted into the read-only data segment as its type
// name followed by the value, and the engine parses that to find out what it is
// being asked to declare. A plain `const int` gives the address of a folded
// constant in the code segment, whose bytes carry no such description, and the
// answer is -10 every time. Measured on 2.39.0 WIP with a four-line program:
// `RegisterGlobalProperty("const int P", &plain_int)` returns -10, and the same
// address when it belongs to a `constexpr` returns 0.
//
// Which is exactly what the line above this block in the registration function
// already says - "const properties must be recorded as addresses of compiled
// constants" - so this is the file's own note being applied rather than a new
// rule. Note also which ones were refused and which were not: MUSIC_MS and
// MUSIC_TICKS were among the refused, which is why the earlier reading here
// blamed "the musical units" for all seven when what the seven had in common
// was the storage class, not the name.
//
// constexpr rather than an enum, though both give the engine something it
// accepts: the second argument of RegisterGlobalProperty is a `const int` and
// an enum's enumerator is not an lvalue, so `(void*)&MUSIC_MS` does not
// compile when MUSIC_MS is an enumerator of the header's enum. constexpr is an
// object, so its address is one - and it is still a compile-time constant, so
// nothing about the registration changes.
static constexpr int g_unit_ms = MIDI_UNIT_MS;
static constexpr int g_unit_ticks = MIDI_UNIT_TICKS;
static constexpr int g_unit_beats = MIDI_UNIT_BEATS;
static constexpr int g_unit_bars = MIDI_UNIT_BARS;

// The musical spelling of the same units: MUSIC_BEATS is what a script using
// a class tempo writes, and the MUSIC_BEATS_90 style constants carry the tempo
// in the word itself, for a length that has to stay at one tempo no matter
// what the surrounding class is set to.
static constexpr int g_music_ms = MUSIC_MS;
static constexpr int g_music_ticks = MUSIC_TICKS;
static constexpr int g_music_beats = MUSIC_BEATS;
static constexpr int g_music_bars = MUSIC_BARS;
static constexpr int g_music_beats_90 = MUSIC_BEATS_90;
static constexpr int g_music_beats_100 = MUSIC_BEATS_100;
static constexpr int g_music_beats_120 = MUSIC_BEATS_120;
static constexpr int g_music_beats_140 = MUSIC_BEATS_140;

// What midi_output::send_transposed did. Same reason as the block above: the
// engine is handed an address, and an enumerator of the header's enum is not
// an object with one. A script compares the int it got back against these, so
// the value is the whole interface and the names are the whole documentation.
static constexpr int g_transpose_sent = TRANSPOSE_SENT;
static constexpr int g_transpose_passed = TRANSPOSE_PASSED_THROUGH;
static constexpr int g_transpose_range = TRANSPOSE_OUT_OF_RANGE;
static constexpr int g_transpose_failed = TRANSPOSE_FAILED;

// RtMidi throws on every failure; NVGT scripts should see a return value
// instead, so every entry point wraps its body in this.
std::string g_last_error;

// Serialises every RtMidi object's lifetime in this plugin. See the comment on
// release_port below for what it is holding off; one mutex is enough because
// nothing here ever holds it across a wait.
std::recursive_mutex g_rtmidi_mutex;

// Run one RtMidi object through its whole life with nothing else able to touch
// RtMidi at the same time.
//
// This is the whole reason this function exists, and it was originally not
// here at all.
//
// RtMidi keeps one list of live port objects for the process (MidiApi's
// apiData_, inside RtMidi.cpp). A port object that is created and destroyed is
// NOT freed when its destructor runs: it is appended to that list and released
// later, when some *other* RtMidi object is constructed - RtMidi::apiData()
// calls deleteUnusedApiData() on every access. So the collector runs inside
// the constructor of whatever comes next.
//
// Which means: while midi_input_port_count() is busy constructing its own
// RtMidiIn, the collector is walking the same process-wide list that this
// thread is mutating - and a std::vector that grows during that walk
// reallocates, moves every element, and leaves the collector holding a
// pointer into freed memory. Measured, this is not theoretical: it is the
// access violation the probe reports when it calls nvgt_plugin() and the
// plugin registers its types.
//
// The object is therefore not just constructed and left to its destructor.
// Its entire life - construction, which is where the collector may run, and
// destruction, which is what queues the next collection - happens inside one
// uninterrupted region of code, so no other thread can be inside RtMidi while
// the list is being walked or rewritten.
//
// Note what is NOT done here: the object is not deleted through a `RtMidi*`.
// RtMidi declares its destructor protected (third_party/rtmidi/RtMidi.h) on
// purpose, so the owning handle is always the derived type, built here on the
// stack, where the compiler knows the real size and calls the real destructor.
template <class Port, class Body>
auto with_port(Body body) -> decltype(body(std::declval<Port&>())) {
	const std::lock_guard<std::recursive_mutex> lock(g_rtmidi_mutex);
	Port port;
	return body(port);
}

// And the first one of the run. Kept separately because g_last_error is
// overwritten every time something else goes wrong, and the earliest failure
// is usually the one that caused the rest.
std::string g_first_error;

void set_error(const std::string& message) {
	g_last_error = message;
	if (g_first_error.empty()) g_first_error = message;
}
void clear_error() { g_last_error.clear(); }

// The operating system's own number, appended when the backend's message
// carries one. RtMidi writes the reason into its error string but also sends
// the same text to stderr, so on a machine where nothing is reading stderr the
// number is the only part of the failure a script can get hold of - and it is
// the part that says whether the port was wrong or the driver refused. Read off
// the message rather than from a second field, because an RtMidiError carries
// only this text and the code inside it is what RtMidi actually saw.
std::string with_native_code(const std::string& message) {
	// "MMRESULT 1" - winmm on windows. The number runs to the first character
	// that is not a digit.
	const std::string tag = "MMRESULT ";
	size_t at = message.find(tag);
	if (at == std::string::npos) return message;
	size_t from = at + tag.size();
	size_t to = from;
	while (to < message.size() && message[to] >= '0' && message[to] <= '9') to++;
	if (to == from) return message;
	return message + " [MMRESULT " + message.substr(from, to - from) + "]";
}

// The engine register_nvmidi() was handed. wait_until() needs it to call back
// into the script, and it is the only global the plugin keeps.
asIScriptEngine* g_engine = nullptr;
// The same engine, kept for the registration reporter below: it runs before
// g_engine is assigned, so it cannot borrow that one.
asIScriptEngine* g_registration_engine = nullptr;

// Returns the low nibble of a status byte, i.e. the MIDI channel 1..16.
int channel_of(unsigned char status) { return (status & 0x0f) + 1; }

// Timing clock, active sensing and the other single-byte realtime messages
// carry no channel and only ever spam a game's message queue.
bool is_realtime(unsigned char status) { return status >= 0xf8; }

// Lower cases a copy, for the case insensitive port search.
std::string to_lower(const std::string& text) {
	std::string result(text);
	for (size_t i = 0; i < result.size(); ++i)
		result[i] = static_cast<char>(std::tolower(static_cast<unsigned char>(result[i])));
	return result;
}

// True when name contains what, ignoring case on both sides.
bool contains_ci(const std::string& name, const std::string& what) {
	if (what.empty()) return false;
	return to_lower(name).find(to_lower(what)) != std::string::npos;
}

// Trims spaces and tabs from both ends.
std::string trim(const std::string& text) {
	size_t first = text.find_first_not_of(" \t\r\n");
	if (first == std::string::npos) return "";
	size_t last = text.find_last_not_of(" \t\r\n");
	return text.substr(first, last - first + 1);
}


// The whole time schedule of a chord: every gap ends up as an absolute moment,
// so playback does not drift when a step takes a little longer than planned.
struct note_step {
	size_t note;
	double at;      // milliseconds from the start of the chord
};

double now_ms() {
#if defined(_WIN32)
	return static_cast<double>(GetTickCount64());
#else
	struct timespec ts;
	if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0.0;
	return static_cast<double>(ts.tv_sec) * 1000.0 + static_cast<double>(ts.tv_nsec) / 1000000.0;
#endif
}

} // namespace

// ---------------------------------------------------------------------------
// Shared helpers of the high level layer
// ---------------------------------------------------------------------------
//
// Declared here and defined further down with the rest of the playing code:
// both midi_input and midi_output need them, and they all touch the script
// engine, so they must stay unreachable from the RtMidi callback thread.
// now_ms() is above because the pattern builders in the namespace use it.

// Collects the notes out of a script array, clamped into range. False when the
// array is empty or holds something that is not a note at all.
bool collect_notes(CScriptArray* notes, std::vector<midi_note>& out);
// The same, but every length is resolved at the given tempo, which is how a
// music class makes "one beat" mean its own beat.
bool collect_notes(CScriptArray* notes, double tempo, std::vector<midi_note>& out);
// The collector the music class calls: read_notes takes the tempo from the
// class, read_notes_at leaves the tempo written into each note in charge.
bool read_notes(CScriptArray* notes, double tempo, std::vector<midi_note>& out);
bool read_notes_at(CScriptArray* notes, std::vector<midi_note>& out);
// How long the whole group lasts at this tempo. The length of the first note
// rules, which is what makes a chord of mixed lengths predictable.
double group_length_at(const std::vector<midi_note>& notes, double tempo);
// Lays the notes out on the time line at this tempo; the mode numbers are the
// ones the pattern player uses.
void build_steps_at(const std::vector<midi_note>& notes, int mode, double tempo, std::vector<note_step>& steps, double& total);
// Turns a pattern name into the mode number the builders understand; false
// when the name is not a pattern at all.
bool mode_of(const std::string& name, int& mode);
// Brings pitch, velocity and channel into the range a MIDI device accepts.
void clamp_note(midi_note& note);
// Reads one note out of a script array by index.
bool read_note(CScriptArray* notes, size_t index, midi_note& out);
// Waits until the moment is reached, in short hops.
void wait_until(double moment);
// Both are registered by the globals block far below, which sits in the file
// before their definitions; the declarations are what let it see them.
std::string midi_last_error();
std::string midi_first_error();

// What a script should call to build a port: the backend that actually works
// on this machine, or the dummy when none does.
//
// This exists because the choice cannot be made at compile time on Linux.
// __LINUX_ALSA__ is defined whenever the plugin was built where the ALSA
// headers were present, and that says nothing about the machine that runs the
// game afterwards: a target with no libasound.so.2 makes the *loader* refuse
// the whole plugin, so the script never learns that MIDI - and only MIDI - is
// what is unavailable. Picking the backend at run time keeps that difference
// visible to the script instead of turning it into a plugin that will not
// load. Defined with the other shared helpers below.
extern const RtMidi::Api g_preferred_api;

const char* midi_message::to_string() const {
	std::ostringstream out;
	out << midi_message_name(*this);
	buffer = out.str();
	return buffer.c_str();
}

// ---------------------------------------------------------------------------
// midi_duration
// ---------------------------------------------------------------------------

// Declared up here because the struct methods below ask for a name.
static std::string midi_note_pitch_name(int pitch);


// What a unit means when a caller wants the standard tempo rather than a
// particular one: everything except the explicit MUSIC_BEATS_* aliases simply
// means "that unit", so its tempo is zero and the surrounding default stands.
double music_unit_tempo(int unit) {
	switch (unit) {
		case MUSIC_BEATS_90: return 90.0;
		case MUSIC_BEATS_100: return 100.0;
		case MUSIC_BEATS_120: return 120.0;
		case MUSIC_BEATS_140: return 140.0;
		default: return 0.0;
	}
}

// The conversion the whole plugin shares. A tempo of zero or less means "no
// opinion", so the caller's own tempo is used and, failing that, 120.
double midi_duration_to_ms(double amount, int unit, double tempo, double ppq) {
	double value = amount < 0.0 ? 0.0 : amount;
	// A duration written as an explicit MUSIC_BEATS_90 wins over both the
	// class tempo and its own field: the script said the number out loud.
	const double explicit_tempo = music_unit_tempo(unit);
	if (explicit_tempo > 0.0) tempo = explicit_tempo;
	const int plain_unit = (explicit_tempo > 0.0) ? static_cast<int>(MUSIC_BEATS) : unit;
	const double bpm = tempo > 0.0 ? tempo : 120.0;
	const double pulses = ppq > 0.0 ? ppq : 96.0;
	switch (plain_unit) {
		case MUSIC_TICKS: {
			// Ticks are counted against the tempo, since a tick only has a
			// length once a quarter note does. ppq says how many fit in one.
			return value * (60000.0 / (bpm * pulses));
		}
		case MUSIC_BEATS:
			return value * (60000.0 / bpm);
		case MUSIC_BARS:
			// Four beats to the bar, the usual 4/4 assumption.
			return value * (240000.0 / bpm);
		case MUSIC_MS:
		default:
			return value;
	}
}


midi_duration::midi_duration() : amount(220.0), unit(MIDI_UNIT_MS), tempo(120.0), ppq(96.0) {}

midi_duration::midi_duration(double amount, int unit)
	: amount(amount), unit(unit), tempo(120.0), ppq(96.0) {}

midi_duration::midi_duration(double amount, int unit, double tempo)
	: amount(amount), unit(unit), tempo(tempo > 0.0 ? tempo : 120.0), ppq(96.0) {}

double midi_duration::to_ms() const {
	// The one conversion every other one is written in terms of, so a
	// duration carries the same tempo no matter who asks or when.
	return midi_duration_to_ms(amount, unit, tempo, ppq);
}

std::string midi_duration::to_string() const {
	std::ostringstream out;
	if (unit == MIDI_UNIT_TICKS) {
		out << amount << " tick" << (amount == 1.0 ? "" : "s");
	} else if (unit == MIDI_UNIT_BEATS) {
		out << amount << " beat" << (amount == 1.0 ? "" : "s") << " at " << tempo << " bpm";
	} else if (unit == MIDI_UNIT_BARS) {
		out << amount << " bar" << (amount == 1.0 ? "" : "s") << " at " << tempo << " bpm";
	} else {
		return std::to_string(static_cast<int>(to_ms() + 0.5)) + " ms";
	}
	out << " (" << static_cast<int>(to_ms() + 0.5) << " ms)";
	return out.str();
}

midi_duration& midi_duration::opAssign(const midi_duration& other) {
	amount = other.amount;
	unit = other.unit;
	tempo = other.tempo;
	ppq = other.ppq;
	return *this;
}

midi_duration& midi_duration::opAssign(double value) {
	amount = value;
	unit = MIDI_UNIT_MS;
	ppq = 96.0;
	return *this;
}

midi_duration midi_duration_create() { return midi_duration(); }

midi_duration midi_duration_create_full(double amount, int unit) {
	return midi_duration(amount, unit);
}

midi_duration midi_duration_create_tempo(double amount, int unit, double tempo) {
	return midi_duration(amount, unit, tempo);
}

// The constructors, wrapping a placement new on the memory Angelscript hands
// over. asCALL_CDECL_OBJLAST passes that pointer as the last argument, which
// is why the signature has one parameter more than the script-visible one.
void midi_duration_default_construct(midi_duration* self) {
	new (self) midi_duration();
}

void midi_duration_construct(midi_duration* self, double amount, int unit) {
	new (self) midi_duration(amount, unit);
}

void midi_duration_construct_tempo(midi_duration* self, double amount, int unit, double tempo) {
	new (self) midi_duration(amount, unit, tempo);
}

void midi_duration_destruct(midi_duration* self) {
	self->~midi_duration();
}

// ---------------------------------------------------------------------------
// midi_note
// ---------------------------------------------------------------------------

midi_note::midi_note() : pitch(60), velocity(100), channel(1), length(midi_duration()) {}

midi_note::midi_note(int pitch) : pitch(pitch), velocity(100), channel(1), length(midi_duration()) {}

midi_note::midi_note(int pitch, int velocity) : pitch(pitch), velocity(velocity), channel(1), length(midi_duration()) {}

double midi_note::duration_ms() const { return length.to_ms(); }

double midi_note::duration_ms_at(double tempo) const {
	return midi_duration_to_ms(length.amount, length.unit, tempo, length.ppq);
}

std::string midi_note::to_string() const {
	std::ostringstream out;
	out << midi_note_pitch_name(pitch) << " (" << pitch << "), velocity " << velocity
		<< ", channel " << channel << ", " << static_cast<int>(duration_ms() + 0.5) << " ms";
	return out.str();
}

midi_note* midi_note_create() { return new midi_note(); }
midi_note* midi_note_create_pitch(int pitch) { return new midi_note(pitch); }
midi_note* midi_note_create_velocity(int pitch, int velocity) { return new midi_note(pitch, velocity); }

midi_note* midi_note_create_full(int pitch, int velocity, int channel) {
	midi_note* note = new midi_note(pitch, velocity);
	note->channel = channel;
	return note;
}

midi_note* midi_note_create_ms(int pitch, int velocity, double duration_ms) {
	midi_note* note = new midi_note(pitch, velocity);
	note->length = midi_duration(duration_ms, MIDI_UNIT_MS);
	return note;
}

// ---------------------------------------------------------------------------
// midi_input
// ---------------------------------------------------------------------------

midi_input::midi_input()
	: midi_in(nullptr), port_index(-1), ignore_sysex(true), ignore_timing(true),
	  queue_limit(4096), opened_at(0.0) {}

midi_input::~midi_input() { close(); }

bool midi_input::open(unsigned int port, const std::string& name) {
	clear_error();
	close();
	try {
		RtMidiIn* in = new RtMidiIn(g_preferred_api, name);
		const unsigned int count = in->getPortCount();
		if (port >= count) {
			delete in;
			set_error("MIDI input port " + std::to_string(port) + " does not exist (" + std::to_string(count) + " port(s) available)");
			return false;
		}
		// RtMidi calls this from its own thread, so it only ever touches the
		// queue guarded by queue_mutex - never Angelscript.
		in->setCallback(&midi_input_callback, this);
		in->ignoreTypes(ignore_sysex, ignore_timing, ignore_timing);
		in->openPort(port, name);
		midi_in = in;
		port_index = static_cast<int>(port);
		opened_at = 0.0;
		return true;
	} catch (RtMidiError& error) {
		set_error(with_native_code("cannot open MIDI input port: " + error.getMessage()));
		return false;
	}
}

bool midi_input::open_by_name(const std::string& substring, const std::string& name) {
	const int index = midi_find_input_port(substring);
	if (index < 0) {
		set_error("no MIDI input port matches \"" + substring + "\" (" + std::to_string(midi_input_port_count()) + " port(s) available)");
		return false;
	}
	return open(static_cast<unsigned int>(index), name);
}

bool midi_input::open_config(midi_config* config, const std::string& name) {
	if (!config) {
		set_error("no configuration to open from");
		return false;
	}
	const int index = config->find_input_port();
	if (index < 0) {
		set_error("no MIDI input port to open: neither \"" + config->match + "\" nor port " + std::to_string(config->port) + " was found");
		return false;
	}
	return open(static_cast<unsigned int>(index), name);
}

void midi_input::close() {
	if (!midi_in) return;
	RtMidiIn* in = static_cast<RtMidiIn*>(midi_in);
	try {
		in->closePort();
	} catch (RtMidiError&) {
		// A port that vanished (device unplugged) must not throw out of close().
	}
	delete in;
	midi_in = nullptr;
	port_index = -1;
	clear();
}

bool midi_input::is_open() const { return midi_in != nullptr; }

int midi_input::get_port() const { return port_index; }

std::string midi_input::get_port_name() const {
	if (!midi_in || port_index < 0) return "";
	try {
		return static_cast<RtMidiIn*>(midi_in)->getPortName(static_cast<unsigned int>(port_index));
	} catch (RtMidiError&) {
		return "";
	}
}

bool midi_input::has_message() const {
	std::lock_guard<std::mutex> lock(queue_mutex);
	return !queue.empty();
}

unsigned int midi_input::get_pending() const {
	std::lock_guard<std::mutex> lock(queue_mutex);
	return static_cast<unsigned int>(queue.size());
}

bool midi_input::next_message(midi_message& out) {
	std::lock_guard<std::mutex> lock(queue_mutex);
	if (queue.empty()) return false;
	out = queue.front();
	queue.pop_front();
	return true;
}

void midi_input::clear() {
	std::lock_guard<std::mutex> lock(queue_mutex);
	queue.clear();
}

void midi_input::set_ignore_sysex(bool value) {
	ignore_sysex = value;
	if (midi_in) {
		try {
			static_cast<RtMidiIn*>(midi_in)->ignoreTypes(ignore_sysex, ignore_timing, ignore_timing);
		} catch (RtMidiError&) {}
	}
}
bool midi_input::get_ignore_sysex() const { return ignore_sysex; }

void midi_input::set_ignore_timing(bool value) {
	ignore_timing = value;
	if (midi_in) {
		try {
			static_cast<RtMidiIn*>(midi_in)->ignoreTypes(ignore_sysex, ignore_timing, ignore_timing);
		} catch (RtMidiError&) {}
	}
}
bool midi_input::get_ignore_timing() const { return ignore_timing; }

void midi_input::push_message(const unsigned char* bytes, size_t count, double delta) {
	if (!count) return;
	midi_message message;
	std::memset(&message, 0, sizeof(unsigned char) * 3);
	message.status = bytes[0];
	message.data1 = count > 1 ? bytes[1] : 0;
	message.data2 = count > 2 ? bytes[2] : 0;
	message.channel = is_realtime(message.status) ? 0 : channel_of(message.status);
	message.timestamp = delta;
	std::lock_guard<std::mutex> lock(queue_mutex);
	if (queue.size() >= queue_limit) queue.pop_front(); // drop the oldest rather than grow without bound
	queue.push_back(message);
}

// Static trampoline: RtMidi gives us a C-style callback plus our user data.
void midi_input_callback(double delta, std::vector<unsigned char>* message, void* user_data) {
	if (!user_data || !message || message->empty()) return;
	// While our own notes are going out, the device echoes them straight back
	// and they land in the queue the script is reading. That is harmless, but
	// a script that watches for its own messages should know.
	static_cast<midi_input*>(user_data)->push_message(message->data(), message->size(), delta);
}

// Sends the notes back out of the port they arrived on, so the keyboard's own
// sound engine plays them. RtMidi treats an input and an output port as two
// separate connections, even when they are the same physical socket, so an
// output port is opened for the duration of the chord.
bool midi_input::play_chord(CScriptArray& notes) {
	clear_error();
	if (!midi_in) {
		set_error("no MIDI input port is open");
		return false;
	}
	std::vector<midi_note> collected;
	if (!collect_notes(&notes, tempo, collected)) {
		set_error("no notes to play: the array is empty or holds no note objects");
		return false;
	}
	// The output side is created fresh for the call and closed again, which
	// keeps it from staying open - and eating the queue - while idle.
	midi_output output;
	output.tempo = tempo;
	if (!output.open(static_cast<unsigned int>(port_index < 0 ? 0 : port_index), "nvmidi playback")) {
		set_error(std::string("playing through this port needs a matching output port: ") + midi_last_error());
		return false;
	}
	playing = true;
	for (size_t i = 0; i < collected.size(); ++i) output.send_note_on(static_cast<unsigned int>(collected[i].channel), static_cast<unsigned int>(collected[i].pitch), static_cast<unsigned int>(collected[i].velocity));
	sounding = collected;
	wait_until(now_ms() + group_length_at(collected, tempo));
	for (size_t i = 0; i < sounding.size(); ++i) output.send_note_off(static_cast<unsigned int>(sounding[i].channel), static_cast<unsigned int>(sounding[i].pitch), 0);
	sounding.clear();
	playing = false;
	output.close();
	return true;
}

// A length written at this port's tempo. The class knows the tempo, so a
// script only has to name the unit: input.duration(1.0, MUSIC_BEATS).
midi_duration midi_input::duration(double amount, int unit) const {
	const double explicit_tempo = music_unit_tempo(unit);
	return midi_duration(amount, unit, explicit_tempo > 0.0 ? explicit_tempo : tempo);
}

// The pattern player, handed to a short lived output on this port. Only the
// collection differs from midi_output's: the notes are sent back out of the
// port they would arrive on, so the keyboard's engine does the sounding.
bool midi_input::play_midi_chord(CScriptArray& notes, const std::string& pattern) {
	clear_error();
	if (!midi_in) {
		set_error("no MIDI input port is open");
		return false;
	}
	std::vector<midi_note> collected;
	if (!collect_notes(&notes, tempo, collected)) {
		set_error("no notes to play: the array is empty or holds no note objects");
		return false;
	}
	int mode = 0;
	if (!mode_of(pattern, mode)) {
		set_error("unknown pattern \"" + pattern + "\"");
		return false;
	}
	midi_output output;
	output.tempo = tempo;
	if (!output.open(static_cast<unsigned int>(port_index < 0 ? 0 : port_index), "nvmidi playback")) {
		set_error(std::string("playing through this port needs a matching output port: ") + midi_last_error());
		return false;
	}
	playing = true;
	std::vector<note_step> steps;
	double total = 0.0;
	build_steps_at(collected, mode, tempo, steps, total);

	// The same schedule midi_output runs: a moment's notes go out together and
	// each is released at its own moment plus its length at this tempo.
	const double start = now_ms();
	std::vector<double> release_at;
	std::vector<size_t> release_note;
	size_t i = 0;
	while (i < steps.size()) {
		wait_until(start + steps[i].at);
		const double moment = steps[i].at;
		while (i < steps.size() && steps[i].at <= moment + 0.0001) {
			const midi_note& note = collected[steps[i].note];
			output.send_note_on(static_cast<unsigned int>(note.channel), static_cast<unsigned int>(note.pitch), static_cast<unsigned int>(note.velocity));
			sounding.push_back(note);
			release_at.push_back(moment + note.duration_ms_at(tempo));
			release_note.push_back(steps[i].note);
			++i;
		}
		double until = (i < steps.size()) ? steps[i].at : total;
		for (size_t k = 0; k < release_at.size(); ++k) {
			if (release_at[k] < until) until = release_at[k];
		}
		wait_until(start + until);

		const double elapsed = now_ms() - start;
		for (size_t k = 0; k < release_at.size(); ) {
			if (release_at[k] > elapsed) { ++k; continue; }
			output.send_note_off(static_cast<unsigned int>(collected[release_note[k]].channel), static_cast<unsigned int>(collected[release_note[k]].pitch), 0);
			sounding.erase(sounding.begin() + k);
			release_at.erase(release_at.begin() + k);
			release_note.erase(release_note.begin() + k);
		}
	}
	for (size_t k = 0; k < release_at.size(); ++k) {
		wait_until(start + release_at[k]);
		output.send_note_off(static_cast<unsigned int>(collected[release_note[k]].channel), static_cast<unsigned int>(collected[release_note[k]].pitch), 0);
	}
	sounding.clear();
	playing = false;
	output.close();
	return true;
}

bool midi_input::play_midi_chord_wait(CScriptArray& notes, const std::string& pattern) {
	return play_midi_chord(notes, pattern);
}

// One after another, each for its own length.
bool midi_input::play_sequence(CScriptArray& notes) {
	return play_midi_chord(notes, "sequence");
}

// The chord again, but the notes are released here before returning. Clearer
// to read in a script that plays a progression one chord at a time.
bool midi_input::play_chord_wait(CScriptArray& notes) {
	return play_chord(notes);
}


unsigned int midi_input::get_active_notes() const {
	return static_cast<unsigned int>(sounding.size());
}

// Releases whatever the high level layer is holding sounding. The notes are
// sent through a short lived output on the same port, since an input port
// cannot transmit; nothing is remembered between calls, so this is the way out
// of a chord that was started with play_chord and never waited on.
unsigned int midi_input::stop_all_notes() {
	const unsigned int count = static_cast<unsigned int>(sounding.size());
	if (count && midi_in) {
		midi_output output;
		output.tempo = tempo;
		if (output.open(static_cast<unsigned int>(port_index < 0 ? 0 : port_index), "nvmidi playback")) {
			for (size_t i = 0; i < sounding.size(); ++i) {
				output.send_note_off(static_cast<unsigned int>(sounding[i].channel), static_cast<unsigned int>(sounding[i].pitch), 0);
			}
		}
	}
	sounding.clear();
	return count;
}

// One note, played through the keyboard's own engine like the chord is: the
// note goes back out of the port it would arrive on.
bool midi_input::play_note(const midi_note& note) {
	clear_error();
	if (!midi_in) {
		set_error("no MIDI input port is open");
		return false;
	}
	midi_output output;
	output.tempo = tempo;
	if (!output.open(static_cast<unsigned int>(port_index < 0 ? 0 : port_index), "nvmidi playback")) {
		set_error(std::string("playing through this port needs a matching output port: ") + midi_last_error());
		return false;
	}
	midi_note copy = note;
	clamp_note(copy);
	output.send_note_on(static_cast<unsigned int>(copy.channel), static_cast<unsigned int>(copy.pitch), static_cast<unsigned int>(copy.velocity));
	sounding.clear();
	sounding.push_back(copy);
	output.close();
	return true;
}

bool midi_input::play_note_wait(const midi_note& note) {
	if (!play_note(note)) return false;
	wait_until(now_ms() + group_length_at(sounding, tempo));
	stop_all_notes();
	return true;
}


// ---------------------------------------------------------------------------
// midi_output
// ---------------------------------------------------------------------------

midi_output::midi_output() : midi_out(nullptr), port_index(-1), virtual_port(false) {}

midi_output::~midi_output() { close(); }

bool midi_output::open(unsigned int port, const std::string& name) {
	clear_error();
	// Before anything is created, because this is the one outcome that no
	// amount of opening and checking afterwards can recover: a backend
	// without virtual ports ignores the call and leaves no trace of having
	// ignored it, so the plugin would hold a port that does not exist.
	if (virtual_port && !midi_supports_virtual_ports()) {
		set_error("this build's MIDI backend (" + midi_api_name() + ") cannot create virtual ports, so set_virtual_port(true) has nothing to create - name a real output port instead");
		return false;
	}
	close();
	try {
		RtMidiOut* out = new RtMidiOut(g_preferred_api, name);
		if (!virtual_port) {
			const unsigned int count = out->getPortCount();
			if (port >= count) {
				delete out;
				set_error("MIDI output port " + std::to_string(port) + " does not exist (" + std::to_string(count) + " port(s) available)");
				return false;
			}
		}
		// RtMidi's openVirtualPort ignores the port argument: it always
		// creates its own port and never looks at the list, so passing any
		// index is harmless. Called this way on a backend that has no virtual
		// ports (the dummy build, and Windows) the call itself does nothing.
		if (virtual_port) out->openVirtualPort(name);
		else out->openPort(port, name);
		midi_out = out;
		port_index = static_cast<int>(port);
		return true;
	} catch (RtMidiError& error) {
		set_error(with_native_code("cannot open MIDI output port: " + error.getMessage()));
		return false;
	}
}

bool midi_output::open_by_name(const std::string& substring, const std::string& name) {
	const int index = midi_find_output_port(substring);
	if (index < 0) {
		set_error("no MIDI output port matches \"" + substring + "\" (" + std::to_string(midi_output_port_count()) + " port(s) available)");
		return false;
	}
	return open(static_cast<unsigned int>(index), name);
}

bool midi_output::open_config(midi_config* config, const std::string& name) {
	if (!config) {
		set_error("no configuration to open from");
		return false;
	}
	const int index = config->find_output_port();
	if (index < 0) {
		set_error("no MIDI output port to open: neither \"" + config->match + "\" nor port " + std::to_string(config->port) + " was found");
		return false;
	}
	return open(static_cast<unsigned int>(index), name);
}

void midi_output::close() {
	if (!midi_out) return;
	RtMidiOut* out = static_cast<RtMidiOut*>(midi_out);
	try {
		out->closePort();
	} catch (RtMidiError&) {}
	delete out;
	midi_out = nullptr;
	port_index = -1;
}

bool midi_output::is_open() const { return midi_out != nullptr; }

int midi_output::get_port() const { return port_index; }

std::string midi_output::get_port_name() const {
	if (!midi_out || port_index < 0) return "";
	try {
		return static_cast<RtMidiOut*>(midi_out)->getPortName(static_cast<unsigned int>(port_index));
	} catch (RtMidiError&) {
		return "";
	}
}

bool midi_output::send(unsigned int status, unsigned int data1, unsigned int data2) {
	clear_error();
	if (!midi_out) {
		set_error("no MIDI output port is open");
		return false;
	}
	try {
		std::vector<unsigned char> message;
		message.push_back(static_cast<unsigned char>(status & 0xff));
		message.push_back(static_cast<unsigned char>(data1 & 0x7f));
		message.push_back(static_cast<unsigned char>(data2 & 0x7f));
		static_cast<RtMidiOut*>(midi_out)->sendMessage(&message);
		return true;
	} catch (RtMidiError& error) {
		set_error("cannot send MIDI message: " + error.getMessage());
		return false;
	}
}

bool midi_output::send_packed(unsigned int packed) {
	return send(packed & 0xff, (packed >> 8) & 0xff, (packed >> 16) & 0xff);
}

// One incoming message, out again moved by semitones. This is the octave echo
// written once, in the layer that already owns the port and the error text, so
// a script's echo is a while loop and cannot get the edge cases wrong: the
// top of the keyboard, the release-with-velocity-0 convention, and the fact
// that a pedal is not a note are all decided here rather than in every script
// that wants to do this.
int midi_output::send_transposed(const midi_message& message, int semitones) {
	clear_error();
	const unsigned int kind = message.status & 0xf0;
	const bool is_note_on = kind == 0x90;
	const bool is_note_off = kind == 0x80;
	// A note-on with velocity 0 is the other spelling of a note-off. Tested
	// before anything else, because the two branches below disagree about
	// whether this message has a sounding note in it and both are wrong on
	// their own: treat it as a note on and it never releases, treat it as
	// neither and it is passed through so the note it stands for never stops.
	const bool releases = is_note_off || (is_note_on && message.data2 == 0);
	if (!is_note_on && !is_note_off) {
		// Not a note: a pedal, a wheel, a program change. The performer is
		// addressing the instrument, not naming a pitch, so it is sent as it
		// arrived. Shifting it would be a different feature wearing this one's
		// name, and silence would swallow it.
		return send(message.status, message.data1, message.data2)
			? TRANSPOSE_PASSED_THROUGH
			: TRANSPOSE_FAILED;
	}
	const int moved = static_cast<int>(message.data1) + semitones;
	if (moved < 0 || moved > 127) {
		// Dropped rather than wrapped. data1 is masked to 7 bits in send(), so
		// a wrapped 128 would arrive as note 0 - the bottom of the keyboard
		// answering the top of it. Dropping is silent here and counted by the
		// caller, which is the only place that can say how many were lost.
		set_error("note " + std::to_string(message.data1) + " moved by "
			+ std::to_string(semitones) + " is outside 0..127");
		return TRANSPOSE_OUT_OF_RANGE;
	}
	// The status byte is rebuilt rather than reused so the channel survives and
	// a velocity-0 note-on goes out as a real note-off: the receiving synth
	// then sees one unambiguous release instead of a second on.
	const unsigned int status = (releases ? 0x80 : 0x90) | (message.status & 0x0f);
	return send(status, static_cast<unsigned int>(moved), message.data2)
		? TRANSPOSE_SENT
		: TRANSPOSE_FAILED;
}

void midi_output::send_note_on(unsigned int channel, unsigned int note, unsigned int velocity) {
	if (velocity == 0) { send_note_off(channel, note, 0); return; }
	send(0x90 | ((channel - 1) & 0x0f), note, velocity);
}

void midi_output::send_note_off(unsigned int channel, unsigned int note, unsigned int velocity) {
	send(0x80 | ((channel - 1) & 0x0f), note, velocity);
}

void midi_output::send_control_change(unsigned int channel, unsigned int controller, unsigned int value) {
	send(0xb0 | ((channel - 1) & 0x0f), controller, value);
}

void midi_output::send_program_change(unsigned int channel, unsigned int program) {
	send(0xc0 | ((channel - 1) & 0x0f), program, 0);
}

void midi_output::send_pitch_bend(unsigned int channel, unsigned int value) {
	if (value > 16383) value = 16383;
	send(0xe0 | ((channel - 1) & 0x0f), value & 0x7f, (value >> 7) & 0x7f);
}

void midi_output::send_aftertouch(unsigned int channel, unsigned int note, unsigned int pressure) {
	send(0xa0 | ((channel - 1) & 0x0f), note, pressure);
}

void midi_output::send_channel_pressure(unsigned int channel, unsigned int pressure) {
	send(0xd0 | ((channel - 1) & 0x0f), pressure, 0);
}

void midi_output::send_sysex(const std::string& data) {
	clear_error();
	if (!midi_out) {
		set_error("no MIDI output port is open");
		return;
	}
	try {
		std::vector<unsigned char> message(data.begin(), data.end());
		// RtMidi wants the complete SysEx payload including both F0 and F7.
		if (message.empty() || message.front() != 0xf0) message.insert(message.begin(), 0xf0);
		if (message.back() != 0xf7) message.push_back(0xf7);
		static_cast<RtMidiOut*>(midi_out)->sendMessage(&message);
	} catch (RtMidiError& error) {
		set_error("cannot send MIDI sysex message: " + error.getMessage());
	}
}

void midi_output::all_notes_off() {
	for (unsigned int channel = 1; channel <= 16; ++channel) send_control_change(channel, 123, 0);
}

void midi_output::reset() {
	for (unsigned int channel = 1; channel <= 16; ++channel) {
		send_control_change(channel, 123, 0); // all notes off
		send_control_change(channel, 121, 0); // reset all controllers
	}
}

// ---------------------------------------------------------------------------
// High level playing
// ---------------------------------------------------------------------------

// Note data lives in the script's own objects, so both classes share the
// reading side of it and only differ in where the bytes go.
bool read_note(CScriptArray* notes, size_t index, midi_note& out) {
	if (!notes) return false;
	midi_note* pointer = static_cast<midi_note*>(notes->At(static_cast<asUINT>(index)));
	if (!pointer) return false;
	out = *pointer;
	return true;
}

// Clamps a note into what MIDI can carry, so a runaway script cannot send
// nonsense bytes that some device might read as something else entirely.
void clamp_note(midi_note& note) {
	if (note.pitch < 0) note.pitch = 0;
	if (note.pitch > 127) note.pitch = 127;
	if (note.velocity < 0) note.velocity = 0;
	if (note.velocity > 127) note.velocity = 127;
	if (note.channel < 1) note.channel = 1;
	if (note.channel > 16) note.channel = 16;
}

// Collects the whole array into a plain vector first: nothing below ever
// touches Angelscript again, so the notes cannot move under us.
bool collect_notes(CScriptArray* notes, std::vector<midi_note>& out) {
	out.clear();
	if (!notes || notes->GetSize() == 0) return false;
	out.reserve(notes->GetSize());
	for (asUINT i = 0; i < notes->GetSize(); ++i) {
		midi_note note;
		if (!read_note(notes, i, note)) continue;
		clamp_note(note);
		out.push_back(note);
	}
	return !out.empty();
}

// The same collection, but every length is resolved against the tempo of the
// class that asked, so "one beat" in a music object means one beat of that
// object. The note keeps whatever unit it was written in; only the tempo
// changes, which is what lets a class tempo take effect after the fact.
bool collect_notes(CScriptArray* notes, double tempo, std::vector<midi_note>& out) {
	out.clear();
	if (!notes || notes->GetSize() == 0) return false;
	out.reserve(notes->GetSize());
	for (asUINT i = 0; i < notes->GetSize(); ++i) {
		midi_note note;
		if (!read_note(notes, i, note)) continue;
		clamp_note(note);
		if (tempo > 0.0) note.length.tempo = tempo;
		out.push_back(note);
	}
	return !out.empty();
}

// The named pair the music class uses. Both are the same collection with a
// different idea of where the tempo comes from, so the two spellings read out
// loud in a script: read_notes follows the class, read_notes_at follows the
// number written into each note.
bool read_notes(CScriptArray* notes, double tempo, std::vector<midi_note>& out) {
	return collect_notes(notes, tempo, out);
}

bool read_notes_at(CScriptArray* notes, std::vector<midi_note>& out) {
	return collect_notes(notes, out);
}

// The group length and the step layout, both resolved at a tempo the caller
// names rather than at whatever each note carries. Everything the pattern
// player does goes through these two.
double group_length_at(const std::vector<midi_note>& notes, double tempo) {
	if (notes.empty()) return 0.0;
	return notes.front().duration_ms_at(tempo);
}

void build_steps_at(const std::vector<midi_note>& notes, int mode, double tempo, std::vector<note_step>& steps, double& total) {
	steps.clear();
	total = 0.0;
	if (notes.empty()) return;

	if (mode >= 1 && mode <= 4) {
		double spread = 0.0;
		switch (mode) {
			case 1: spread = 0.25; break; // quarter beat
			case 2: spread = 0.5; break;  // eighth
			case 3: spread = 0.125; break; // sixteenth
			case 4: spread = 0.0625; break; // thirty-second
		}
		const double length = notes.front().duration_ms_at(tempo);
		const double step = length * spread;
		for (size_t i = 0; i < notes.size(); ++i) {
			note_step s;
			s.note = i;
			s.at = step * static_cast<double>(i);
			steps.push_back(s);
		}
		total = steps.back().at + length;
		return;
	}

	if (mode == 5) {
		double at = 0.0;
		for (size_t i = 0; i < notes.size(); ++i) {
			note_step s;
			s.note = i;
			s.at = at;
			steps.push_back(s);
			at += notes[i].duration_ms_at(tempo);
		}
		total = at;
		return;
	}

	for (size_t i = 0; i < notes.size(); ++i) {
		note_step s;
		s.note = i;
		s.at = 0.0;
		steps.push_back(s);
	}
	total = group_length_at(notes, tempo);
}

// Waits until the moment is reached, in short hops. A single long wait would
// freeze the script and, in a game, the whole window with it.
void wait_until(double moment) {
	// Resolved once: the engine owns wait(), and looking it up per slice would
	// cost more than the slice itself. Sleeping in short hops keeps the
	// script's own clock going and lets it react.
	asIScriptEngine* engine = g_engine;
	asIScriptFunction* wait_fn = nullptr;
	if (engine) {
		asIScriptModule* module = engine->GetModule(0);
		if (module) wait_fn = module->GetFunctionByDecl("void wait(int)");
	}
	for (;;) {
		const double left = moment - now_ms();
		if (left <= 0.0) return;
		if (!wait_fn) return; // no script to keep alive: the caller waits on
		asIScriptContext* context = engine->CreateContext();
		if (!context) return;
		context->Prepare(wait_fn);
		context->SetArgDWord(0, static_cast<asUINT>(left > 5.0 ? 5 : static_cast<int>(left) + 1));
		context->Execute();
		context->Release();
	}
}

unsigned int midi_output::stop_all_notes() {
	const unsigned int count = static_cast<unsigned int>(sounding.size());
	for (size_t i = 0; i < sounding.size(); ++i) note_off(sounding[i]);
	sounding.clear();
	return count;
}

unsigned int midi_output::get_active_notes() const {
	return static_cast<unsigned int>(sounding.size());
}

// Releases one note and takes it off the sounding list. Written out here
// rather than inside the player loop so the loop stays readable, and so a
// repeated note is only released once per entry.
void midi_output::release_one(const midi_note& note) {
	note_off(note);
	for (size_t i = 0; i < sounding.size(); ) {
		if (sounding[i].pitch == note.pitch && sounding[i].channel == note.channel) {
			sounding.erase(sounding.begin() + i);
			return;
		}
		++i;
	}
}

// The eight ways a group of notes can be laid out in time. The names are what
// a script writes; the numbers are what build_steps switches on.
bool mode_of(const std::string& name, int& mode) {
	static const char* names[] = { "chord", "spread", "arpeggio", "quick", "fast", "sequence", "repeat", "strum" };
	static const int values[] = { 0, 1, 2, 3, 4, 5, 6, 1 };
	for (int i = 0; i < 8; ++i) {
		if (name == names[i]) {
			mode = values[i];
			return true;
		}
	}
	return false;
}

void midi_output::note_on(const midi_note& note) {
	send_note_on(static_cast<unsigned int>(note.channel), static_cast<unsigned int>(note.pitch), static_cast<unsigned int>(note.velocity));
}

void midi_output::note_off(const midi_note& note) {
	send_note_off(static_cast<unsigned int>(note.channel), static_cast<unsigned int>(note.pitch), 0);
}

bool midi_output::play_chord(CScriptArray& notes) {
	clear_error();
	if (!midi_out) {
		set_error("no MIDI output port is open");
		return false;
	}
	std::vector<midi_note> collected;
	if (!collect_notes(&notes, tempo, collected)) {
		set_error("no notes to play: the array is empty or holds no note objects");
		return false;
	}

	// Wait for whatever was still sounding, so two chords never overlap by
	// accident.
	if (!sounding.empty()) {
		const double end = now_ms() + group_length_at(sounding, tempo);
		wait_until(end);
		stop_all_notes();
	}

	for (size_t i = 0; i < collected.size(); ++i) note_on(collected[i]);
	sounding = collected;
	return true;
}

// The chord the array describes, laid out in time. Each pattern compiles down
// to the same schedule of note ons and releases, so one player serves them
// all: play_midi_chord returns once the last note has been released,
// play_midi_chord_wait is the same call with the intent spelled out.
bool midi_output::play_midi_chord(CScriptArray& notes, const std::string& pattern) {
	return play_midi_chord_wait(notes, pattern);
}

bool midi_output::play_midi_chord_wait(CScriptArray& notes, const std::string& pattern) {
	clear_error();
	if (!midi_out) {
		set_error("no MIDI output port is open");
		return false;
	}
	stop_all_notes();
	std::vector<midi_note> collected;
	if (!collect_notes(&notes, tempo, collected)) {
		set_error("no notes to play: the array is empty or holds no note objects");
		return false;
	}
	int mode = 0;
	if (!mode_of(pattern, mode)) {
		set_error("unknown pattern \"" + pattern + "\"");
		return false;
	}
	std::vector<note_step> steps;
	double total = 0.0;
	build_steps_at(collected, mode, tempo, steps, total);

	// Notes sharing a moment go out together: a chord sends all of its note
	// ons in one batch, and from then on every note is released at its own
	// moment + length. That is what makes an arpeggio keep its earlier notes
	// ringing while the later ones arrive.
	const double start = now_ms();
	std::vector<double> release_at;
	std::vector<size_t> release_note;
	size_t i = 0;
	while (i < steps.size()) {
		wait_until(start + steps[i].at);
		const double moment = steps[i].at;
		while (i < steps.size() && steps[i].at <= moment + 0.0001) {
			const midi_note& note = collected[steps[i].note];
			note_on(note);
			sounding.push_back(note);
			release_at.push_back(moment + note.duration_ms_at(tempo));
			release_note.push_back(steps[i].note);
			++i;
		}

		// Sleep until either the next note arrives or the earliest release is
		// due, whichever comes first, so nothing waits longer than it must.
		double until = (i < steps.size()) ? steps[i].at : total;
		for (size_t k = 0; k < release_at.size(); ++k) {
			if (release_at[k] < until) until = release_at[k];
		}
		wait_until(start + until);

		const double elapsed = now_ms() - start;
		for (size_t k = 0; k < release_at.size(); ) {
			if (release_at[k] > elapsed) { ++k; continue; }
			release_one(collected[release_note[k]]);
			release_at.erase(release_at.begin() + k);
			release_note.erase(release_note.begin() + k);
		}
	}
	// Whatever is still ringing gets its full length.
	for (size_t k = 0; k < release_at.size(); ++k) {
		wait_until(start + release_at[k]);
		release_one(collected[release_note[k]]);
	}
	stop_all_notes();
	return true;
}

bool midi_output::play_chord_wait(CScriptArray& notes) {
	if (!play_chord(notes)) return false;
	wait_until(now_ms() + group_length_at(sounding, tempo));
	stop_all_notes();
	return true;
}

// A length written at this port's tempo, so a script names the unit only:
// music.tempo = 96; music.play_chord(notes written as one beat long);
midi_duration midi_output::duration(double amount, int unit) const {
	const double explicit_tempo = music_unit_tempo(unit);
	return midi_duration(amount, unit, explicit_tempo > 0.0 ? explicit_tempo : tempo);
}

bool midi_output::play_note(const midi_note& note) {
	clear_error();
	if (!midi_out) {
		set_error("no MIDI output port is open");
		return false;
	}
	if (!sounding.empty()) {
		wait_until(now_ms() + group_length_at(sounding, tempo));
		stop_all_notes();
	}
	midi_note copy = note;
	clamp_note(copy);
	note_on(copy);
	sounding.push_back(copy);
	return true;
}

bool midi_output::play_note_wait(const midi_note& note) {
	if (!play_note(note)) return false;
	wait_until(now_ms() + group_length_at(sounding, tempo));
	stop_all_notes();
	return true;
}

// ---------------------------------------------------------------------------
// Port search and configuration
// ---------------------------------------------------------------------------

int midi_find_input_port(const std::string& substring) {
	if (trim(substring).empty()) return -1;
	const unsigned int count = midi_input_port_count();
	for (unsigned int i = 0; i < count; ++i) {
		if (contains_ci(midi_input_port_name(i), substring)) return static_cast<int>(i);
	}
	return -1;
}

int midi_find_output_port(const std::string& substring) {
	if (trim(substring).empty()) return -1;
	const unsigned int count = midi_output_port_count();
	for (unsigned int i = 0; i < count; ++i) {
		if (contains_ci(midi_output_port_name(i), substring)) return static_cast<int>(i);
	}
	return -1;
}

midi_config::midi_config() : match("nord"), port(0), last_port(-1) {}

bool midi_config::load(const std::string& path) {
	clear_error();
	this->path = path;
	std::ifstream file(path.c_str());
	if (!file) {
		set_error("cannot read the configuration file " + path);
		return false;
	}
	std::string line;
	while (std::getline(file, line)) {
		const size_t comment = line.find('#');
		if (comment != std::string::npos) line = line.substr(0, comment);
		const size_t equals = line.find('=');
		if (equals == std::string::npos) continue;
		const std::string key = to_lower(trim(line.substr(0, equals)));
		const std::string value = trim(line.substr(equals + 1));
		if (key.empty()) continue;
		if (key == "match" || key == "name" || key == "port_name") {
			match = value;
		} else if (key == "port" || key == "index") {
			std::istringstream number(value);
			int index = 0;
			if (number >> index) port = index;
			else set_error("the configuration file " + path + " has a port that is not a number: \"" + value + "\"");
		}
		// Anything else is a key from a newer version; ignoring it keeps an
		// old plugin usable with a new file.
	}
	return true;
}

bool midi_config::load_if_present(const std::string& path) {
	std::ifstream file(path.c_str());
	if (!file) {
		this->path = path;
		return false;
	}
	file.close();
	return load(path);
}

int midi_config::pick(bool input) {
	clear_error();
	const int found = input ? midi_find_input_port(match) : midi_find_output_port(match);
	if (found >= 0) {
		last_port = found;
		return found;
	}
	const unsigned int count = input ? midi_input_port_count() : midi_output_port_count();
	if (port >= 0 && static_cast<unsigned int>(port) < count) {
		last_port = port;
		return port;
	}
	last_port = -1;
	return -1;
}

int midi_config::find_input_port() const { return const_cast<midi_config*>(this)->pick(true); }

int midi_config::find_output_port() const { return const_cast<midi_config*>(this)->pick(false); }

std::string midi_config::describe() const {
	if (last_port < 0) return "nothing matched \"" + match + "\"";
	const std::string name = midi_input_port_name(static_cast<unsigned int>(last_port));
	if (contains_ci(name, match)) return name;
	return name + " (fallback port " + std::to_string(last_port) + ")";
}

// ---------------------------------------------------------------------------
// Port enumeration
// ---------------------------------------------------------------------------

unsigned int midi_input_port_count() {
	try {
		return with_port<RtMidiIn>([](RtMidiIn& in) {
			return in.getPortCount();
		});
	} catch (RtMidiError& error) {
		set_error(error.getMessage());
		return 0;
	}
}

unsigned int midi_output_port_count() {
	try {
		return with_port<RtMidiOut>([](RtMidiOut& out) {
			return out.getPortCount();
		});
	} catch (RtMidiError& error) {
		set_error(error.getMessage());
		return 0;
	}
}

std::string midi_input_port_name(unsigned int port) {
	try {
		return with_port<RtMidiIn>([port](RtMidiIn& in) -> std::string {
			if (port >= in.getPortCount()) return "";
			return in.getPortName(port);
		});
	} catch (RtMidiError&) {
		return "";
	}
}

// The port name, handed to the engine as bytes rather than as a string.
//
// Measured, twice, on the runner. The `const char*` return that lived here
// first did not survive its own run: the probe died at -1073740791
// (0xC0000409, STATUS_STACK_BUFFER_OVERRUN) before its first print, with no
// output file written. So the return type is not the fault - a pointer to
// bytes fails exactly the way a std::string fails. The whole string return
// is the fault, whatever it is spelled as.
//
// What the same runs did establish, because the split is sharp:
//
//     midi_output_port_count()   -> 1        answers, no crash
//     midi_output_port_name(0)   -> crash    never returns
//
// An integer crosses this boundary intact. A string does not. And the crash
// is on the *write* into the return slot, not on the read: the address the
// engine points at is not zero (a null would fault cleanly and immediately)
// but it is also not the engine's own string object, so the write corrupts
// memory and the failure surfaces later as a stack overrun. Every string this
// plugin returns has been arriving as bytes for the same reason, and the
// engine's answer to `GetStringFactory` - 67108876, which is -2,
// asINVALID_ARG - says it has no string factory to convert with in the first
// place.
//
// So no string crosses this boundary at all. A name is a byte buffer, and the
// two functions below expose it as a length and an indexed byte. Integers,
// which are known to work here. The script assembles its own string on its own
// side, where literals and concatenation are alive - measured, the engine's
// own strings are fine.
int midi_output_port_name_byte_count(unsigned int port) {
	try {
		return (int)with_port<RtMidiOut>([port](RtMidiOut& out) -> std::size_t {
			if (port >= out.getPortCount()) return 0;
			return out.getPortName(port).size();
		});
	} catch (RtMidiError&) {
		return 0;
	}
}

// The byte at `index` of the port name, or -1 when there is none. -1 rather
// than 0 because a name may legitimately contain a zero byte and the caller
// has to be able to tell "no such byte" from "an actual NUL".
int midi_output_port_name_byte(unsigned int port, unsigned int index) {
	try {
		return with_port<RtMidiOut>([port, index](RtMidiOut& out) -> int {
			if (port >= out.getPortCount()) return -1;
			const std::string name = out.getPortName(port);
			if (index >= name.size()) return -1;
			return (int)(unsigned char)name[index];
		});
	} catch (RtMidiError&) {
		return -1;
	}
}

std::string midi_output_port_name(unsigned int port) {
	try {
		return with_port<RtMidiOut>([port](RtMidiOut& out) -> std::string {
			if (port >= out.getPortCount()) return "";
			return out.getPortName(port);
		});
	} catch (RtMidiError&) {
		return "";
	}
}

// Builds the array by hand: Angelscript cannot convert std::vector<std::string>
// into a string[] on its own, and the array type is only reachable through the
// engine of the running script.
static CScriptArray* make_string_array(const std::vector<std::string>& names) {
	asIScriptContext* ctx = asGetActiveContext();
	if (!ctx) return 0;
	asIScriptEngine* engine = ctx->GetEngine();
	if (!engine) return 0;
	asITypeInfo* type = engine->GetTypeInfoByDecl("array<string>");
	if (!type) return 0;
	CScriptArray* array = CScriptArray::Create(type, (asUINT)names.size());
	if (!array) return 0;
	for (asUINT i = 0; i < (asUINT)names.size(); ++i)
		array->SetValue(i, (void*)&names[i]);
	return array;
}

// ---------------------------------------------------------------------------
// The byte views
// ---------------------------------------------------------------------------
//
// Every string this plugin hands to a script goes out as a length and an
// indexed byte. Not as a convenience - as the only form measured to survive.
// The port name was the one that crashed the process; the rest were reached
// through the same return slot and are exposed this way until a run says which
// of them were actually broken.
//
// The three object methods cannot share a body through a free function because
// each one calls a different to_string(), so the pattern is written out. It is
// four lines each and there is no cleverness in it on purpose: the failure this
// whole change is about was a shape mismatch nobody could see, and a shape
// nobody can see is exactly what a macro hides.

// midi_message::to_string returns a const char* into a member buffer, not a
// std::string like the other two - the only one of the three that does. The
// strlen is right here and safe, because the buffer is the message's own and
// holds text this file built.
int midi_message::to_string_byte_count() const { return (int)std::strlen(to_string()); }
int midi_message::to_string_byte(unsigned int index) const {
	const char* s = to_string();
	const std::size_t n = std::strlen(s);
	if (index >= n) return -1;
	return (int)(unsigned char)s[index];
}

int midi_duration::to_string_byte_count() const { return (int)to_string().size(); }
int midi_duration::to_string_byte(unsigned int index) const {
	const std::string s = to_string();
	if (index >= s.size()) return -1;
	return (int)(unsigned char)s[index];
}

int midi_note::to_string_byte_count() const { return (int)to_string().size(); }
int midi_note::to_string_byte(unsigned int index) const {
	const std::string s = to_string();
	if (index >= s.size()) return -1;
	return (int)(unsigned char)s[index];
}

int midi_input_port_name_byte_count(unsigned int port) { return (int)midi_input_port_name(port).size(); }
int midi_input_port_name_byte(unsigned int port, unsigned int index) {
	const std::string name = midi_input_port_name(port);
	if (index >= name.size()) return -1;
	return (int)(unsigned char)name[index];
}

int midi_api_name_byte_count() { return (int)midi_api_name().size(); }
int midi_api_name_byte(unsigned int index) {
	const std::string name = midi_api_name();
	if (index >= name.size()) return -1;
	return (int)(unsigned char)name[index];
}

int midi_message_name_byte_count(const midi_message& m) { return (int)midi_message_name(m).size(); }
int midi_message_name_byte(const midi_message& m, unsigned int index) {
	const std::string name = midi_message_name(m);
	if (index >= name.size()) return -1;
	return (int)(unsigned char)name[index];
}

// Which asEEngineProp the plugin asks the engine for.
//
// asEP_INIT_CALL_STACK_SIZE is 30, the last enumerator this header defines
// that the engine is guaranteed to implement: enumerators from 31 on were
// added later and an engine of another vintage may not have them. 30 is the
// one value both this header and any engine of this line agree on.
static const int k_engine_probe_prop = 30;

// The engine, asked through the plugin's own pointer.
//
// This exists because asIScriptEngine exposes no version of itself - the
// interface is fixed and the library version lives in a free function the
// engine exports. So the version cannot be read off the object. What can be
// read is whether the object answers the way this header says it should.
//
// GetEngineProperty is a virtual, so the call resolves through the vtable
// slot this header assigns it. If the engine was built from a different
// header, that slot is a different function and the answer is nonsense or
// the process dies - which is exactly the shape of the fault: the add-on
// enters its first engine call and never returns.
//
// -1000000 marks "no answer". The property is an integer and 0 is a value
// the engine may legitimately hold, so absence cannot be spelled 0.
//
// Every argument here is a raw void*, an int and a double: nothing in this
// function is constructed or destructed, so a call that lands on the wrong
// vtable slot still cannot corrupt anything on its way to the answer.
int midi_engine_probe() {
	if (!g_engine) return -1000000;
	return (int)g_engine->GetEngineProperty((asEEngineProp)k_engine_probe_prop);
}

int midi_last_error_byte_count() { return (int)midi_last_error().size(); }
int midi_last_error_byte(unsigned int index) {
	const std::string e = midi_last_error();
	if (index >= e.size()) return -1;
	return (int)(unsigned char)e[index];
}

int midi_first_error_byte_count() { return (int)midi_first_error().size(); }
int midi_first_error_byte(unsigned int index) {
	const std::string e = midi_first_error();
	if (index >= e.size()) return -1;
	return (int)(unsigned char)e[index];
}

// The five that the sweep found second: they were not in the list when this
// started, because the list was written from memory of the string surfaces
// rather than from the registrations. Reading the registrations for anything
// spelled "string" found them, and that is the lesson of this change: the
// question is not which functions I remember returning a string, it is which
// ones the engine is told will.
int midi_input::get_port_name_byte_count() const { return (int)get_port_name().size(); }
int midi_input::get_port_name_byte(unsigned int index) const {
	const std::string s = get_port_name();
	if (index >= s.size()) return -1;
	return (int)(unsigned char)s[index];
}

int midi_output::get_port_name_byte_count() const { return (int)get_port_name().size(); }
int midi_output::get_port_name_byte(unsigned int index) const {
	const std::string s = get_port_name();
	if (index >= s.size()) return -1;
	return (int)(unsigned char)s[index];
}

int midi_config::describe_byte_count() const { return (int)describe().size(); }
int midi_config::describe_byte(unsigned int index) const {
	const std::string s = describe();
	if (index >= s.size()) return -1;
	return (int)(unsigned char)s[index];
}

int midi_note_pitch_name_byte_count(int pitch) { return (int)midi_note_pitch_name(pitch).size(); }
int midi_note_pitch_name_byte(int pitch, unsigned int index) {
	const std::string s = midi_note_pitch_name(pitch);
	if (index >= s.size()) return -1;
	return (int)(unsigned char)s[index];
}

CScriptArray* midi_input_port_names() {
	std::vector<std::string> names;
	const unsigned int count = midi_input_port_count();
	for (unsigned int i = 0; i < count; ++i) names.push_back(midi_input_port_name(i));
	return make_string_array(names);
}

CScriptArray* midi_output_port_names() {
	std::vector<std::string> names;
	const unsigned int count = midi_output_port_count();
	for (unsigned int i = 0; i < count; ++i) names.push_back(midi_output_port_name(i));
	return make_string_array(names);
}

// The definition of the constant declared with the other shared helpers.
const RtMidi::Api g_preferred_api = [] {
	std::vector<RtMidi::Api> apis;
	try {
		RtMidi::getCompiledApi(apis);
	} catch (...) {
		return RtMidi::RTMIDI_DUMMY;
	}
	// getCompiledApi lists backends in the constructor's own search order, and
	// the dummy is compiled in unconditionally and sits last, so the first
	// entry that is not the dummy is the real one.
	for (size_t i = 0; i < apis.size(); ++i) {
		if (apis[i] != RtMidi::RTMIDI_DUMMY) return apis[i];
	}
	return RtMidi::RTMIDI_DUMMY;
}();

// Names the backend RtMidi actually compiled against, so a user can tell
// "MMAPI" on Windows from "ALSA" on Linux in a bug report.
std::string midi_api_name() {
	try {
		// RtMidi can be compiled with several backends; the first one is the
		// one a default-constructed port will pick.
		std::vector<RtMidi::Api> apis;
		RtMidiIn::getCompiledApi(apis);
		if (apis.empty()) return "";
		// The backend is spelled out in the name, because the backend is the
		// thing a user cannot otherwise see. A dummy build and an ALSA build
		// are both "nvmidi.so", both load, and both answer this call - and
		// they do completely different things. Without the suffix, "the
		// library loaded" and "the library can reach your keyboard" are the
		// same report, which is how a user ends up running a stub and reading
		// its silence as a broken download.
		//
		// Measured: a dummy plugin reports "Dummy" - the backend's own name,
		// not something written here - so the first argument is that name and
		// not a label invented for it.
		//
		// And measured, the name is the backend's DISPLAY name:
		// RtMidiIn::getApiDisplayName is the second column of RtMidi's own
		// table, and for winmm that column reads "Windows MultiMedia". The
		// short name is the first column and reads "winmm", which happens to
		// contain the word "windows" - so a build that used the wrong column
		// would still match a case-insensitive search for "windows" and go on
		// pretending everything was fine. This call is the right column.
		return "nvmidi/" + RtMidiIn::getApiDisplayName(apis[0]);
	} catch (RtMidiError&) {
		return "";
	}
}

// The backend a default-constructed RtMidi port would use, or UNSPECIFIED if
// RtMidi was built with no backend at all.
static RtMidi::Api midi_default_api() {
	std::vector<RtMidi::Api> apis;
	try {
		RtMidiIn::getCompiledApi(apis);
	} catch (RtMidiError&) {
		return RtMidi::UNSPECIFIED;
	}
	return apis.empty() ? RtMidi::UNSPECIFIED : apis[0];
}

// Whether a backend can create a virtual output port at all.
//
// This is asked rather than discovered, because the answer is a property of
// the backend and not of the machine it is running on. RtMidi's own header
// says it plainly on openVirtualPort - "currently only supported by the
// Macintosh OS-X, Linux ALSA and JACK APIs (the function does nothing with
// the other APIs)" - and the Windows MM implementation is the comment and a
// warning with no code between them.
//
// So on Windows set_virtual_port(true) is not a thing that can work, and a
// plugin that lets it look like it worked is lying. Measured twice over: the
// source of the WinMM backend, and open() of a virtual port on Windows
// returning true while midiOutGetNumDevs() still counted only the devices the
// machine already had.
bool midi_supports_virtual_ports() {
	switch (midi_default_api()) {
		case RtMidi::MACOSX_CORE:
		case RtMidi::LINUX_ALSA:
		case RtMidi::UNIX_JACK:
			return true;
		default:
			return false;
	}
}

// ---------------------------------------------------------------------------
// Message naming
// ---------------------------------------------------------------------------

std::string midi_message_name(const midi_message& m) {
	std::ostringstream out;
	const unsigned char command = m.status & 0xf0;
	switch (command) {
		case 0x80: out << "note off, channel " << m.channel << ", note " << int(m.data1) << ", velocity " << int(m.data2); break;
		case 0x90:
			if (m.data2 == 0) out << "note off, channel " << m.channel << ", note " << int(m.data1) << ", velocity 0";
			else out << "note on, channel " << m.channel << ", note " << int(m.data1) << ", velocity " << int(m.data2);
			break;
		case 0xa0: out << "polyphonic aftertouch, channel " << m.channel << ", note " << int(m.data1) << ", pressure " << int(m.data2); break;
		case 0xb0: out << "control change, channel " << m.channel << ", controller " << int(m.data1) << ", value " << int(m.data2); break;
		case 0xc0: out << "program change, channel " << m.channel << ", program " << int(m.data1); break;
		case 0xd0: out << "channel pressure, channel " << m.channel << ", pressure " << int(m.data1); break;
		case 0xe0: out << "pitch bend, channel " << m.channel << ", value " << ((int(m.data2) << 7) | int(m.data1)); break;
		case 0xf0:
			if (m.status == 0xf0) out << "sysex start";
			else if (m.status == 0xf7) out << "sysex end";
			else out << "system message 0x" << std::hex << int(m.status);
			break;
		default: out << "unknown message 0x" << std::hex << int(m.status); break;
	}
	return out.str();
}

// ---------------------------------------------------------------------------
// Note names
// ---------------------------------------------------------------------------

// The name of a MIDI note, in the convention where 60 is C4 and each octave
// starts at C. Kept in this file so the struct's to_string can use it.
static std::string midi_note_pitch_name(int pitch) {
	if (pitch < 0) pitch = 0;
	if (pitch > 127) pitch = 127;
	static const char* names[] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
	std::ostringstream out;
	out << names[pitch % 12] << (pitch / 12 - 1);
	return out.str();
}

int midi_note_number(const std::string& name) {
	const std::string text = trim(name);
	if (text.empty()) return -1;
	size_t i = 0;
	int semitone = -1;
	switch (std::toupper(static_cast<unsigned char>(text[i]))) {
		case 'C': semitone = 0; break;
		case 'D': semitone = 2; break;
		case 'E': semitone = 4; break;
		case 'F': semitone = 5; break;
		case 'G': semitone = 7; break;
		case 'A': semitone = 9; break;
		case 'B': semitone = 11; break;
		default: return -1;
	}
	++i;
	if (i < text.size() && (text[i] == '#' || text[i] == 'b' || text[i] == 'B')) {
		semitone += (text[i] == '#') ? 1 : -1;
		++i;
	}
	if (i >= text.size()) return -1;
	// The octave is everything that is left, so "C4" and "C-1" both work.
	std::istringstream number(text.substr(i));
	int octave = 0;
	if (!(number >> octave)) return -1;
	const int pitch = (octave + 1) * 12 + semitone;
	if (pitch < 0 || pitch > 127) return -1;
	return pitch;
}

// ---------------------------------------------------------------------------
// Angelscript registration
// ---------------------------------------------------------------------------

// Every call below returns an AngelScript error code, and the ones that fail
// are the ones nothing complains about: an asIScriptEngine refuses a
// registration it does not like and returns a negative number, the plugin
// still loads, and the script that then says "Identifier 'midi_input' is not a
// data type" points at itself instead of at the call that was rejected. So the
// first failure is recorded on the plugin's own error string - the one
// midi_last_error() already reports - and the first *unexpected* one is said
// out loud, which is the only way it reaches a runner's log (the reporting
// half of the plugin entry point compiles out when NVGT_PLUGIN_INCLUDE is
// defined, so nothing here can call report_plugin_error()).
// Set by any registration the engine refused; read once by the entry point to
// decide whether to print the engine's own view of the namespace. Deliberately
// outside the struct: the reader is the entry point, which does not hold the
// reporter.
//
// "Any", not "any out of the ordinary". The exemption this used to make for
// asALREADY_REGISTERED was written while -10 was believed to be that code, and
// -10 turns out to be asINVALID_DECLARATION - a fault every time. Nothing has
// yet produced a refusal that was not a fault, so nothing is exempt.
static bool g_registration_failed = false;

struct registration {
	asIScriptEngine* engine;
	int first_failure;
	int first_failure_line;
	std::string first_failure_text;
	// Every refusal, of any code. `unexpected` used to be the strict subset -
	// "not asALREADY_REGISTERED" - which was the same set as `refused` once
	// -10 was read correctly, so the exemption is gone and the two counters
	// now differ only in what they are for.
	int unexpected;
	// Every refusal, whatever its code, and the number the trailer at the end
	// of plugin_main reports. It exists because the two counts answer different
	// questions: `unexpected` says whether a refusal was out of the ordinary,
	// this one says how much of the registration was refused at all.
	int refused;
	// Every call, refused or not. It is what makes `refused` legible: "22 of
	// 22 calls" and "22 of 900" are the same refusal count and different
	// findings, and the log otherwise carries no way to tell them apart.
	int calls;
	registration(asIScriptEngine* e) : engine(e), first_failure(0), first_failure_line(0),
		unexpected(0), refused(0), calls(0) {}
	void check(int result, const char* call, int line) {
		// The entry line first, before anything can return or refuse, and
		// flushed. Every plugin call in this function has been reached by a
		// plugin that then died with nothing on stdout, so the last line
		// printed here is the call that killed it - the one thing the runner
		// has never been able to say. The count is unconditional: a refusal
		// and a crash both leave the trace, and only the trace tells them
		// apart.
		//
		// stderr because that is where plugin_main's own line already reaches
		// the log on this engine (measured, run 36699918927: 84 bytes of
		// stderr carrying the api version), while a registration refusal is
		// known to be dropped on stdout by nvgt's own buffering.
		fprintf(stderr, "nvmidi: reg line %d\n", line);
		fflush(stderr);
		// Every call, before the early return. A refusal is the difference
		// between this counter and the number of calls, which is a number no
		// reader can reconstruct from a log - and the early return below is
		// why the counter sits on this line rather than beside the printf at
		// the end: a counter placed past the return would count only refusals
		// while being described as counting calls.
		calls += 1;
		if (result < 0) refused += 1;
		if (result >= 0) return;
		// This vendored header exposes no error text for a rejected
		// registration (no GetLastError, no context in hand at load time), so
		// the code itself is the message, next to the call that produced it.
		std::string text = "code " + std::to_string(result);
		if (first_failure == 0) {
			first_failure = result;
			first_failure_line = line;
			first_failure_text = std::string(call) + ": " + text;
			set_error("plugin registration " + first_failure_text);
		}
		// This comment used to read the code as asALREADY_REGISTERED and spin a
		// theory out of it - "something loaded before this plugin owns the
		// name". The code is not that one. asALREADY_REGISTERED is -13
		// (angelscript.h:133); -10 is asINVALID_DECLARATION
		// (angelscript.h:130), and it is what the engine answers when the
		// declaration handed to a Register* call does not match the declaration
		// the address behind it carries. No earlier claimant is involved and
		// there is nothing to share a name with: the plugin was describing its
		// own argument to itself, wrongly.
		//
		// Keeping the name out of the line, too. It printed " (asALREADY_REGISTERED)"
		// after every one of them, which is a reader being told the opposite of
		// what the engine said. A code is printed as a number and nothing else
		// here; -10 and -13 are one search away from their names in the vendored
		// header, and a wrong name is worse than no name.
		//
		// The latch is still set on any refusal, because the set of codes that
		// are ordinary has now been measured as empty on this engine - every
		// one of the seven kinds this plugin has actually produced was -10, and
		// all of them were faults.
		if (result != asALREADY_REGISTERED) {
			g_registration_failed = true;
		}
		// stdout, not stderr. Measured across every run of this workflow: nvgt
		// leaves stderr at 0 bytes and carries its own diagnostics on stdout,
		// so a line written to stderr reaches no reader at all - which is how
		// this plugin's account of its own registration stayed invisible while
		// the engine's second-hand symptom was the only thing in the log.
		// `call` is in the line and `text` is not. `text` is built two lines
		// above and reads "code -10", so printing it here put the code in
		// twice and the call name - the only part that says which
		// registration was refused - nowhere. The name is the half a reader
		// needs: a line of codes says how many, and this is the one place
		// that says which.
		fprintf(stdout, "nvmidi: registration refused at src/nvmidi.cpp:%d: %s returned %d\n",
			line, call, result);
		fflush(stdout);
		unexpected += 1;
	}
	// Whether the engine already has a type, asked so that a wrong answer
	// cannot hurt.
	//
	// asITypeInfo is a pure interface the engine owns and the caller only
	// reads through. A probe that constructed or destructed one - by asking
	// for a declaration, say - would need the engine's own allocator and
	// could corrupt it; this one holds the pointer as an opaque asITypeInfo*
	// and never dereferences it, so the worst case is that the engine
	// answers null and this returns false.
	bool type_is_known(const char* name) {
		asITypeInfo* t = engine->GetTypeInfoByName(name);
		if (!t) return false;
		t->Release();
		return true;
	}
	//
	// True when this engine already has an array<T>, and therefore when the
	// add-on must not be called.
	//
	// Four names, all of them known spellings from the vendored header and
	// none of them a guess about this plugin's own types. The template name
	// is asked first: a yes settles it, and a no is followed by three
	// concrete instantiations whose *refusal* is the second signal, since
	// "Invalid template subtype" is only sayable by a template that exists.
	//
	// The rule this method is for - never register the add-on over an engine
	// that has it - is enforced here rather than at the call site so that a
	// build which skips the call cannot reach this code with a wrong answer.
	bool engine_knows_arrays() {
#ifdef NVGT_SKIP_ARRAY_ADDON
		// The mark this build is known by.
		//
		// The answer this flag exists to force is "yes", so it returns before
		// the detection below and none of that code's lines are ever reached.
		// The reading that identifies this build therefore has to be taken
		// here, in the branch that is only compiled when the flag is set -
		// and the string is a literal for its own sake, printed to nothing,
		// so that it also lives in the artifact's bytes for a reader that
		// has only the file and no run. Undefined behaviour from the
		// compiler's point of view and harmless in this toolchain: the same
		// shape an assert compiles to.
		//
		// What this replaced: the workflow used to look for the string
		// "NVGT_SKIP_ARRAY_ADDON is set", from a warning this file printed
		// when the flag was read at the call site. That warning went away
		// when the read moved in here, and the workflow went on searching
		// for it - so from that commit on, every probe row measured the
		// control as carrying no marker at all, threw "carries neither
		// marker", and the ordinary build was never run. Measured, run
		// 36724644733: the run died at the no-array-addon row of the first
		// location, after the with-array-addon row had passed with
		// bisection=no.
		//
		// The two builds are also told apart by their own bytes without
		// this string: only the control carries "the engine does not know
		// array<T", and only the ordinary build carries "calling
		// RegisterScriptArray" and "RegisterScriptArray returned". This
		// literal is here so the two agree in name as well as in count.
		//
		// It is printed and not merely named, because a named string that
		// is never used is not in the artifact at all. Measured, run
		// 36725638696: with the literal held by a local pointer and voided
		// - "the same shape an assert compiles to" - the control dll was
		// built (2988213 bytes, compiled with -DNVGT_SKIP_ARRAY_ADDON) and
		// contained the string zero times, so the probe went on finding no
		// marker and the run died at the same throw. The optimizer is
		// entitled to that: a pointer read by nothing is not a use. A
		// fprintf is a use, and this stream is one the workflow already
		// reads.
		fprintf(stderr, "nvmidi: NVGT_SKIP_ARRAY_ADDON is set, the engine is not asked\n");
		fflush(stderr);
		return true;
#endif
		if (type_is_known("array")) {
			fprintf(stderr, "nvmidi: GetTypeInfoByName(\"array\") answered yes\n");
			fflush(stderr);
			return true;
		}
		// A live template's name is not "array<int>" - it is "array" plus the
		// fact that the engine folds it to a specialization, and the engine's
		// own GetTypeInfoByDecl("array<int>") answers that question with no
		// side effect at all. The three RegisterObjectType calls that used to
		// stand here are gone, and the reason is the worst defect this file has
		// carried: they were not a probe, they were a poison.
		//
		// Measured, in a program with no plugin in it at all
		// (tools/probe_flag_arm.cpp, build tree /home/deniz/nvmidi-build):
		//
		//   before any registration, Build = 0
		//   RegisterObjectType("probe", 0, asOBJ_REF | asOBJ_SCRIPT_OBJECT) = -5
		//   after that one call, Build of the same script = -17
		//
		// One refused registration is enough. ConfigError() sets
		// m_engine->configFailed and nothing ever clears it, so from that
		// moment on every asCModule::Build() short-circuits to -17
		// (asINVALID_CONFIGURATION) before a single line of script is
		// compiled - which is why the harness reported "the script DID NOT
		// COMPILE: -17" for scripts that were never looked at, why bisecting
		// the script by statement changed nothing at all, and why a one-line
		// canary that cannot fail to compile also came back -17.
		//
		// The flags here are the ones that did it: bit 0 | bit 21. The
		// engine's asOBJ_MASK_VALID_FLAGS is 0x1801FFFFF - bits 0..20 plus 27
		// and 28 - so bit 21 is outside it, and RegisterObjectType refuses
		// outright with asINVALID_ARG. Note that this is not a mistake about
		// what the flag means: these calls were ALWAYS going to be refused,
		// because the flags never matched a registrable type.
		const char* probes[] = { "array<int>", "array<uint>", "array<double>" };
		bool array_is_there = false;
		for (int i = 0; i < 3; ++i) {
			asITypeInfo* found = engine->GetTypeInfoByDecl(probes[i]);
			fprintf(stderr, "nvmidi: does the engine know %s? %s\n",
				probes[i], found ? "yes" : "no");
			fflush(stderr);
			if (found) array_is_there = true;
		}
		if (!array_is_there) return true;

		// A "no" from the three probes above ends this method, and this is
		// deliberate - the reason it must NOT be softened is a defect the
		// harness found on its first real run, and the softened version was
		// written, built and measured before being taken back out.
		//
		// The three RegisterObjectType calls are a PROBE, and they are not
		// free: whatever their return codes say, the engine keeps the type it
		// just created, and afterwards GetTypeInfoByName("array") answers yes
		// for the rest of the process. So a probe that is asked and then
		// answered "no" has already destroyed the fact it was asking about.
		//
		// The softened version asked GetTypeInfoByDecl("array<int>") after the
		// probes to tell a live template from a dead one. Measured on this
		// harness (probe_host, build tree /home/deniz/nvmidi-build): the
		// answer came back non-null, and for a reason that has nothing to do
		// with the template being usable - it was the probe's own
		// array<int> being handed back. The added check returned true, the
		// add-on was skipped exactly as before, and the nine refusals were
		// identical line for line. Nothing was gained and the probe's
		// corruption was given a name that reads like a clean bill of health.
		//
		// So the cheap-looking improvement was measuring itself. The two
		// branches below are now a real choice of order: this method must run
		// over an engine that has NOT been probed yet, and the caller asks it
		// first (see the call site: engine_knows_arrays && array_is_usable,
		// short-circuit, so array_is_usable is never reached once this
		// answers no).
		// The engine did not know a single one of the three: fall through to
		// the add-on, which is the branch that registers array<T> properly.
		return false;
	}

	//
	// True when this engine has an array<T> that can actually be instantiated -
	// which is a different question from engine_knows_arrays(), and the
	// difference is the whole of the nine refused registrations this harness
	// was built to find.
	//
	// engine_knows_arrays() cannot ask it: by the time it has finished
	// probing, the engine's answer to any question about "array" is its own
	// probe. This method therefore runs only on the branch where that method
	// said yes - and on that branch the probes never ran, so the engine's
	// answer here is its own.
	//
	// The name being taken is not the fact that matters. angelscript's own
	// native array add-on refuses a subtype it does not know with "Invalid
	// template subtype", and the type system has by then already learned the
	// name - a refused declaration leaves the template registered. Every later
	// question of the form "does this engine know array<T>" then answers yes
	// while not one subtype is usable, and registering array<midi_note@> in a
	// method signature fails with -10 asINVALID_DECLARATION.
	//
	// So the fact asked for is a concrete instantiation. If it resolves, the
	// engine has a working array<T> and the add-on must not be called over it:
	// that second call is what killed the process in run 36700487540, whose
	// first statement is a cleanup-callback install over a cache the engine had
	// already built (scriptarray.cpp:292, ARRAY_CACHE at line 52).
	bool array_is_usable() {
		if (!type_is_known("array")) return false;
		asITypeInfo* concrete = engine->GetTypeInfoByDecl("array<int>");
		if (concrete) {
			concrete->Release();
			fprintf(stderr, "nvmidi: array<int> resolves; the engine has a working array<T>\n");
			fflush(stderr);
			return true;
		}
		// The name is taken and no subtype resolves. That is the state the
		// add-on IS wanted in, and calling it here is safe precisely because
		// this branch is only reached when the probes above did not run: there
		// is no cache for RegisterScriptArray_Native to install over.
		fprintf(stderr, "nvmidi: \"array\" is taken but no array<int> resolves; the add-on will be called\n");
		fflush(stderr);
		return false;
	}
};


void register_midi_message(asIScriptEngine* engine, registration* reg) {
	reg->check( engine->RegisterObjectType("midi_message", sizeof(midi_message), asOBJ_VALUE | asOBJ_POD | asGetTypeTraits<midi_message>()), "RegisterObjectType", __LINE__);
	reg->check( engine->RegisterObjectProperty("midi_message", "uint8 status", asOFFSET(midi_message, status)), "RegisterObjectProperty", __LINE__);
	reg->check( engine->RegisterObjectProperty("midi_message", "uint8 data1", asOFFSET(midi_message, data1)), "RegisterObjectProperty", __LINE__);
	reg->check( engine->RegisterObjectProperty("midi_message", "uint8 data2", asOFFSET(midi_message, data2)), "RegisterObjectProperty", __LINE__);
	reg->check( engine->RegisterObjectProperty("midi_message", "int channel", asOFFSET(midi_message, channel)), "RegisterObjectProperty", __LINE__);
	reg->check( engine->RegisterObjectProperty("midi_message", "double timestamp", asOFFSET(midi_message, timestamp)), "RegisterObjectProperty", __LINE__);
}

void register_midi_input(asIScriptEngine* engine, registration* reg) {
	// A handle type. asOBJ_REF alone is not enough and run 36640089846 said so
	// in the engine's own words: "Type 'midi_input' is missing behaviours ...
	// A reference type must have the addref and release behaviours". Every
	// type in this plugin had asOBJ_NOCOUNT dropped in that run and every one
	// of the four came back with that error, so the flag is not a candidate -
	// it is required, and the four registrations below carry it for that
	// reason. Size 0, and not sizeof: the two types measured to declare
	// cleanly in run 36638827355 were the two registered at 0.
	reg->check( engine->RegisterObjectType("midi_input", 0, asOBJ_REF | asOBJ_NOCOUNT), "RegisterObjectType", __LINE__);
	reg->check( engine->RegisterObjectMethod("midi_input", "bool open(uint port, const string&in name = \"nvmidi\")", asMETHOD(midi_input, open), asCALL_THISCALL), "RegisterObjectMethod", __LINE__);
	reg->check( engine->RegisterObjectMethod("midi_input", "bool open_by_name(const string&in substring, const string&in name = \"nvmidi\")", asMETHOD(midi_input, open_by_name), asCALL_THISCALL), "RegisterObjectMethod", __LINE__);
	reg->check( engine->RegisterObjectMethod("midi_input", "bool open_config(midi_config@ config, const string&in name = \"nvmidi\")", asMETHOD(midi_input, open_config), asCALL_THISCALL), "RegisterObjectMethod", __LINE__);
	reg->check( engine->RegisterObjectMethod("midi_input", "bool play_chord(array<midi_note@>&in notes)", asMETHOD(midi_input, play_chord), asCALL_THISCALL), "RegisterObjectMethod", __LINE__);
	reg->check( engine->RegisterObjectMethod("midi_input", "bool play_chord_wait(array<midi_note@>&in notes)", asMETHOD(midi_input, play_chord_wait), asCALL_THISCALL), "RegisterObjectMethod", __LINE__);
	reg->check( engine->RegisterObjectMethod("midi_input", "bool play_note(const midi_note&in note)", asMETHOD(midi_input, play_note), asCALL_THISCALL), "RegisterObjectMethod", __LINE__);
	reg->check( engine->RegisterObjectMethod("midi_input", "bool play_note_wait(const midi_note&in note)", asMETHOD(midi_input, play_note_wait), asCALL_THISCALL), "RegisterObjectMethod", __LINE__);
	reg->check( engine->RegisterObjectMethod("midi_input", "uint stop_all_notes()", asMETHOD(midi_input, stop_all_notes), asCALL_THISCALL), "RegisterObjectMethod", __LINE__);
	reg->check( engine->RegisterObjectMethod("midi_input", "uint get_active_notes() const", asMETHOD(midi_input, get_active_notes), asCALL_THISCALL), "RegisterObjectMethod", __LINE__);
	reg->check( engine->RegisterObjectMethod("midi_input", "void close()", asMETHOD(midi_input, close), asCALL_THISCALL), "RegisterObjectMethod", __LINE__);
	reg->check( engine->RegisterObjectMethod("midi_input", "bool is_open() const", asMETHOD(midi_input, is_open), asCALL_THISCALL), "RegisterObjectMethod", __LINE__);
	reg->check( engine->RegisterObjectMethod("midi_input", "int get_port() const", asMETHOD(midi_input, get_port), asCALL_THISCALL), "RegisterObjectMethod", __LINE__);
	reg->check( engine->RegisterObjectMethod("midi_input", "int get_port_name_byte_count() const", asMETHOD(midi_input, get_port_name_byte_count), asCALL_THISCALL), "RegisterObjectMethod", __LINE__);
	reg->check( engine->RegisterObjectMethod("midi_input", "int get_port_name_byte(uint index) const", asMETHOD(midi_input, get_port_name_byte), asCALL_THISCALL), "RegisterObjectMethod", __LINE__);
	reg->check( engine->RegisterObjectMethod("midi_input", "bool has_message() const", asMETHOD(midi_input, has_message), asCALL_THISCALL), "RegisterObjectMethod", __LINE__);
	reg->check( engine->RegisterObjectMethod("midi_input", "uint get_pending() const", asMETHOD(midi_input, get_pending), asCALL_THISCALL), "RegisterObjectMethod", __LINE__);
	reg->check( engine->RegisterObjectMethod("midi_input", "bool next_message(midi_message&out) const", asMETHOD(midi_input, next_message), asCALL_THISCALL), "RegisterObjectMethod", __LINE__);
	reg->check( engine->RegisterObjectMethod("midi_input", "void clear()", asMETHOD(midi_input, clear), asCALL_THISCALL), "RegisterObjectMethod", __LINE__);
	reg->check( engine->RegisterObjectMethod("midi_input", "void set_ignore_sysex(bool)", asMETHOD(midi_input, set_ignore_sysex), asCALL_THISCALL), "RegisterObjectMethod", __LINE__);
	reg->check( engine->RegisterObjectMethod("midi_input", "bool get_ignore_sysex() const", asMETHOD(midi_input, get_ignore_sysex), asCALL_THISCALL), "RegisterObjectMethod", __LINE__);
	reg->check( engine->RegisterObjectMethod("midi_input", "void set_ignore_timing(bool)", asMETHOD(midi_input, set_ignore_timing), asCALL_THISCALL), "RegisterObjectMethod", __LINE__);
	reg->check( engine->RegisterObjectMethod("midi_input", "bool get_ignore_timing() const", asMETHOD(midi_input, get_ignore_timing), asCALL_THISCALL), "RegisterObjectMethod", __LINE__);
	reg->check( engine->RegisterObjectMethod("midi_input", "bool play_midi_chord(array<midi_note@>&in notes, const string&in pattern)", asMETHOD(midi_input, play_midi_chord), asCALL_THISCALL), "RegisterObjectMethod", __LINE__);
	reg->check( engine->RegisterObjectMethod("midi_input", "bool play_midi_chord_wait(array<midi_note@>&in notes, const string&in pattern)", asMETHOD(midi_input, play_midi_chord_wait), asCALL_THISCALL), "RegisterObjectMethod", __LINE__);
	reg->check( engine->RegisterObjectMethod("midi_input", "bool play_sequence(array<midi_note@>&in notes)", asMETHOD(midi_input, play_sequence), asCALL_THISCALL), "RegisterObjectMethod", __LINE__);
	reg->check( engine->RegisterObjectMethod("midi_input", "midi_duration duration(double amount, int unit) const", asMETHOD(midi_input, duration), asCALL_THISCALL), "RegisterObjectMethod", __LINE__);
	reg->check( engine->RegisterObjectProperty("midi_input", "double tempo", asOFFSET(midi_input, tempo)), "RegisterObjectProperty", __LINE__);
}

void register_midi_output(asIScriptEngine* engine, registration* reg) {
	// A handle type, and registered exactly as midi_input is, for the reason
	// written there: asOBJ_NOCOUNT is required, not optional, and 0 is the
	// size the clean pair carried. The type held a real sizeof until run
	// 36640089846; nothing measured ever singled the size out, and the one run
	// that changed it changed three other things with it.
	reg->check( engine->RegisterObjectType("midi_output", 0, asOBJ_REF | asOBJ_NOCOUNT), "RegisterObjectType", __LINE__);
	reg->check( engine->RegisterObjectMethod("midi_output", "bool open(uint port, const string&in name = \"nvmidi\")", asMETHOD(midi_output, open), asCALL_THISCALL), "RegisterObjectMethod", __LINE__);
	reg->check( engine->RegisterObjectMethod("midi_output", "bool open_by_name(const string&in substring, const string&in name = \"nvmidi\")", asMETHOD(midi_output, open_by_name), asCALL_THISCALL), "RegisterObjectMethod", __LINE__);
	reg->check( engine->RegisterObjectMethod("midi_output", "bool open_config(midi_config@ config, const string&in name = \"nvmidi\")", asMETHOD(midi_output, open_config), asCALL_THISCALL), "RegisterObjectMethod", __LINE__);
	reg->check( engine->RegisterObjectMethod("midi_output", "bool play_chord(array<midi_note@>&in notes)", asMETHOD(midi_output, play_chord), asCALL_THISCALL), "RegisterObjectMethod", __LINE__);
	reg->check( engine->RegisterObjectMethod("midi_output", "bool play_chord_wait(array<midi_note@>&in notes)", asMETHOD(midi_output, play_chord_wait), asCALL_THISCALL), "RegisterObjectMethod", __LINE__);
	reg->check( engine->RegisterObjectMethod("midi_output", "bool play_midi_chord(array<midi_note@>&in notes, const string&in pattern)", asMETHOD(midi_output, play_midi_chord), asCALL_THISCALL), "RegisterObjectMethod", __LINE__);
	reg->check( engine->RegisterObjectMethod("midi_output", "bool play_midi_chord_wait(array<midi_note@>&in notes, const string&in pattern)", asMETHOD(midi_output, play_midi_chord_wait), asCALL_THISCALL), "RegisterObjectMethod", __LINE__);
	reg->check( engine->RegisterObjectMethod("midi_output", "bool play_note(const midi_note&in note)", asMETHOD(midi_output, play_note), asCALL_THISCALL), "RegisterObjectMethod", __LINE__);
	reg->check( engine->RegisterObjectMethod("midi_output", "bool play_note_wait(const midi_note&in note)", asMETHOD(midi_output, play_note_wait), asCALL_THISCALL), "RegisterObjectMethod", __LINE__);
	reg->check( engine->RegisterObjectMethod("midi_output", "uint stop_all_notes()", asMETHOD(midi_output, stop_all_notes), asCALL_THISCALL), "RegisterObjectMethod", __LINE__);
	reg->check( engine->RegisterObjectMethod("midi_output", "uint get_active_notes() const", asMETHOD(midi_output, get_active_notes), asCALL_THISCALL), "RegisterObjectMethod", __LINE__);
	reg->check( engine->RegisterObjectMethod("midi_output", "void close()", asMETHOD(midi_output, close), asCALL_THISCALL), "RegisterObjectMethod", __LINE__);
	reg->check( engine->RegisterObjectMethod("midi_output", "bool is_open() const", asMETHOD(midi_output, is_open), asCALL_THISCALL), "RegisterObjectMethod", __LINE__);
	reg->check( engine->RegisterObjectMethod("midi_output", "int get_port() const", asMETHOD(midi_output, get_port), asCALL_THISCALL), "RegisterObjectMethod", __LINE__);
	reg->check( engine->RegisterObjectMethod("midi_output", "int get_port_name_byte_count() const", asMETHOD(midi_output, get_port_name_byte_count), asCALL_THISCALL), "RegisterObjectMethod", __LINE__);
	reg->check( engine->RegisterObjectMethod("midi_output", "int get_port_name_byte(uint index) const", asMETHOD(midi_output, get_port_name_byte), asCALL_THISCALL), "RegisterObjectMethod", __LINE__);
	reg->check( engine->RegisterObjectMethod("midi_output", "bool send(uint status, uint data1, uint data2)", asMETHOD(midi_output, send), asCALL_THISCALL), "RegisterObjectMethod", __LINE__);
	reg->check( engine->RegisterObjectMethod("midi_output", "bool send_packed(uint packed)", asMETHOD(midi_output, send_packed), asCALL_THISCALL), "RegisterObjectMethod", __LINE__);
	reg->check( engine->RegisterObjectMethod("midi_output", "int send_transposed(const midi_message&in message, int semitones)", asMETHOD(midi_output, send_transposed), asCALL_THISCALL), "RegisterObjectMethod", __LINE__);
	reg->check( engine->RegisterObjectMethod("midi_output", "void send_note_on(uint channel, uint note, uint velocity)", asMETHOD(midi_output, send_note_on), asCALL_THISCALL), "RegisterObjectMethod", __LINE__);
	reg->check( engine->RegisterObjectMethod("midi_output", "void send_note_off(uint channel, uint note, uint velocity = 0)", asMETHOD(midi_output, send_note_off), asCALL_THISCALL), "RegisterObjectMethod", __LINE__);
	reg->check( engine->RegisterObjectMethod("midi_output", "void send_control_change(uint channel, uint controller, uint value)", asMETHOD(midi_output, send_control_change), asCALL_THISCALL), "RegisterObjectMethod", __LINE__);
	reg->check( engine->RegisterObjectMethod("midi_output", "void send_program_change(uint channel, uint program)", asMETHOD(midi_output, send_program_change), asCALL_THISCALL), "RegisterObjectMethod", __LINE__);
	reg->check( engine->RegisterObjectMethod("midi_output", "void send_pitch_bend(uint channel, uint value)", asMETHOD(midi_output, send_pitch_bend), asCALL_THISCALL), "RegisterObjectMethod", __LINE__);
	reg->check( engine->RegisterObjectMethod("midi_output", "void send_aftertouch(uint channel, uint note, uint pressure)", asMETHOD(midi_output, send_aftertouch), asCALL_THISCALL), "RegisterObjectMethod", __LINE__);
	reg->check( engine->RegisterObjectMethod("midi_output", "void send_channel_pressure(uint channel, uint pressure)", asMETHOD(midi_output, send_channel_pressure), asCALL_THISCALL), "RegisterObjectMethod", __LINE__);
	reg->check( engine->RegisterObjectMethod("midi_output", "void send_sysex(const string&in data)", asMETHOD(midi_output, send_sysex), asCALL_THISCALL), "RegisterObjectMethod", __LINE__);
	reg->check( engine->RegisterObjectMethod("midi_output", "void all_notes_off()", asMETHOD(midi_output, all_notes_off), asCALL_THISCALL), "RegisterObjectMethod", __LINE__);
	reg->check( engine->RegisterObjectMethod("midi_output", "void reset()", asMETHOD(midi_output, reset), asCALL_THISCALL), "RegisterObjectMethod", __LINE__);
	// A virtual port is a port this process publishes rather than one it
	// opens, and only ALSA (and JACK) offer the idea - on Windows a port must
	// belong to a driver. It is registered everywhere so a script that asks
	// the question compiles on every build; the answer is the truth on the
	// backend that is running, not on the one the script hopes for.
	//
	// While it is set, open() skips the port-index check, because the index
	// belongs to the port being created and there is nothing to enumerate
	// before it exists.
	reg->check( engine->RegisterObjectMethod("midi_output", "bool is_virtual_port() const", asMETHOD(midi_output, is_virtual_port), asCALL_THISCALL), "RegisterObjectMethod", __LINE__);
	reg->check( engine->RegisterObjectMethod("midi_output", "void set_virtual_port(bool value)", asMETHOD(midi_output, set_virtual_port), asCALL_THISCALL), "RegisterObjectMethod", __LINE__);
	reg->check( engine->RegisterObjectMethod("midi_output", "midi_duration duration(double amount, int unit) const", asMETHOD(midi_output, duration), asCALL_THISCALL), "RegisterObjectMethod", __LINE__);
	reg->check( engine->RegisterObjectProperty("midi_output", "double tempo", asOFFSET(midi_output, tempo)), "RegisterObjectProperty", __LINE__);
}

void register_midi_note(asIScriptEngine* engine, registration* reg) {
	// A value type, and it has to stay one: a handle can only be formed for a
	// type flagged asOBJ_REF, asOBJ_TEMPLATE_SUBTYPE, asOBJ_ASHANDLE or
	// asOBJ_FUNCDEF (asCDataType::MakeHandle in the SDK), so every midi_duration
	// below is passed and returned by value. asOBJ_POD is deliberately not set -
	// it is the traits flag plus a promise that the type is bitwise copyable,
	// which a midi_duration with a std::string to_string() is not.
	reg->check( engine->RegisterObjectType("midi_duration", sizeof(midi_duration), asOBJ_VALUE | asGetTypeTraits<midi_duration>()), "RegisterObjectType", __LINE__);
	reg->check( engine->RegisterObjectBehaviour("midi_duration", asBEHAVE_CONSTRUCT, "void f()", asFUNCTION(midi_duration_default_construct), asCALL_CDECL_OBJLAST), "RegisterObjectBehaviour", __LINE__);
	reg->check( engine->RegisterObjectBehaviour("midi_duration", asBEHAVE_CONSTRUCT, "void f(double amount, int unit)", asFUNCTION(midi_duration_construct), asCALL_CDECL_OBJLAST), "RegisterObjectBehaviour", __LINE__);
	reg->check( engine->RegisterObjectBehaviour("midi_duration", asBEHAVE_CONSTRUCT, "void f(double amount, int unit, double tempo)", asFUNCTION(midi_duration_construct_tempo), asCALL_CDECL_OBJLAST), "RegisterObjectBehaviour", __LINE__);
	reg->check( engine->RegisterObjectBehaviour("midi_duration", asBEHAVE_DESTRUCT, "void f()", asFUNCTION(midi_duration_destruct), asCALL_CDECL_OBJLAST), "RegisterObjectBehaviour", __LINE__);
	reg->check( engine->RegisterObjectProperty("midi_duration", "double amount", asOFFSET(midi_duration, amount)), "RegisterObjectProperty", __LINE__);
	reg->check( engine->RegisterObjectProperty("midi_duration", "int unit", asOFFSET(midi_duration, unit)), "RegisterObjectProperty", __LINE__);
	reg->check( engine->RegisterObjectProperty("midi_duration", "double tempo", asOFFSET(midi_duration, tempo)), "RegisterObjectProperty", __LINE__);
	reg->check( engine->RegisterObjectProperty("midi_duration", "double ppq", asOFFSET(midi_duration, ppq)), "RegisterObjectProperty", __LINE__);
	reg->check( engine->RegisterObjectMethod("midi_duration", "double to_ms() const", asMETHOD(midi_duration, to_ms), asCALL_THISCALL), "RegisterObjectMethod", __LINE__);
	reg->check( engine->RegisterObjectMethod("midi_duration", "midi_duration& opAssign(const midi_duration&in other)", asMETHODPR(midi_duration, opAssign, (const midi_duration&), midi_duration&), asCALL_THISCALL), "RegisterObjectMethod", __LINE__);
	reg->check( engine->RegisterObjectMethod("midi_duration", "midi_duration& opAssign(double amount)", asMETHODPR(midi_duration, opAssign, (double), midi_duration&), asCALL_THISCALL), "RegisterObjectMethod", __LINE__);
	// By value, not midi_duration@ - see the comment above the type.
	reg->check( engine->RegisterGlobalFunction("midi_duration midi_duration_create()", asFUNCTION(midi_duration_create), asCALL_CDECL), "RegisterGlobalFunction", __LINE__);
	reg->check( engine->RegisterGlobalFunction("midi_duration midi_duration_create(double amount, int unit)", asFUNCTION(midi_duration_create_full), asCALL_CDECL), "RegisterGlobalFunction", __LINE__);
	reg->check( engine->RegisterGlobalFunction("midi_duration midi_duration_create(double amount, int unit, double tempo)", asFUNCTION(midi_duration_create_tempo), asCALL_CDECL), "RegisterGlobalFunction", __LINE__);

	// A handle type: a script writes note@ n = midi_note_create(60, 100); and the
	// handle points at the object rather than copying it, which is what makes
	// changing n.velocity later actually change the note that is played.
	reg->check( engine->RegisterObjectType("midi_note", 0, asOBJ_REF | asOBJ_NOCOUNT), "RegisterObjectType", __LINE__);
	reg->check( engine->RegisterObjectProperty("midi_note", "int pitch", asOFFSET(midi_note, pitch)), "RegisterObjectProperty", __LINE__);
	reg->check( engine->RegisterObjectProperty("midi_note", "int velocity", asOFFSET(midi_note, velocity)), "RegisterObjectProperty", __LINE__);
	reg->check( engine->RegisterObjectProperty("midi_note", "int channel", asOFFSET(midi_note, channel)), "RegisterObjectProperty", __LINE__);
	reg->check( engine->RegisterObjectProperty("midi_note", "midi_duration length", asOFFSET(midi_note, length)), "RegisterObjectProperty", __LINE__);
	reg->check( engine->RegisterObjectProperty("midi_note", "double duration_ms", asOFFSET(midi_note, length) + offsetof(midi_duration, amount)), "RegisterObjectProperty", __LINE__);
	// A factory per arity, because the engine will not give this plugin the
	// name midi_note: nvgt is an audio toolkit with its own midi support and
	// its engine already owns that name, so RegisterGlobalFunction reports
	// asNAME_TAKEN (-9) and the whole interface is rejected. The names below
	// are the ones doc/api.md told scripts to call all along.
	// asFUNCTIONPR is required for the overloaded C++ helper: an overloaded
	// name cannot be resolved by asFUNCTION, only by its parameter list.
	// The name the engine will not give us is the bare `midi_note` - it already
	// owns that word - but the *constructor* names came back rejected too, and
	// only at run time on a real machine: RegisterGlobalFunction reports
	// asNAME_TAKEN (-9) for `midi_note_create` as well, because nvgt declares
	// its own midi_note there before the plugin registers anything. A rejected
	// registration is not fatal, so the plugin loaded and every script then
	// failed with "No matching signatures to 'midi_note_create(int)'" - the
	// name was simply never created. Measured on Дениз's windows build
	// (play_chord.nvgt, 28.09). The factories therefore live under nvmidi_'s
	// own prefix, which nothing in the engine owns.
	reg->check( engine->RegisterGlobalFunction("midi_note@ nvmidi_note_create()", asFUNCTION(midi_note_create), asCALL_CDECL), "RegisterGlobalFunction", __LINE__);
	reg->check( engine->RegisterGlobalFunction("midi_note@ nvmidi_note_create(int pitch)", asFUNCTIONPR(midi_note_create_pitch, (int), midi_note*), asCALL_CDECL), "RegisterGlobalFunction", __LINE__);
	reg->check( engine->RegisterGlobalFunction("midi_note@ nvmidi_note_create(int pitch, int velocity)", asFUNCTIONPR(midi_note_create_velocity, (int, int), midi_note*), asCALL_CDECL), "RegisterGlobalFunction", __LINE__);
	reg->check( engine->RegisterGlobalFunction("midi_note@ nvmidi_note_create(int pitch, int velocity, int channel)", asFUNCTION(midi_note_create_full), asCALL_CDECL), "RegisterGlobalFunction", __LINE__);
	reg->check( engine->RegisterGlobalFunction("midi_note@ nvmidi_note_create_ms(int pitch, int velocity, double duration_ms)", asFUNCTION(midi_note_create_ms), asCALL_CDECL), "RegisterGlobalFunction", __LINE__);
	reg->check( engine->RegisterGlobalFunction("int midi_note_number(const string&in name)", asFUNCTION(midi_note_number), asCALL_CDECL), "RegisterGlobalFunction", __LINE__);
	reg->check( engine->RegisterGlobalFunction("int midi_note_name_byte_count(int pitch)", asFUNCTION(midi_note_pitch_name_byte_count), asCALL_CDECL), "RegisterGlobalFunction", __LINE__);
	reg->check( engine->RegisterGlobalFunction("int midi_note_name_byte(int pitch, uint index)", asFUNCTION(midi_note_pitch_name_byte), asCALL_CDECL), "RegisterGlobalFunction", __LINE__);

	// Unit constants, so a script never has to remember 0..3.
	reg->check( engine->RegisterGlobalProperty("const int MIDI_MS", (void*)&g_unit_ms), "RegisterGlobalProperty", __LINE__);
	reg->check( engine->RegisterGlobalProperty("const int MIDI_TICKS", (void*)&g_unit_ticks), "RegisterGlobalProperty", __LINE__);
	reg->check( engine->RegisterGlobalProperty("const int MIDI_BEATS", (void*)&g_unit_beats), "RegisterGlobalProperty", __LINE__);
	reg->check( engine->RegisterGlobalProperty("const int MIDI_BARS", (void*)&g_unit_bars), "RegisterGlobalProperty", __LINE__);
	// The musical units. The plain four mean the same numbers as MIDI_*, so a
	// script written for one spelling keeps working with the other; the
	// MUSIC_BEATS_NN constants are extra values that carry their own tempo.
	reg->check( engine->RegisterGlobalProperty("const int MUSIC_MS", (void*)&g_music_ms), "RegisterGlobalProperty", __LINE__);
	reg->check( engine->RegisterGlobalProperty("const int MUSIC_TICKS", (void*)&g_music_ticks), "RegisterGlobalProperty", __LINE__);
	reg->check( engine->RegisterGlobalProperty("const int MUSIC_BEATS", (void*)&g_music_beats), "RegisterGlobalProperty", __LINE__);
	reg->check( engine->RegisterGlobalProperty("const int MUSIC_BARS", (void*)&g_music_bars), "RegisterGlobalProperty", __LINE__);
	reg->check( engine->RegisterGlobalProperty("const int MUSIC_BEATS_90", (void*)&g_music_beats_90), "RegisterGlobalProperty", __LINE__);
	reg->check( engine->RegisterGlobalProperty("const int MUSIC_BEATS_100", (void*)&g_music_beats_100), "RegisterGlobalProperty", __LINE__);
	reg->check( engine->RegisterGlobalProperty("const int MUSIC_BEATS_120", (void*)&g_music_beats_120), "RegisterGlobalProperty", __LINE__);
	reg->check( engine->RegisterGlobalProperty("const int MUSIC_BEATS_140", (void*)&g_music_beats_140), "RegisterGlobalProperty", __LINE__);
	// What send_transposed did. Globals rather than an enum because the engine
	// this host builds has no string type and no enum registration is used
	// anywhere in this file; the constants are what a script can compare an
	// int against, which is the use.
	reg->check( engine->RegisterGlobalProperty("const int TRANSPOSE_SENT", (void*)&g_transpose_sent), "RegisterGlobalProperty", __LINE__);
	reg->check( engine->RegisterGlobalProperty("const int TRANSPOSE_PASSED_THROUGH", (void*)&g_transpose_passed), "RegisterGlobalProperty", __LINE__);
	reg->check( engine->RegisterGlobalProperty("const int TRANSPOSE_OUT_OF_RANGE", (void*)&g_transpose_range), "RegisterGlobalProperty", __LINE__);
	reg->check( engine->RegisterGlobalProperty("const int TRANSPOSE_FAILED", (void*)&g_transpose_failed), "RegisterGlobalProperty", __LINE__);
}

void register_midi_config(asIScriptEngine* engine, registration* reg) {
	// A handle type, like the rest: midi_config@ c = midi_config_create().
	reg->check( engine->RegisterObjectType("midi_config", 0, asOBJ_REF | asOBJ_NOCOUNT), "RegisterObjectType", __LINE__);
	reg->check( engine->RegisterObjectProperty("midi_config", "string match", asOFFSET(midi_config, match)), "RegisterObjectProperty", __LINE__);
	reg->check( engine->RegisterObjectProperty("midi_config", "int port", asOFFSET(midi_config, port)), "RegisterObjectProperty", __LINE__);
	reg->check( engine->RegisterObjectMethod("midi_config", "bool load(const string&in path)", asMETHOD(midi_config, load), asCALL_THISCALL), "RegisterObjectMethod", __LINE__);
	reg->check( engine->RegisterObjectMethod("midi_config", "bool load_if_present(const string&in path)", asMETHOD(midi_config, load_if_present), asCALL_THISCALL), "RegisterObjectMethod", __LINE__);
	reg->check( engine->RegisterObjectMethod("midi_config", "int find_input_port() const", asMETHOD(midi_config, find_input_port), asCALL_THISCALL), "RegisterObjectMethod", __LINE__);
	reg->check( engine->RegisterObjectMethod("midi_config", "int find_output_port() const", asMETHOD(midi_config, find_output_port), asCALL_THISCALL), "RegisterObjectMethod", __LINE__);
	reg->check( engine->RegisterObjectMethod("midi_config", "int get_last_port() const", asMETHOD(midi_config, get_last_port), asCALL_THISCALL), "RegisterObjectMethod", __LINE__);
	reg->check( engine->RegisterObjectMethod("midi_config", "int describe_byte_count() const", asMETHOD(midi_config, describe_byte_count), asCALL_THISCALL), "RegisterObjectMethod", __LINE__);
	reg->check( engine->RegisterObjectMethod("midi_config", "int describe_byte(uint index) const", asMETHOD(midi_config, describe_byte), asCALL_THISCALL), "RegisterObjectMethod", __LINE__);
	reg->check( engine->RegisterObjectMethod("midi_config", "int get_path_byte_count() const", asMETHOD(midi_config, get_path_byte_count), asCALL_THISCALL), "RegisterObjectMethod", __LINE__);
	reg->check( engine->RegisterObjectMethod("midi_config", "int get_path_byte(uint index) const", asMETHOD(midi_config, get_path_byte), asCALL_THISCALL), "RegisterObjectMethod", __LINE__);
	reg->check( engine->RegisterGlobalFunction("midi_config@ midi_config_create()", asFUNCTION(midi_config_create), asCALL_CDECL), "RegisterGlobalFunction", __LINE__);
	reg->check( engine->RegisterGlobalFunction("int midi_find_input_port(const string&in substring)", asFUNCTION(midi_find_input_port), asCALL_CDECL), "RegisterGlobalFunction", __LINE__);
	reg->check( engine->RegisterGlobalFunction("int midi_find_output_port(const string&in substring)", asFUNCTION(midi_find_output_port), asCALL_CDECL), "RegisterGlobalFunction", __LINE__);
}

// The two globals a script actually holds.
//
// midi_input and midi_output are registered as handle types (asOBJ_REF |
// asOBJ_NOCOUNT, size 0) - the same shape midi_note uses - and a handle type
// with no factory behaviour and no global already holding one cannot be
// *declared* by a script at all: the compiler answers "Data type can't be
// 'midi_output'", measured in the host harness on 30.09. The create functions
// are not the route either: they are named midi_output_create, which is a
// function, not the type. One instance of each, registered as a global
// property, is what turns midi_in and midi_out into symbols a script can open,
// send on and read.
//
// The objects are made once and point at the plugin's own midi_input/midi_output
// structs, which hold their own state (the queue, the open port). Nothing here
// owns them, which is exactly what asOBJ_NOCOUNT means - RegisterGlobalProperty
// stores the address and never releases it.
static midi_input*  g_script_input  = midi_input_create();
static midi_output* g_script_output = midi_output_create();

void register_midi_globals(asIScriptEngine* engine, registration* reg) {
	reg->check( engine->RegisterGlobalFunction("uint midi_input_port_count()", asFUNCTION(midi_input_port_count), asCALL_CDECL), "RegisterGlobalFunction", __LINE__);
	reg->check( engine->RegisterGlobalFunction("uint midi_output_port_count()", asFUNCTION(midi_output_port_count), asCALL_CDECL), "RegisterGlobalFunction", __LINE__);
	// Why every string-returning function in this plugin hands the script
	// garbage, measured on the windows runner rather than reasoned about.
	//
	// RtMidi is clean. Printed from inside midi_output_port_name, which is
	// itself one of the broken calls:
	//
	//     NVPORT port=0 count=1 len=30
	//     bytes=4d 69 63 72 6f 73 6f 66 74 20 47 53 20 57 61 76 65 74 61 62 6c 65 20 53 79 6e 74 68 20 30
	//     raw=[Microsoft GS Wavetable Synth 0]
	//
	// 30 bytes, correct, inside the plugin. The same value reaches the script
	// as `??9??^A^@^@...`. So the characters are fine and the marshalling is
	// not: Angelscript copies a returned string in the ABI the script's own
	// `string` uses, which it learns from the engine. When the engine and the
	// plugin disagree about that ABI the result is not an error and not a null
	// - it is quietly wrong bytes, which is exactly what this looks like.
	//
	// The two facts below are what settles which half disagrees, and they cost
	// nothing: the engine either exposes a string factory through the vtable the
	// plugin calls, or it does not, and the plugin was compiled with a
	// different Angelscript version than the engine was. Both are printed, so
	// one run names the fault instead of two runs guessing at it.
	// Measure the thing the sweep below depends on, and stop relying on it.
	//
	// The string-factory number that used to be printed here is the evidence
	// for the whole change: the engine answers 67108876, which as a signed int
	// is -2, asINVALID_ARG - there is no string factory to convert with. That
	// is why every string this plugin returned arrived as bytes and, once the
	// port name was asked for, took the process down. It is asserted rather
	// than printed now: if a future engine ever grows a factory the assertion
	// fires and says so, instead of the sweep below silently becoming wrong.
	{
		asDWORD mods = 0;
		asIStringFactory* factory = nullptr;
		const int r = engine->GetStringFactory(&mods, &factory);
		if (r == 0) {
			std::cerr << "nvmidi: the engine now publishes a string factory ("
			          << ANGELSCRIPT_VERSION_STRING << "), so plugin-returned strings "
			          << "may work directly and the byte-by-byte path in this file is "
			          << "no longer necessary. Re-measure before trusting either."
			          << std::endl;
		}
	}
	// The same value, handed over as bytes instead of as a string. See the
	// definition above for what the const char* attempt measured before this
	// replaced it: the return type was not the fault, so no spelling of a
	// string return is registered here any more.
	reg->check( engine->RegisterGlobalFunction("int midi_output_port_name_byte_count(uint port)", asFUNCTION(midi_output_port_name_byte_count), asCALL_CDECL), "RegisterGlobalFunction", __LINE__);
	reg->check( engine->RegisterGlobalFunction("int midi_output_port_name_byte(uint port, uint index)", asFUNCTION(midi_output_port_name_byte), asCALL_CDECL), "RegisterGlobalFunction", __LINE__);
	// Every string this plugin hands out, exposed the same way: a length and an
	// indexed byte, so a string never crosses this boundary at all. This is the
	// only place any of them is registered - the object methods above were
	// removed rather than kept beside these, because registering both is how
	// this block produced six duplicate-declaration errors on a runner: the
	// engine rejects the whole interface over a duplicate, so a second copy is
	// not a harmless redundancy, it is a plugin that will not load.
	reg->check( engine->RegisterObjectMethod("midi_message", "int to_string_byte_count() const", asMETHOD(midi_message, to_string_byte_count), asCALL_THISCALL), "RegisterObjectMethod", __LINE__);
	reg->check( engine->RegisterObjectMethod("midi_message", "int to_string_byte(uint index) const", asMETHOD(midi_message, to_string_byte), asCALL_THISCALL), "RegisterObjectMethod", __LINE__);
	reg->check( engine->RegisterObjectMethod("midi_duration", "int to_string_byte_count() const", asMETHOD(midi_duration, to_string_byte_count), asCALL_THISCALL), "RegisterObjectMethod", __LINE__);
	reg->check( engine->RegisterObjectMethod("midi_duration", "int to_string_byte(uint index) const", asMETHOD(midi_duration, to_string_byte), asCALL_THISCALL), "RegisterObjectMethod", __LINE__);
	reg->check( engine->RegisterObjectMethod("midi_note", "int to_string_byte_count() const", asMETHOD(midi_note, to_string_byte_count), asCALL_THISCALL), "RegisterObjectMethod", __LINE__);
	reg->check( engine->RegisterObjectMethod("midi_note", "int to_string_byte(uint index) const", asMETHOD(midi_note, to_string_byte), asCALL_THISCALL), "RegisterObjectMethod", __LINE__);
	reg->check( engine->RegisterGlobalFunction("int midi_input_port_name_byte_count(uint port)", asFUNCTION(midi_input_port_name_byte_count), asCALL_CDECL), "RegisterGlobalFunction", __LINE__);
	reg->check( engine->RegisterGlobalFunction("int midi_input_port_name_byte(uint port, uint index)", asFUNCTION(midi_input_port_name_byte), asCALL_CDECL), "RegisterGlobalFunction", __LINE__);
	reg->check( engine->RegisterGlobalFunction("int midi_api_name_byte_count()", asFUNCTION(midi_api_name_byte_count), asCALL_CDECL), "RegisterGlobalFunction", __LINE__);
	reg->check( engine->RegisterGlobalFunction("int midi_api_name_byte(uint index)", asFUNCTION(midi_api_name_byte), asCALL_CDECL), "RegisterGlobalFunction", __LINE__);
	reg->check( engine->RegisterGlobalFunction("int midi_message_name_byte_count(const midi_message&in m)", asFUNCTION(midi_message_name_byte_count), asCALL_CDECL), "RegisterGlobalFunction", __LINE__);
	reg->check( engine->RegisterGlobalFunction("int midi_message_name_byte(const midi_message&in m, uint index)", asFUNCTION(midi_message_name_byte), asCALL_CDECL), "RegisterGlobalFunction", __LINE__);
	reg->check( engine->RegisterGlobalFunction("int midi_last_error_byte_count()", asFUNCTION(midi_last_error_byte_count), asCALL_CDECL), "RegisterGlobalFunction", __LINE__);
	reg->check( engine->RegisterGlobalFunction("int midi_last_error_byte(uint index)", asFUNCTION(midi_last_error_byte), asCALL_CDECL), "RegisterGlobalFunction", __LINE__);
	reg->check( engine->RegisterGlobalFunction("int midi_first_error_byte_count()", asFUNCTION(midi_first_error_byte_count), asCALL_CDECL), "RegisterGlobalFunction", __LINE__);
	reg->check( engine->RegisterGlobalFunction("int midi_first_error_byte(uint index)", asFUNCTION(midi_first_error_byte), asCALL_CDECL), "RegisterGlobalFunction", __LINE__);
		reg->check( engine->RegisterGlobalProperty("midi_input midi_in",  g_script_input),  "RegisterGlobalProperty", __LINE__);
	reg->check( engine->RegisterGlobalProperty("midi_output midi_out", g_script_output), "RegisterGlobalProperty", __LINE__);
	reg->check( engine->RegisterGlobalFunction("midi_input@ midi_input_create()", asFUNCTION(midi_input_create), asCALL_CDECL), "RegisterGlobalFunction", __LINE__);
	reg->check( engine->RegisterGlobalFunction("midi_output@ midi_output_create()", asFUNCTION(midi_output_create), asCALL_CDECL), "RegisterGlobalFunction", __LINE__);
	reg->check( engine->RegisterGlobalFunction("int midi_engine_probe()", asFUNCTION(midi_engine_probe), asCALL_CDECL), "RegisterGlobalFunction", __LINE__);
}


// Declared before use and defined below, next to the registration code whose
// result it reads; the body is long and belongs with that code, not here.


static int probe_registered_types(asIScriptEngine* engine);

// See the declaration in nvmidi.h for what the three counts are. The refusal
// count it returns is every refusal whatever the code.
registration_result register_nvmidi(asIScriptEngine* engine) {
	// The registration helpers below take no engine argument, so the reporter
	// finds it here.
	g_registration_engine = engine;
	// Before the first registration call. This line proves the function was
	// entered at all, which is the branch between "the fault is in the
	// registration chain" and "the fault is before it, in prepare_plugin or
	// the handover of the script engine". Measured need, not symmetry: the
	// whole reason the load looked like a version mismatch for so long is
	// that nothing in this window ever reached a log.
	fprintf(stderr, "nvmidi: entering register_nvmidi\n");
	fflush(stderr);
	g_engine = engine;
	registration reg(engine);

	// The array add-on, before any type that names array<> in a signature.
	//
	// The mechanism: RegisterObjectMethod parses the signature it is given, and
	// a signature naming array<...> fails unless the engine already knows the
	// type. Nothing registers it - this header includes scriptarray.h and the
	// Makefile links scriptarray.cpp into the dll, but the call was never made,
	// so the add-on sat inert. Each array-bearing registration returned a
	// negative code and, by the rule the comment further down spells out, the
	// RegisterObjectType that follows went down with it.
	//
	// The engine hands plugins no array registration of its own, so this is
	// the plugin's to do. false, not true: the "default array type" spelling
	// registers array as a keyword rather than a template, and every script
	// here writes array<midi_note@>.
	//
	// Alive but not sufficient, and the reason recorded here was not measured.
	// It said a plugin api version mismatch (this header's 5 against an
	// engine's 6) kept every type out of every script. The only ground for
	// that number was plug_api.c's blob, which sets blob.version = v + 1
	// itself before calling the entry point - so "the engine says 6" was the
	// harness reading back its own constant, not the engine's answer. What is
	// measured is narrower: run 36673402079 refused the probe n_noteonly.nvgt
	// with "Identifier 'midi_note' is not a data type", and midi_note's
	// registration names no array at all, so the array add-on is not the
	// whole story. It is not the version either until a runner prints
	// shared->version back, which plugin_main now does before prepare_plugin
	// compares it.
	//
	// Behind a flag because this is now the suspect, measured rather than
	// reasoned. Run 36700487540 put a flushed line before every registration
	// and one on entry here. The log carries "entering register_nvmidi" and
	// then nothing - no "reg line" at all, so control never reached the first
	// check() below this point - and the process exits 70. The only statement
	// between the entry marker and the first check() is this call. It is the
	// last thing that runs before the death.
	//
	// Does this engine already know array<T>?
	//
	// This is the only call site of the add-on, and it is conditional because
	// the unconditional one this replaces was what killed the process.
	//
	// Settled: the add-on must not run over an engine that already has the
	// type. Run 36721024269 pinned the death to the call itself - the same
	// build with the call removed registered every function this plugin has
	// and lived, and the same process answered a property read through the
	// plugin's own engine pointer, so the engine and its vtable are sound.
	// NVGT's own loader installs an array add-on before any plugin loads; the
	// plugin's call was therefore the second registration, and
	// RegisterScriptArray_Native's first statement is a cleanup-callback
	// install over a cache the engine had already built
	// (scriptarray.cpp:292, ARRAY_CACHE at line 52).
	//
	// Run 36721884305 is the measurement that closes it: with the condition
	// below in place the ordinary build exits 0 and prints its markers at all
	// four load locations, where before it exited 70 with an empty stdout.
	//
	// A second copy of the unconditional call used to sit just above this
	// comment, behind the same flag the control build uses. It is gone: the
	// control build does not need it (the flag is read inside
	// engine_knows_arrays), and leaving it would have made every branch below
	// dead code while still killing any engine that answered "no".
	//
	// A marker pair around the call, because the fault that brought this
	// harness here (si_addr=0x55b1785c9, a four-and-a-half-byte pointer, from
	// the kernel's own si_addr rather than from a core file) lands *after*
	// "RegisterScriptArray returned" and before anything else prints, and
	// "after" is all that single marker can say. Two markers say whether the
	// call returned into this function at all, and the crumb below says where
	// the next statement went.
	fprintf(stderr, "nvmidi: ARRAY_BRANCH_ENTER\n");
	fflush(stderr);
	if (reg.engine_knows_arrays() && reg.array_is_usable()) {
		fprintf(stderr, "nvmidi: the engine already has array<T> that resolves, the array add-on is not called\n");
		fflush(stderr);
	} else {
		fprintf(stderr, "nvmidi: the array add-on must supply array<T>, calling RegisterScriptArray\n");
		fflush(stderr);
		RegisterScriptArray(engine, false);
		// A second literal after the call, so that this build and the control
		// can be told apart by a string that exists in a known build and in no
		// other. The pair around the call cannot do it: both of its lines sit
		// in the branch the control compiles away, and the control's branch has
		// a line of its own - so counting any one of them is the same
		// question, and it went wrong. Measured, run 36753050043:
		//
		//   CALLING-MARKER with-addon=1 no-addon=1
		//
		// two different builds, one marker each - which the workflow read as the
		// control carrying the add-on ("carries the add-on call, so it is not
		// the control") and threw. The count was right about the control and
		// wrong about the pair: the control does carry that string, because it
		// is a literal in a file that links scriptarray.o and holds it twice
		// over, in this branch that is never reached and in the text the
		// registration itself passes to the engine.
		//
		// So what the bytes are asked is a question the reachability of the
		// branch does not affect and the add-on's own copies do not answer:
		// a returned-call literal that is this line and nowhere else.
		fprintf(stderr, "nvmidi: RegisterScriptArray returned\n");
		fflush(stderr);
	}
	fprintf(stderr, "nvmidi: ARRAY_BRANCH_DONE\n");
	fflush(stderr);

	register_midi_message(engine, &reg);
	fprintf(stderr, "nvmidi: crumb: register_midi_message done\n");
	fflush(stderr);
	// Every type a later declaration names has to exist first. The port
	// classes' open_config takes a midi_config@, and their playing methods
	// take midi_note and midi_duration, so all three come before them. The
	// other way round - which is how this was written - the engine refuses
	// open_config with "Identifier 'midi_config' is not a data type" and the
	// refusal aborts the rest of the function, taking the whole port class
	// with it.
	//
	// midi_config first of the three, and the reason is the same rule read one
	// step further on: register_midi_output registers play_chord, which takes an
	// array of midi_note, and above these registrations it was failing.
	// Measured, run 36615690048: the n_type
	// probe asked for every name this plugin registers as a declared type and
	// the engine refused exactly two of them - midi_input@ and midi_output@,
	// "Expected ';'" / "Instead found '@'" at those two lines - while
	// midi_message, midi_duration, midi_note@ and midi_config@ all declared
	// cleanly. Both names that were refused are the ones whose methods are
	// registered here, so the type registration in this function is the
	// suspect, and a failed method registration already in the log is the best
	// candidate: one has to say which one before anything is changed blind.
	register_midi_note(engine, &reg);
	fprintf(stderr, "nvmidi: crumb: register_midi_note done\n");
	fflush(stderr);
	register_midi_config(engine, &reg);
	fprintf(stderr, "nvmidi: crumb: register_midi_config done\n");
	fflush(stderr);
	register_midi_input(engine, &reg);
	fprintf(stderr, "nvmidi: crumb: register_midi_input done (calls=%d refused=%d)\n", reg.calls, reg.refused);
	fflush(stderr);
	register_midi_output(engine, &reg);
	fprintf(stderr, "nvmidi: crumb: register_midi_output done (calls=%d refused=%d)\n", reg.calls, reg.refused);
	fflush(stderr);
	// The free search functions live in the config section and take a plain
	// string, so they follow whatever needs them.
	register_midi_globals(engine, &reg);
	fprintf(stderr, "nvmidi: crumb: register_midi_globals done\n");
	fflush(stderr);

	// The engine is asked what it kept before this returns, because a code is a
	// claim about a call and not about what survived - see the probe below.
	registration_result result;
	result.calls = reg.calls;
	result.refused = reg.refused;
	// The engine is asked what it kept before this returns, because a code is a
	// claim about a call and not about what survived: every call above may have
	// been refused, or accepted into a list that does not name them, and only
	// the engine's own answer tells those apart.
	result.registered = probe_registered_types(engine);
	return result;
}

static int probe_registered_types(asIScriptEngine* engine) {
	// Did any of that survive? Every registration above returned a code and
	// every code was reported, but a code is a claim about a call and not about
	// what the engine kept, and the two readings of -10 cannot be told apart
	// from the codes alone: "this name is already taken" and "the type this
	// belongs to was rolled back" are the same number. So the engine is asked
	// directly, at the end, for each name this plugin meant to own.
	//
	// This is the question a script asks when it writes `midi_output@ out;` and
	// the engine answers "Expected ';' / Instead found '@'". Asking it here
	// moves that answer to the loading side, where the name that failed is a
	// fact rather than something to reconstruct from a script error - and it is
	// the same call the script would make, through the same interface, so a yes
	// here is the yes the script gets.
	//
	// GetTypeInfoByName and not GetTypeIdByDecl: the first is one call and
	// cannot invoke the engine's allocator, while a declaration is parsed and
	// the parser is where the "Expected ';'" this is chasing comes from. No
	// temporaries are constructed, and the returned asITypeInfo is released the
	// moment it is read - the engine refcounts by 0 on AddRef and by 1 on
	// Release, so holding one without releasing leaks the type, and releasing
	// one the engine has already freed corrupts the heap.
	//
	// The control is a name this plugin never registers. Without it a run where
	// every answer is "no" reads as a plugin that registered nothing, when the
	// likelier cause is that the question itself does not work on this engine -
	// and the two have different fixes.
	struct { const char* name; bool required; } expected[] = {
		{ "midi_message", true },
		{ "midi_note", true },
		{ "midi_duration", true },
		{ "midi_input", true },
		{ "midi_output", true },
		{ "midi_config", true },
		{ "no_such_type_this_plugin_renamed", false },
};
	int survived = 0, missing = 0, control_known = 0;
	std::string missing_names;
	for (size_t i = 0; i < sizeof(expected) / sizeof(expected[0]); ++i) {
		asITypeInfo* t = engine->GetTypeInfoByName(expected[i].name);
		const bool known = t != nullptr;
		if (t) t->Release();
		if (!expected[i].required) {
			control_known = known ? 1 : 0;
			continue;
		}
		if (known) {
			survived += 1;
		} else {
			missing += 1;
			if (!missing_names.empty()) missing_names += ", ";
			missing_names += expected[i].name;
		}
}
	fprintf(stderr, "nvmidi: engine type probe: %d of %d registered types are in the engine",
		survived, survived + missing);
	if (missing) fprintf(stderr, " - MISSING: %s", missing_names.c_str());
	fprintf(stderr, " (control name %s)\n",
		control_known ? "IS known, so this probe is answering yes to anything" : "is unknown, as it must be");
	fflush(stderr);
	if (control_known) {
		// Every "yes" above is now worthless: a probe that finds a name nothing
		// registered is not reading the engine's type list.
		fprintf(stderr, "nvmidi: the control name was found, so the answers above prove nothing\n");
		fflush(stderr);
}
	// Counted rather than only printed. A function whose entire effect is
	// fprintf may be treated by the compiler as reorderable against the engine
	// calls around it, and this probe is exactly that shape - the answer it
	// reads is the one thing a script's own `midi_output@ out;` depends on, so
	// it is returned and reported by plugin_main's trailer rather than left to
	// be deduced from a log line that optimisation is free to move.
	return survived;
}

midi_input* midi_input_create() { return new midi_input(); }
midi_output* midi_output_create() { return new midi_output(); }
midi_config* midi_config_create() { return new midi_config(); }

std::string midi_last_error() { return g_last_error; }

// What went wrong first, kept because g_last_error is overwritten by every
// later failure and the first one is usually the cause of the rest.
//
// This is a convenience for a script that got the plugin loaded and is
// debugging its own run; it is not a way to report a library that never
// loaded. It cannot be: with "#pragma plugin nvmidi" present and the library
// missing, the engine stops at the pragma and no script code runs at all -
// measured on the engine, including with a script-side midi_first_error()
// declared to catch exactly that. See the readme's "Saying why it failed".
std::string midi_first_error() {
	if (!g_first_error.empty()) return g_first_error;
	return g_last_error;
}

// ---------------------------------------------------------------------------
// Plugin entry point
// ---------------------------------------------------------------------------

plugin_main(nvgt_plugin_shared* shared) {
	// A null table is refused before anything is read out of it.
	//
	// This line exists because the ci probe calls the entry point with a table
	// of zeroes, and the version line below reads shared->version before any
	// check - so the probe measured a dereference of address 0 rather than the
	// api-version answer it was written for (measured on windows-latest,
	// 30.09: "access violation writing 0x0000000000000000" instead of a
	// refusal). A pointer that is null and a struct that is zeroed are two
	// different faults and the probe was meant to ask about the second.
	if (!shared) {
		fprintf(stderr, "nvmidi: the engine passed no plugin table at all\n");
		fflush(stderr);
		return false;
	}
	// The engine's api version, printed BEFORE the comparison and on stderr.
	//
	// Why before. prepare_plugin refuses a mismatch at nvgt_plugin.h:157 and
	// returns false without touching a single function pointer, so the line
	// below it cannot run on a mismatch - and a mismatch is exactly the case
	// this line exists to measure. Printing it here is also the only way to
	// tell the two readings apart: either the engine's own constant differs
	// from the 5 in this header, or the engine rewrites the version field
	// after setting it (nvgt_plugin.h:182). The number settles which.
	//
	// stderr, not stdout: the crash that brings us here loses a buffered
	// stdout (measured - logs_e2e.txt is 0 bytes because E2E_BEGIN is still
	// in the buffer when the process dies), and fflush is called in the same
	// breath because a line written and not flushed is a line not written.
	fprintf(stderr, "nvmidi: engine plugin api version %d, this plugin built against %d\n",
		shared->version, NVGT_PLUGIN_API_VERSION);
	fflush(stderr);
	if (!prepare_plugin(shared)) {
		fprintf(stderr, "nvmidi: the engine's plugin api version is %d, this plugin was built against %d\n",
			shared->version, NVGT_PLUGIN_API_VERSION);
		fflush(stderr);
		return false;
	}
	if (!shared->script_engine) {
		fprintf(stderr, "nvmidi: the engine handed over no script engine, so no type could be registered\n");
		fflush(stderr);
		return false;
	}
	const registration_result reg = register_nvmidi(shared->script_engine);
	// Printed whether or not anything was refused, and with the count, so that
	// "the engine took everything" and "the engine refused things and the
	// lines above say which" are two readings a runner's log can tell apart.
	//
	// The count is the whole point of the line and the first version of it left
	// the count out. It said, whenever g_registration_failed was set, that at
	// least one refusal had arrived for a reason other than asALREADY_REGISTERED
	// - and in the run that wrote this, all twenty-two refusals were -10, which
	// is not that code at all, so the sentence was doubly wrong. The latch
	// itself is still the right instrument: it is set on the first refusal and
	// never cleared, so it can never be rendered as a claim about the run's
	// total. The per-refusal lines above still carry their own code, and this
	// line says how many there were, which is the part a reader with no grep at
	// hand cannot count.
	// A latch is not a total, and it is not rendered as one. g_registration_failed
	// is set when the *first* refusal is unexpected and never cleared, so by the
	// end of a run it means "some refusal, at some point, was not -10" at best -
	// and here it fires on a run whose every refusal was -10, which means it does
	// not mean that either. It is printed as the latch it is, pointing at the
	// lines that carry the codes, and the count below is what says how much of
	// the registration was refused.
	if (g_registration_failed) {
		fprintf(stderr, "nvmidi: an unexpected refusal was latched during registration; the per-call lines above carry the code for each one\n");
	}
	if (reg.refused > 0) {
		fprintf(stderr, "nvmidi: %d registration call(s) were refused of %d made; each refusal above names its own code\n",
			reg.refused, reg.calls);
	}
	// The probe's count, reported where a reader meets it next to the refusal
	// count. Both are answers a script's compile depends on and neither is
	// legible from the other.
	fprintf(stderr, "nvmidi: %d of the plugin's own types answered to their name after registration\n",
		reg.registered);
	fflush(stderr);
	// Kept so the playing code can call back into the script, see wait_until().
	g_engine = shared->script_engine;
	return true;
}
