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

class asIScriptEngine;
class CScriptArray;

// A single MIDI message as received from or sent to a device.
// status is the command byte (note on/off, control change, ...), data1 and
// data2 are the two parameter bytes, and channel is the low nibble of status.
struct midi_message {
	unsigned char status;
	unsigned char data1;
	unsigned char data2;
	int channel; // 1..16
	double timestamp; // seconds since the input port was opened
	std::string to_string() const;
};

// One MIDI message queued for the script to read.
// Messages arrive on RtMidi's own thread, so they are buffered here and
// handed to the script only when it asks (next_message / poll).
class midi_input {
public:
	midi_input();
	~midi_input();

	// Opens the input port at the given index of the enumerated device list.
	bool open(unsigned int port, const std::string& name = "nvmidi");
	void close();
	bool is_open() const;
	int get_port() const; // index of the open port, -1 when closed
	std::string get_port_name() const;

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
};

// An open MIDI output port.
class midi_output {
public:
	midi_output();
	~midi_output();

	bool open(unsigned int port, const std::string& name = "nvmidi");
	void close();
	bool is_open() const;
	int get_port() const;
	std::string get_port_name() const;

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
	// Stops every sounding note on all 16 channels.
	void all_notes_off();
	void reset();

	bool is_virtual_port() const { return virtual_port; }
	void set_virtual_port(bool value) { virtual_port = value; }

private:
	void* midi_out; // RtMidiOut*
	int port_index;
	bool virtual_port;
};

// Free functions registered with Angelscript.
unsigned int midi_input_port_count();
unsigned int midi_output_port_count();
std::string midi_input_port_name(unsigned int port);
std::string midi_output_port_name(unsigned int port);
std::string midi_api_name();
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

// Trampoline handed to RtMidi; defined in nvmidi.cpp.
void midi_input_callback(double delta, std::vector<unsigned char>* message, void* user_data);

void register_nvmidi(asIScriptEngine* engine);
