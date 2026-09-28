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
#include <cstring>
#include <sstream>

namespace {

// RtMidi throws on every failure; NVGT scripts should see a return value
// instead, so every entry point wraps its body in this.
std::string g_last_error;

void set_error(const std::string& message) { g_last_error = message; }
void clear_error() { g_last_error.clear(); }

// Returns the low nibble of a status byte, i.e. the MIDI channel 1..16.
int channel_of(unsigned char status) { return (status & 0x0f) + 1; }

// Timing clock, active sensing and the other single-byte realtime messages
// carry no channel and only ever spam a game's message queue.
bool is_realtime(unsigned char status) { return status >= 0xf8; }

} // namespace

std::string midi_message::to_string() const {
	std::ostringstream out;
	out << midi_message_name(*this);
	return out.str();
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
	static_cast<midi_input*>(user_data)->push_message(message->data(), message->size(), delta);
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
}

void register_midi_output(asIScriptEngine* engine) {
	engine->RegisterObjectType("midi_output", 0, asOBJ_REF | asOBJ_NOCOUNT);
	engine->RegisterObjectMethod("midi_output", "bool open(uint port, const string&in name = \"nvmidi\")", asMETHOD(midi_output, open), asCALL_THISCALL);
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
	return true;
}
