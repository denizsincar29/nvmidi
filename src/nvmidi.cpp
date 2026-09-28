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
#include <sstream>

namespace {

// The unit constants a script sees, so MIDI_BEATS and friends are readable
// names rather than bare numbers. They live here and are exposed as global
// properties; nothing ever writes to them.
const int g_unit_ms = MIDI_UNIT_MS;
const int g_unit_ticks = MIDI_UNIT_TICKS;
const int g_unit_beats = MIDI_UNIT_BEATS;
const int g_unit_bars = MIDI_UNIT_BARS;

// The musical spelling of the same units: MUSIC_BEATS is what a script using
// a class tempo writes, and the MUSIC_BEATS_90 style constants carry the tempo
// in the word itself, for a length that has to stay at one tempo no matter
// what the surrounding class is set to.
const int g_music_ms = MUSIC_MS;
const int g_music_ticks = MUSIC_TICKS;
const int g_music_beats = MUSIC_BEATS;
const int g_music_bars = MUSIC_BARS;
const int g_music_beats_90 = MUSIC_BEATS_90;
const int g_music_beats_100 = MUSIC_BEATS_100;
const int g_music_beats_120 = MUSIC_BEATS_120;
const int g_music_beats_140 = MUSIC_BEATS_140;

// RtMidi throws on every failure; NVGT scripts should see a return value
// instead, so every entry point wraps its body in this.
std::string g_last_error;

void set_error(const std::string& message) { g_last_error = message; }
void clear_error() { g_last_error.clear(); }

// The engine register_nvmidi() was handed. wait_until() needs it to call back
// into the script, and it is the only global the plugin keeps.
asIScriptEngine* g_engine = nullptr;

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


// The whole time schedule of a chord, worked out before a single byte is
// sent: every gap ends up as an absolute moment, so playback does not drift
// when a step takes a little longer than planned.
struct note_step {
	size_t note;
	double at;      // milliseconds from the start of the chord
};

// How long the whole group lasts. The length of the first note rules, which
// is the one rule that makes a chord of mixed note lengths predictable.
double group_length(const std::vector<midi_note>& notes) {
	if (notes.empty()) return 0.0;
	return notes.front().duration_ms();
}

// Lays the notes out on the time line pointed at by mode: 0 sounds them all
// at once, 1..3 spread them, 4 hands each one over to the next after its own
// release, 5 repeats the whole group.
void build_steps(const std::vector<midi_note>& notes, int mode, std::vector<note_step>& steps, double& total) {
	steps.clear();
	total = 0.0;
	if (notes.empty()) return;

	if (mode >= 1 && mode <= 4) {
		// Spread the entries; the chord still ends when the last note does,
		// which is the first note's length from its own start.
		double spread = 0.0;
		switch (mode) {
			case 1: spread = 0.25; break; // quarter beat
			case 2: spread = 0.5; break;  // eighth
			case 3: spread = 0.125; break; // sixteenth
			case 4: spread = 0.0625; break; // thirty-second
		}
		const double step = notes.front().duration_ms() * spread;
		for (size_t i = 0; i < notes.size(); ++i) {
			note_step s;
			s.note = i;
			s.at = step * static_cast<double>(i);
			steps.push_back(s);
		}
		total = steps.back().at + notes.front().duration_ms();
		return;
	}

	if (mode == 5) {
		// One after another: each note starts when the previous one ends.
		double at = 0.0;
		for (size_t i = 0; i < notes.size(); ++i) {
			note_step s;
			s.note = i;
			s.at = at;
			steps.push_back(s);
			at += notes[i].duration_ms();
		}
		total = at;
		return;
	}

	// Everything at once.
	for (size_t i = 0; i < notes.size(); ++i) {
		note_step s;
		s.note = i;
		s.at = 0.0;
		steps.push_back(s);
	}
	total = group_length(notes);
}

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

std::string midi_message::to_string() const {
	std::ostringstream out;
	out << midi_message_name(*this);
	return out.str();
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

midi_duration* midi_duration_create() { return new midi_duration(); }

midi_duration* midi_duration_create_full(double amount, int unit) {
	return new midi_duration(amount, unit);
}

midi_duration* midi_duration_create_tempo(double amount, int unit, double tempo) {
	return new midi_duration(amount, unit, tempo);
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
		RtMidiIn* in = new RtMidiIn(RtMidi::UNSPECIFIED, name);
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
		set_error("cannot open MIDI input port: " + error.getMessage());
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
bool midi_input::play_chord(CScriptArray* notes) {
	clear_error();
	if (!midi_in) {
		set_error("no MIDI input port is open");
		return false;
	}
	std::vector<midi_note> collected;
	if (!collect_notes(notes, tempo, collected)) {
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
bool midi_input::play_midi_chord(CScriptArray* notes, const std::string& pattern) {
	clear_error();
	if (!midi_in) {
		set_error("no MIDI input port is open");
		return false;
	}
	std::vector<midi_note> collected;
	if (!collect_notes(notes, tempo, collected)) {
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

bool midi_input::play_midi_chord_wait(CScriptArray* notes, const std::string& pattern) {
	return play_midi_chord(notes, pattern);
}

// One after another, each for its own length.
bool midi_input::play_sequence(CScriptArray* notes) {
	return play_midi_chord(notes, "sequence");
}

// The chord again, but the notes are released here before returning. Clearer
// to read in a script that plays a progression one chord at a time.
bool midi_input::play_chord_wait(CScriptArray* notes) {
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
	close();
	try {
		RtMidiOut* out = new RtMidiOut(RtMidi::UNSPECIFIED, name);
		if (!virtual_port) {
			const unsigned int count = out->getPortCount();
			if (port >= count) {
				delete out;
				set_error("MIDI output port " + std::to_string(port) + " does not exist (" + std::to_string(count) + " port(s) available)");
				return false;
			}
		}
		out->openPort(port, name);
		midi_out = out;
		port_index = static_cast<int>(port);
		return true;
	} catch (RtMidiError& error) {
		set_error("cannot open MIDI output port: " + error.getMessage());
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

bool midi_output::play_chord(CScriptArray* notes) {
	clear_error();
	if (!midi_out) {
		set_error("no MIDI output port is open");
		return false;
	}
	std::vector<midi_note> collected;
	if (!collect_notes(notes, tempo, collected)) {
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
bool midi_output::play_midi_chord(CScriptArray* notes, const std::string& pattern) {
	return play_midi_chord_wait(notes, pattern);
}

bool midi_output::play_midi_chord_wait(CScriptArray* notes, const std::string& pattern) {
	clear_error();
	if (!midi_out) {
		set_error("no MIDI output port is open");
		return false;
	}
	stop_all_notes();
	std::vector<midi_note> collected;
	if (!collect_notes(notes, tempo, collected)) {
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

bool midi_output::play_chord_wait(CScriptArray* notes) {
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
		RtMidiIn in;
		return in.getPortCount();
	} catch (RtMidiError& error) {
		set_error(error.getMessage());
		return 0;
	}
}

unsigned int midi_output_port_count() {
	try {
		RtMidiOut out;
		return out.getPortCount();
	} catch (RtMidiError& error) {
		set_error(error.getMessage());
		return 0;
	}
}

std::string midi_input_port_name(unsigned int port) {
	try {
		RtMidiIn in;
		if (port >= in.getPortCount()) return "";
		return in.getPortName(port);
	} catch (RtMidiError&) {
		return "";
	}
}

std::string midi_output_port_name(unsigned int port) {
	try {
		RtMidiOut out;
		if (port >= out.getPortCount()) return "";
		return out.getPortName(port);
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

// Names the backend RtMidi actually compiled against, so a user can tell
// "MMAPI" on Windows from "ALSA" on Linux in a bug report.
std::string midi_api_name() {
	try {
		// RtMidi can be compiled with several backends; the first one is the
		// one a default-constructed port will pick.
		std::vector<RtMidi::Api> apis;
		RtMidiIn::getCompiledApi(apis);
		if (apis.empty()) return "";
		return RtMidiIn::getApiDisplayName(apis[0]);
	} catch (RtMidiError&) {
		return "";
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

void register_midi_message(asIScriptEngine* engine) {
	engine->RegisterObjectType("midi_message", sizeof(midi_message), asOBJ_VALUE | asOBJ_POD | asGetTypeTraits<midi_message>());
	engine->RegisterObjectProperty("midi_message", "uint8 status", asOFFSET(midi_message, status));
	engine->RegisterObjectProperty("midi_message", "uint8 data1", asOFFSET(midi_message, data1));
	engine->RegisterObjectProperty("midi_message", "uint8 data2", asOFFSET(midi_message, data2));
	engine->RegisterObjectProperty("midi_message", "int channel", asOFFSET(midi_message, channel));
	engine->RegisterObjectProperty("midi_message", "double timestamp", asOFFSET(midi_message, timestamp));
	engine->RegisterObjectMethod("midi_message", "string opImplConv() const", asMETHOD(midi_message, to_string), asCALL_THISCALL);
	engine->RegisterObjectMethod("midi_message", "string to_string() const", asMETHOD(midi_message, to_string), asCALL_THISCALL);
}

void register_midi_input(asIScriptEngine* engine) {
	engine->RegisterObjectType("midi_input", 0, asOBJ_REF | asOBJ_NOCOUNT);
	engine->RegisterObjectMethod("midi_input", "bool open(uint port, const string&in name = \"nvmidi\")", asMETHOD(midi_input, open), asCALL_THISCALL);
	engine->RegisterObjectMethod("midi_input", "bool open_by_name(const string&in substring, const string&in name = \"nvmidi\")", asMETHOD(midi_input, open_by_name), asCALL_THISCALL);
	engine->RegisterObjectMethod("midi_input", "bool open_config(midi_config@ config, const string&in name = \"nvmidi\")", asMETHOD(midi_input, open_config), asCALL_THISCALL);
	engine->RegisterObjectMethod("midi_input", "bool play_chord(midi_note@[]@ notes)", asMETHOD(midi_input, play_chord), asCALL_THISCALL);
	engine->RegisterObjectMethod("midi_input", "bool play_chord_wait(midi_note@[]@ notes)", asMETHOD(midi_input, play_chord_wait), asCALL_THISCALL);
	engine->RegisterObjectMethod("midi_input", "bool play_note(const midi_note&in note)", asMETHOD(midi_input, play_note), asCALL_THISCALL);
	engine->RegisterObjectMethod("midi_input", "bool play_note_wait(const midi_note&in note)", asMETHOD(midi_input, play_note_wait), asCALL_THISCALL);
	engine->RegisterObjectMethod("midi_input", "uint stop_all_notes()", asMETHOD(midi_input, stop_all_notes), asCALL_THISCALL);
	engine->RegisterObjectMethod("midi_input", "uint get_active_notes() const", asMETHOD(midi_input, get_active_notes), asCALL_THISCALL);
	engine->RegisterObjectMethod("midi_input", "void close()", asMETHOD(midi_input, close), asCALL_THISCALL);
	engine->RegisterObjectMethod("midi_input", "bool is_open() const", asMETHOD(midi_input, is_open), asCALL_THISCALL);
	engine->RegisterObjectMethod("midi_input", "int get_port() const", asMETHOD(midi_input, get_port), asCALL_THISCALL);
	engine->RegisterObjectMethod("midi_input", "string get_port_name() const", asMETHOD(midi_input, get_port_name), asCALL_THISCALL);
	engine->RegisterObjectMethod("midi_input", "bool has_message() const", asMETHOD(midi_input, has_message), asCALL_THISCALL);
	engine->RegisterObjectMethod("midi_input", "uint get_pending() const", asMETHOD(midi_input, get_pending), asCALL_THISCALL);
	engine->RegisterObjectMethod("midi_input", "bool next_message(midi_message&out) const", asMETHOD(midi_input, next_message), asCALL_THISCALL);
	engine->RegisterObjectMethod("midi_input", "void clear()", asMETHOD(midi_input, clear), asCALL_THISCALL);
	engine->RegisterObjectMethod("midi_input", "void set_ignore_sysex(bool)", asMETHOD(midi_input, set_ignore_sysex), asCALL_THISCALL);
	engine->RegisterObjectMethod("midi_input", "bool get_ignore_sysex() const", asMETHOD(midi_input, get_ignore_sysex), asCALL_THISCALL);
	engine->RegisterObjectMethod("midi_input", "void set_ignore_timing(bool)", asMETHOD(midi_input, set_ignore_timing), asCALL_THISCALL);
	engine->RegisterObjectMethod("midi_input", "bool get_ignore_timing() const", asMETHOD(midi_input, get_ignore_timing), asCALL_THISCALL);
	engine->RegisterObjectMethod("midi_input", "uint get_message_queue_size() const", asMETHOD(midi_input, get_pending), asCALL_THISCALL);
	engine->RegisterObjectMethod("midi_input", "bool play_midi_chord(midi_note@[]@ notes, const string&in pattern)", asMETHOD(midi_input, play_midi_chord), asCALL_THISCALL);
	engine->RegisterObjectMethod("midi_input", "bool play_midi_chord_wait(midi_note@[]@ notes, const string&in pattern)", asMETHOD(midi_input, play_midi_chord_wait), asCALL_THISCALL);
	engine->RegisterObjectMethod("midi_input", "bool play_sequence(midi_note@[]@ notes)", asMETHOD(midi_input, play_sequence), asCALL_THISCALL);
	engine->RegisterObjectMethod("midi_input", "midi_duration duration(double amount, int unit) const", asMETHOD(midi_input, duration), asCALL_THISCALL);
	engine->RegisterObjectProperty("midi_input", "double tempo", asOFFSET(midi_input, tempo));
}

void register_midi_output(asIScriptEngine* engine) {
	engine->RegisterObjectType("midi_output", 0, asOBJ_REF | asOBJ_NOCOUNT);
	engine->RegisterObjectMethod("midi_output", "bool open(uint port, const string&in name = \"nvmidi\")", asMETHOD(midi_output, open), asCALL_THISCALL);
	engine->RegisterObjectMethod("midi_output", "bool open_by_name(const string&in substring, const string&in name = \"nvmidi\")", asMETHOD(midi_output, open_by_name), asCALL_THISCALL);
	engine->RegisterObjectMethod("midi_output", "bool open_config(midi_config@ config, const string&in name = \"nvmidi\")", asMETHOD(midi_output, open_config), asCALL_THISCALL);
	engine->RegisterObjectMethod("midi_output", "bool play_chord(midi_note@[]@ notes)", asMETHOD(midi_output, play_chord), asCALL_THISCALL);
	engine->RegisterObjectMethod("midi_output", "bool play_chord_wait(midi_note@[]@ notes)", asMETHOD(midi_output, play_chord_wait), asCALL_THISCALL);
	engine->RegisterObjectMethod("midi_output", "bool play_midi_chord(midi_note@[]@ notes, const string&in pattern)", asMETHOD(midi_output, play_midi_chord), asCALL_THISCALL);
	engine->RegisterObjectMethod("midi_output", "bool play_midi_chord_wait(midi_note@[]@ notes, const string&in pattern)", asMETHOD(midi_output, play_midi_chord_wait), asCALL_THISCALL);
	engine->RegisterObjectMethod("midi_output", "bool play_note(const midi_note&in note)", asMETHOD(midi_output, play_note), asCALL_THISCALL);
	engine->RegisterObjectMethod("midi_output", "bool play_note_wait(const midi_note&in note)", asMETHOD(midi_output, play_note_wait), asCALL_THISCALL);
	engine->RegisterObjectMethod("midi_output", "uint stop_all_notes()", asMETHOD(midi_output, stop_all_notes), asCALL_THISCALL);
	engine->RegisterObjectMethod("midi_output", "uint get_active_notes() const", asMETHOD(midi_output, get_active_notes), asCALL_THISCALL);
	engine->RegisterObjectMethod("midi_output", "void close()", asMETHOD(midi_output, close), asCALL_THISCALL);
	engine->RegisterObjectMethod("midi_output", "bool is_open() const", asMETHOD(midi_output, is_open), asCALL_THISCALL);
	engine->RegisterObjectMethod("midi_output", "int get_port() const", asMETHOD(midi_output, get_port), asCALL_THISCALL);
	engine->RegisterObjectMethod("midi_output", "string get_port_name() const", asMETHOD(midi_output, get_port_name), asCALL_THISCALL);
	engine->RegisterObjectMethod("midi_output", "bool send(uint status, uint data1, uint data2)", asMETHOD(midi_output, send), asCALL_THISCALL);
	engine->RegisterObjectMethod("midi_output", "bool send_packed(uint packed)", asMETHOD(midi_output, send_packed), asCALL_THISCALL);
	engine->RegisterObjectMethod("midi_output", "void send_note_on(uint channel, uint note, uint velocity)", asMETHOD(midi_output, send_note_on), asCALL_THISCALL);
	engine->RegisterObjectMethod("midi_output", "void send_note_off(uint channel, uint note, uint velocity = 0)", asMETHOD(midi_output, send_note_off), asCALL_THISCALL);
	engine->RegisterObjectMethod("midi_output", "void send_control_change(uint channel, uint controller, uint value)", asMETHOD(midi_output, send_control_change), asCALL_THISCALL);
	engine->RegisterObjectMethod("midi_output", "void send_program_change(uint channel, uint program)", asMETHOD(midi_output, send_program_change), asCALL_THISCALL);
	engine->RegisterObjectMethod("midi_output", "void send_pitch_bend(uint channel, uint value)", asMETHOD(midi_output, send_pitch_bend), asCALL_THISCALL);
	engine->RegisterObjectMethod("midi_output", "void send_aftertouch(uint channel, uint note, uint pressure)", asMETHOD(midi_output, send_aftertouch), asCALL_THISCALL);
	engine->RegisterObjectMethod("midi_output", "void send_channel_pressure(uint channel, uint pressure)", asMETHOD(midi_output, send_channel_pressure), asCALL_THISCALL);
	engine->RegisterObjectMethod("midi_output", "void send_sysex(const string&in data)", asMETHOD(midi_output, send_sysex), asCALL_THISCALL);
	engine->RegisterObjectMethod("midi_output", "void all_notes_off()", asMETHOD(midi_output, all_notes_off), asCALL_THISCALL);
	engine->RegisterObjectMethod("midi_output", "void reset()", asMETHOD(midi_output, reset), asCALL_THISCALL);
	engine->RegisterObjectMethod("midi_output", "midi_duration duration(double amount, int unit) const", asMETHOD(midi_output, duration), asCALL_THISCALL);
	engine->RegisterObjectProperty("midi_output", "double tempo", asOFFSET(midi_output, tempo));
}

void register_midi_note(asIScriptEngine* engine) {
	engine->RegisterObjectType("midi_duration", sizeof(midi_duration), asOBJ_VALUE | asOBJ_POD | asGetTypeTraits<midi_duration>());
	engine->RegisterObjectProperty("midi_duration", "double amount", asOFFSET(midi_duration, amount));
	engine->RegisterObjectProperty("midi_duration", "int unit", asOFFSET(midi_duration, unit));
	engine->RegisterObjectProperty("midi_duration", "double tempo", asOFFSET(midi_duration, tempo));
	engine->RegisterObjectProperty("midi_duration", "double ppq", asOFFSET(midi_duration, ppq));
	engine->RegisterObjectMethod("midi_duration", "double to_ms() const", asMETHOD(midi_duration, to_ms), asCALL_THISCALL);
	engine->RegisterObjectMethod("midi_duration", "string to_string() const", asMETHOD(midi_duration, to_string), asCALL_THISCALL);
	engine->RegisterObjectMethod("midi_duration", "string opImplConv() const", asMETHOD(midi_duration, to_string), asCALL_THISCALL);
	engine->RegisterGlobalFunction("midi_duration@ midi_duration_create()", asFUNCTION(midi_duration_create), asCALL_CDECL);
	engine->RegisterGlobalFunction("midi_duration@ midi_duration_create(double amount, int unit)", asFUNCTION(midi_duration_create_full), asCALL_CDECL);
	engine->RegisterGlobalFunction("midi_duration@ midi_duration_create(double amount, int unit, double tempo)", asFUNCTION(midi_duration_create_tempo), asCALL_CDECL);

	// A handle type: a script writes note@ n = midi_note(60, 100); and the
	// handle points at the object rather than copying it, which is what makes
	// changing n.velocity later actually change the note that is played.
	engine->RegisterObjectType("midi_note", 0, asOBJ_REF | asOBJ_NOCOUNT);
	engine->RegisterObjectProperty("midi_note", "int pitch", asOFFSET(midi_note, pitch));
	engine->RegisterObjectProperty("midi_note", "int velocity", asOFFSET(midi_note, velocity));
	engine->RegisterObjectProperty("midi_note", "int channel", asOFFSET(midi_note, channel));
	engine->RegisterObjectProperty("midi_note", "midi_duration length", asOFFSET(midi_note, length));
	engine->RegisterObjectProperty("midi_note", "double duration_ms", asOFFSET(midi_note, length) + offsetof(midi_duration, amount));
	engine->RegisterObjectMethod("midi_note", "string to_string() const", asMETHOD(midi_note, to_string), asCALL_THISCALL);
	engine->RegisterObjectMethod("midi_note", "string opImplConv() const", asMETHOD(midi_note, to_string), asCALL_THISCALL);
	engine->RegisterGlobalFunction("midi_note@ midi_note_create()", asFUNCTION(midi_note_create), asCALL_CDECL);
	engine->RegisterGlobalFunction("midi_note@ midi_note_create(int pitch, int velocity, int channel)", asFUNCTION(midi_note_create_full), asCALL_CDECL);
	engine->RegisterGlobalFunction("midi_note@ midi_note_create_ms(int pitch, int velocity, double duration_ms)", asFUNCTION(midi_note_create_ms), asCALL_CDECL);
	engine->RegisterGlobalFunction("int midi_note_number(const string&in name)", asFUNCTION(midi_note_number), asCALL_CDECL);
	engine->RegisterGlobalFunction("string midi_note_name(int pitch)", asFUNCTION(midi_note_pitch_name), asCALL_CDECL);

	// Unit constants, so a script never has to remember 0..3.
	engine->RegisterGlobalProperty("const int MIDI_MS", (void*)&g_unit_ms);
	engine->RegisterGlobalProperty("const int MIDI_TICKS", (void*)&g_unit_ticks);
	engine->RegisterGlobalProperty("const int MIDI_BEATS", (void*)&g_unit_beats);
	engine->RegisterGlobalProperty("const int MIDI_BARS", (void*)&g_unit_bars);
	// The musical units. The plain four mean the same numbers as MIDI_*, so a
	// script written for one spelling keeps working with the other; the
	// MUSIC_BEATS_NN constants are extra values that carry their own tempo.
	engine->RegisterGlobalProperty("const int MUSIC_MS", (void*)&g_music_ms);
	engine->RegisterGlobalProperty("const int MUSIC_TICKS", (void*)&g_music_ticks);
	engine->RegisterGlobalProperty("const int MUSIC_BEATS", (void*)&g_music_beats);
	engine->RegisterGlobalProperty("const int MUSIC_BARS", (void*)&g_music_bars);
	engine->RegisterGlobalProperty("const int MUSIC_BEATS_90", (void*)&g_music_beats_90);
	engine->RegisterGlobalProperty("const int MUSIC_BEATS_100", (void*)&g_music_beats_100);
	engine->RegisterGlobalProperty("const int MUSIC_BEATS_120", (void*)&g_music_beats_120);
	engine->RegisterGlobalProperty("const int MUSIC_BEATS_140", (void*)&g_music_beats_140);
}

void register_midi_config(asIScriptEngine* engine) {
	engine->RegisterObjectType("midi_config", 0, asOBJ_REF | asOBJ_NOCOUNT);
	engine->RegisterObjectProperty("midi_config", "string match", asOFFSET(midi_config, match));
	engine->RegisterObjectProperty("midi_config", "int port", asOFFSET(midi_config, port));
	engine->RegisterObjectMethod("midi_config", "bool load(const string&in path)", asMETHOD(midi_config, load), asCALL_THISCALL);
	engine->RegisterObjectMethod("midi_config", "bool load_if_present(const string&in path)", asMETHOD(midi_config, load_if_present), asCALL_THISCALL);
	engine->RegisterObjectMethod("midi_config", "int find_input_port() const", asMETHOD(midi_config, find_input_port), asCALL_THISCALL);
	engine->RegisterObjectMethod("midi_config", "int find_output_port() const", asMETHOD(midi_config, find_output_port), asCALL_THISCALL);
	engine->RegisterObjectMethod("midi_config", "int get_last_port() const", asMETHOD(midi_config, get_last_port), asCALL_THISCALL);
	engine->RegisterObjectMethod("midi_config", "string describe() const", asMETHOD(midi_config, describe), asCALL_THISCALL);
	engine->RegisterObjectMethod("midi_config", "string get_path() const", asMETHOD(midi_config, get_path), asCALL_THISCALL);
	engine->RegisterGlobalFunction("midi_config@ midi_config_create()", asFUNCTION(midi_config_create), asCALL_CDECL);
	engine->RegisterGlobalFunction("int midi_find_input_port(const string&in substring)", asFUNCTION(midi_find_input_port), asCALL_CDECL);
	engine->RegisterGlobalFunction("int midi_find_output_port(const string&in substring)", asFUNCTION(midi_find_output_port), asCALL_CDECL);
}

void register_midi_globals(asIScriptEngine* engine) {
	engine->RegisterGlobalFunction("uint midi_input_port_count()", asFUNCTION(midi_input_port_count), asCALL_CDECL);
	engine->RegisterGlobalFunction("uint midi_output_port_count()", asFUNCTION(midi_output_port_count), asCALL_CDECL);
	engine->RegisterGlobalFunction("string midi_input_port_name(uint port)", asFUNCTION(midi_input_port_name), asCALL_CDECL);
	engine->RegisterGlobalFunction("string midi_output_port_name(uint port)", asFUNCTION(midi_output_port_name), asCALL_CDECL);
	engine->RegisterGlobalFunction("string[]@ midi_input_port_names()", asFUNCTION(midi_input_port_names), asCALL_CDECL);
	engine->RegisterGlobalFunction("string[]@ midi_output_port_names()", asFUNCTION(midi_output_port_names), asCALL_CDECL);
	engine->RegisterGlobalFunction("string midi_api_name()", asFUNCTION(midi_api_name), asCALL_CDECL);
	engine->RegisterGlobalFunction("string midi_message_name(const midi_message&in m)", asFUNCTION(midi_message_name), asCALL_CDECL);
	engine->RegisterGlobalFunction("string midi_last_error()", asFUNCTION(midi_last_error), asCALL_CDECL);
	engine->RegisterGlobalFunction("midi_input@ midi_input_create()", asFUNCTION(midi_input_create), asCALL_CDECL);
	engine->RegisterGlobalFunction("midi_output@ midi_output_create()", asFUNCTION(midi_output_create), asCALL_CDECL);
}

void register_nvmidi(asIScriptEngine* engine) {
	register_midi_message(engine);
	// The note, duration and config types first: the port classes name them in
	// their own method declarations, so the engine has to know them by then.
	register_midi_note(engine);
	register_midi_config(engine);
	register_midi_input(engine);
	register_midi_output(engine);
	register_midi_globals(engine);
}

midi_input* midi_input_create() { return new midi_input(); }
midi_output* midi_output_create() { return new midi_output(); }

std::string midi_last_error() { return g_last_error; }

// ---------------------------------------------------------------------------
// Plugin entry point
// ---------------------------------------------------------------------------

plugin_main(nvgt_plugin_shared* shared) {
	if (!prepare_plugin(shared)) return false;
	if (!shared->script_engine) return false;
	register_nvmidi(shared->script_engine);
	// Kept so the playing code can call back into the script, see wait_until().
	g_engine = shared->script_engine;
	return true;
}
