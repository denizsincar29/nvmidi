// nvmidi-tester: a fake MIDI keyboard for people who have no MIDI keyboard.
//
// It opens a virtual MIDI output port called "nvmidi virtual" and sends what
// you type on stdin as if a keyboard had produced it.
//
//   nvmidi-tester        every Enter (empty line) plays a C-E-G chord, half a
//                        second long. Text commands send controllers.
//   nvmidi-tester -a     stdin is one JSON object per line, each one a MIDI
//                        message. Meant for scripts and other programs.
//
// Windows 11 25H2 or newer: the tool creates the virtual device itself through
// Windows MIDI Services (MSVC build, wms_backend.cpp). Anything older: winmm has
// no virtual ports, so the tool opens an existing output port whose name
// contains "nvmidi virtual" - create one with loopMIDI and point the game at
// the same port. Linux (ALSA) and macOS (CoreMIDI) create a real virtual port.
//
// Output is plain lines of text, nothing else, so a screen reader reads it.

#include "RtMidi.h"
#ifdef NVMIDI_WMS
#include "wms_backend.h"
#endif

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <queue>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

using Bytes = std::vector<unsigned char>;
using Clock = std::chrono::steady_clock;

// ---------------------------------------------------------------- helpers

static std::string lower(std::string s) {
	for (auto &c : s) c = (char)std::tolower((unsigned char)c);
	return s;
}

static std::string trim(const std::string &s) {
	size_t a = 0, b = s.size();
	while (a < b && std::isspace((unsigned char)s[a])) a++;
	while (b > a && std::isspace((unsigned char)s[b - 1])) b--;
	return s.substr(a, b - a);
}

// "C4" -> 60, "F#3", "Bb5", "c-1". Also a plain number 0..127. -1 on failure.
static int parse_note(const std::string &in) {
	std::string s = trim(in);
	if (s.empty()) return -1;
	if (std::isdigit((unsigned char)s[0])) {
		char *end = nullptr;
		long v = std::strtol(s.c_str(), &end, 10);
		if (*end || v < 0 || v > 127) return -1;
		return (int)v;
	}
	static const int base[7] = {9, 11, 0, 2, 4, 5, 7}; // A B C D E F G
	char l = (char)std::toupper((unsigned char)s[0]);
	if (l < 'A' || l > 'G') return -1;
	int n = base[l - 'A'];
	size_t i = 1;
	while (i < s.size() && (s[i] == '#' || s[i] == 'b' || s[i] == 'B')) {
		// 'b' right after the letter is a flat; a trailing digit is the octave.
		n += (s[i] == '#') ? 1 : -1;
		i++;
	}
	if (i >= s.size()) return -1;
	char *end = nullptr;
	long oct = std::strtol(s.c_str() + i, &end, 10);
	if (*end) return -1;
	int v = (int)((oct + 1) * 12 + n);
	return (v < 0 || v > 127) ? -1 : v;
}

static int clamp(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

// ---------------------------------------------------------------- the port

class Keyboard {
public:
	std::unique_ptr<RtMidiOut> out;
	std::mutex send_mx;
	int channel = 1;   // 1..16, the same convention as the nvmidi plugin
	int velocity = 100;

	bool use_wms = false; // Windows MIDI Services backend instead of RtMidi

	void send(const Bytes &m) {
		std::lock_guard<std::mutex> g(send_mx);
#ifdef NVMIDI_WMS
		if (use_wms) {
			std::string e;
			if (!wms::send(m, e)) std::cerr << "error: " << e << "\n";
			return;
		}
#endif
		try { out->sendMessage(&m); } catch (RtMidiError &e) { std::cerr << "error: " << e.getMessage() << "\n"; }
	}

	unsigned char st(unsigned char kind, int ch) { return (unsigned char)(kind | ((clamp(ch, 1, 16) - 1) & 0x0f)); }

	void note_on(int ch, int n, int v) { send({st(0x90, ch), (unsigned char)n, (unsigned char)clamp(v, 1, 127)}); }
	void note_off(int ch, int n) { send({st(0x80, ch), (unsigned char)n, 0}); }
	void cc(int ch, int c, int v) { send({st(0xb0, ch), (unsigned char)clamp(c, 0, 127), (unsigned char)clamp(v, 0, 127)}); }
	void program(int ch, int p) { send({st(0xc0, ch), (unsigned char)clamp(p, 0, 127)}); }
	void bend(int ch, int v) { // -8192..8191
		int u = clamp(v, -8192, 8191) + 8192;
		send({st(0xe0, ch), (unsigned char)(u & 0x7f), (unsigned char)((u >> 7) & 0x7f)});
	}
	void poly_pressure(int ch, int n, int p) { send({st(0xa0, ch), (unsigned char)n, (unsigned char)clamp(p, 0, 127)}); }
	void chan_pressure(int ch, int p) { send({st(0xd0, ch), (unsigned char)clamp(p, 0, 127)}); }
	void panic() {
		for (int ch = 1; ch <= 16; ch++) {
			cc(ch, 64, 0);  // sustain off
			cc(ch, 123, 0); // all notes off
			cc(ch, 120, 0); // all sound off
		}
	}
};

// Timed note-offs, so a long note does not block stdin.
class Scheduler {
public:
	explicit Scheduler(Keyboard &k) : kb(k), th([this] { run(); }) {}
	~Scheduler() { stop(); }

	void note_off_in(int ms, int ch, int n) {
		{
			std::lock_guard<std::mutex> g(mx);
			q.push({Clock::now() + std::chrono::milliseconds(ms), ch, n});
		}
		cv.notify_one();
	}
	// Release everything that is still pending, now.
	void flush() {
		std::vector<Item> v;
		{
			std::lock_guard<std::mutex> g(mx);
			while (!q.empty()) { v.push_back(q.top()); q.pop(); }
		}
		for (auto &i : v) kb.note_off(i.ch, i.n);
	}
	void stop() {
		if (done.exchange(true)) return;
		cv.notify_all();
		if (th.joinable()) th.join();
	}

private:
	struct Item {
		Clock::time_point at; int ch; int n;
		bool operator<(const Item &o) const { return at > o.at; }
	};
	void run() {
		std::unique_lock<std::mutex> lk(mx);
		while (!done) {
			if (q.empty()) { cv.wait(lk); continue; }
			auto top = q.top();
			if (Clock::now() < top.at) { cv.wait_until(lk, top.at); continue; }
			q.pop();
			lk.unlock();
			kb.note_off(top.ch, top.n);
			lk.lock();
		}
	}
	Keyboard &kb;
	std::mutex mx;
	std::condition_variable cv;
	std::priority_queue<Item> q;
	std::atomic<bool> done{false};
	std::thread th;
};

// ---------------------------------------------------------------- tiny JSON

struct JVal {
	enum T { Null, Bool, Num, Str, Arr, Obj } t = Null;
	bool b = false;
	double n = 0;
	std::string s;
	std::vector<JVal> a;
	std::map<std::string, JVal> o;
};

struct JParser {
	const std::string &s;
	size_t i = 0;
	std::string err;
	explicit JParser(const std::string &str) : s(str) {}

	void ws() { while (i < s.size() && std::isspace((unsigned char)s[i])) i++; }
	bool fail(const std::string &m) { if (err.empty()) err = m + " at column " + std::to_string(i + 1); return false; }

	bool parse(JVal &v) {
		ws();
		if (i >= s.size()) return fail("unexpected end");
		char c = s[i];
		if (c == '{') return object(v);
		if (c == '[') return array(v);
		if (c == '"') { v.t = JVal::Str; return str(v.s); }
		if (!s.compare(i, 4, "true")) { v.t = JVal::Bool; v.b = true; i += 4; return true; }
		if (!s.compare(i, 5, "false")) { v.t = JVal::Bool; v.b = false; i += 5; return true; }
		if (!s.compare(i, 4, "null")) { v.t = JVal::Null; i += 4; return true; }
		if (c == '-' || std::isdigit((unsigned char)c)) {
			char *end = nullptr;
			v.n = std::strtod(s.c_str() + i, &end);
			if (end == s.c_str() + i) return fail("bad number");
			i = (size_t)(end - s.c_str());
			v.t = JVal::Num;
			return true;
		}
		return fail(std::string("unexpected '") + c + "'");
	}
	bool str(std::string &out) {
		i++; // opening quote
		while (i < s.size() && s[i] != '"') {
			if (s[i] == '\\' && i + 1 < s.size()) {
				char e = s[++i];
				switch (e) {
					case 'n': out += '\n'; break;
					case 't': out += '\t'; break;
					case 'r': out += '\r'; break;
					case 'u': // keep it simple: only the ASCII range matters here
						if (i + 4 < s.size()) { out += (char)std::strtol(s.substr(i + 1, 4).c_str(), nullptr, 16); i += 4; }
						break;
					default: out += e;
				}
			} else out += s[i];
			i++;
		}
		if (i >= s.size()) return fail("unterminated string");
		i++;
		return true;
	}
	bool array(JVal &v) {
		v.t = JVal::Arr; i++; ws();
		if (i < s.size() && s[i] == ']') { i++; return true; }
		for (;;) {
			JVal e; if (!parse(e)) return false;
			v.a.push_back(std::move(e)); ws();
			if (i < s.size() && s[i] == ',') { i++; continue; }
			if (i < s.size() && s[i] == ']') { i++; return true; }
			return fail("expected , or ]");
		}
	}
	bool object(JVal &v) {
		v.t = JVal::Obj; i++; ws();
		if (i < s.size() && s[i] == '}') { i++; return true; }
		for (;;) {
			ws();
			if (i >= s.size() || s[i] != '"') return fail("expected key");
			std::string k; if (!str(k)) return false;
			ws();
			if (i >= s.size() || s[i] != ':') return fail("expected :");
			i++;
			JVal e; if (!parse(e)) return false;
			v.o[lower(k)] = std::move(e); ws();
			if (i < s.size() && s[i] == ',') { i++; continue; }
			if (i < s.size() && s[i] == '}') { i++; return true; }
			return fail("expected , or }");
		}
	}
};

// ---------------------------------------------------------------- JSON mode

static bool j_int(const JVal &o, const char *k, int &out) {
	auto it = o.o.find(k);
	if (it == o.o.end()) return false;
	if (it->second.t == JVal::Num) { out = (int)it->second.n; return true; }
	if (it->second.t == JVal::Bool) { out = it->second.b ? 127 : 0; return true; }
	if (it->second.t == JVal::Str) {
		std::string v = lower(it->second.s);
		if (v == "on" || v == "true" || v == "down") { out = 127; return true; }
		if (v == "off" || v == "false" || v == "up") { out = 0; return true; }
		char *end = nullptr; long x = std::strtol(v.c_str(), &end, 10);
		if (*end == 0 && !v.empty()) { out = (int)x; return true; }
	}
	return false;
}

// "note" may be a number or a name.
static bool j_note(const JVal &o, const char *k, int &out) {
	auto it = o.o.find(k);
	if (it == o.o.end()) return false;
	if (it->second.t == JVal::Num) { out = (int)it->second.n; return out >= 0 && out <= 127; }
	if (it->second.t == JVal::Str) { out = parse_note(it->second.s); return out >= 0; }
	return false;
}

static bool handle_obj(Keyboard &kb, Scheduler &sch, const JVal &v, std::string &err);

// One line is one message, or an array of messages.
static bool handle_json(Keyboard &kb, Scheduler &sch, const std::string &line, std::string &err) {
	JParser p(line);
	JVal v;
	if (!p.parse(v)) { err = "bad json: " + p.err; return false; }
	if (v.t == JVal::Arr) {
		for (auto &e : v.a)
			if (!handle_obj(kb, sch, e, err)) return false;
		return true;
	}
	return handle_obj(kb, sch, v, err);
}

static bool handle_obj(Keyboard &kb, Scheduler &sch, const JVal &v, std::string &err) {
	if (v.t != JVal::Obj) { err = "expected a json object"; return false; }

	// raw bytes win over everything
	for (const char *key : {"bytes", "data", "raw"}) {
		auto it = v.o.find(key);
		if (it != v.o.end() && it->second.t == JVal::Arr) {
			Bytes m;
			for (auto &x : it->second.a) {
				if (x.t != JVal::Num || x.n < 0 || x.n > 255) { err = "bytes must be numbers 0..255"; return false; }
				m.push_back((unsigned char)x.n);
			}
			if (m.empty()) { err = "empty bytes"; return false; }
			kb.send(m);
			return true;
		}
	}

	std::string type;
	auto ti = v.o.find("type");
	if (ti == v.o.end()) ti = v.o.find("msg");
	if (ti == v.o.end()) ti = v.o.find("command");
	if (ti == v.o.end() || ti->second.t != JVal::Str) { err = "missing \"type\""; return false; }
	type = lower(ti->second.s);
	for (auto &c : type) if (c == '-' || c == ' ') c = '_';

	int ch = kb.channel;
	j_int(v, "channel", ch);
	if (ch < 1 || ch > 16) { err = "channel must be 1..16"; return false; }
	int vel = kb.velocity;
	if (!j_int(v, "velocity", vel)) j_int(v, "vel", vel);
	int dur = -1;
	if (!j_int(v, "duration", dur)) if (!j_int(v, "ms", dur)) j_int(v, "length", dur);

	if (type == "note_on" || type == "noteon" || type == "note" || type == "on") {
		int n;
		if (!j_note(v, "note", n)) { err = "note_on needs a valid \"note\" (0..127 or a name like C4)"; return false; }
		if (vel == 0) kb.note_off(ch, n); else kb.note_on(ch, n, vel);
		if (dur >= 0 && vel > 0) sch.note_off_in(dur, ch, n);
		return true;
	}
	if (type == "note_off" || type == "noteoff" || type == "off") {
		int n;
		if (!j_note(v, "note", n)) { err = "note_off needs a valid \"note\""; return false; }
		kb.note_off(ch, n);
		return true;
	}
	if (type == "chord") {
		auto it = v.o.find("notes");
		if (it == v.o.end() || it->second.t != JVal::Arr || it->second.a.empty()) { err = "chord needs a \"notes\" array"; return false; }
		if (dur < 0) dur = 500;
		for (auto &x : it->second.a) {
			int n = -1;
			if (x.t == JVal::Num) n = (int)x.n; else if (x.t == JVal::Str) n = parse_note(x.s);
			if (n < 0 || n > 127) { err = "bad note in chord"; return false; }
			kb.note_on(ch, n, vel);
			sch.note_off_in(dur, ch, n);
		}
		return true;
	}
	if (type == "cc" || type == "control_change" || type == "controller" || type == "controlchange") {
		int c, val;
		if (!j_int(v, "control", c) && !j_int(v, "controller", c) && !j_int(v, "cc", c) && !j_int(v, "number", c)) { err = "cc needs \"control\""; return false; }
		if (!j_int(v, "value", val)) { err = "cc needs \"value\""; return false; }
		if (c < 0 || c > 127) { err = "control must be 0..127"; return false; }
		kb.cc(ch, c, val);
		return true;
	}
	struct Pedal { const char *name; int cc; };
	static const Pedal pedals[] = {{"sustain", 64}, {"damper", 64}, {"sostenuto", 66}, {"soft", 67}, {"soft_pedal", 67},
	                               {"portamento", 65}, {"modulation", 1}, {"mod", 1}, {"volume", 7}, {"expression", 11}, {"pan", 10}};
	for (auto &pd : pedals) {
		if (type == pd.name) {
			int val = 127;
			if (!j_int(v, "value", val)) if (!j_int(v, "state", val)) j_int(v, "on", val);
			kb.cc(ch, pd.cc, val);
			return true;
		}
	}
	if (type == "program_change" || type == "program" || type == "pc" || type == "programchange") {
		int p;
		if (!j_int(v, "program", p) && !j_int(v, "value", p)) { err = "program_change needs \"program\""; return false; }
		kb.program(ch, p);
		return true;
	}
	if (type == "pitch_bend" || type == "pitchbend" || type == "bend") {
		int b;
		if (!j_int(v, "value", b) && !j_int(v, "bend", b)) { err = "pitch_bend needs \"value\" (-8192..8191)"; return false; }
		kb.bend(ch, b);
		return true;
	}
	if (type == "aftertouch" || type == "poly_pressure" || type == "key_pressure") {
		int n, p;
		if (!j_note(v, "note", n) || (!j_int(v, "pressure", p) && !j_int(v, "value", p))) { err = "aftertouch needs \"note\" and \"pressure\""; return false; }
		kb.poly_pressure(ch, n, p);
		return true;
	}
	if (type == "channel_pressure" || type == "pressure" || type == "channel_aftertouch") {
		int p;
		if (!j_int(v, "pressure", p) && !j_int(v, "value", p)) { err = "channel_pressure needs \"pressure\""; return false; }
		kb.chan_pressure(ch, p);
		return true;
	}
	if (type == "panic" || type == "all_notes_off" || type == "all_sound_off") {
		sch.flush();
		kb.panic();
		return true;
	}
	err = "unknown type \"" + type + "\"";
	return false;
}

// ---------------------------------------------------------------- text mode

static const char *HELP =
	"Enter (empty line)      play a C E G chord for half a second\n"
	"sustain on|off          sustain pedal (cc 64)\n"
	"soft on|off             soft pedal (cc 67)\n"
	"sostenuto on|off        sostenuto pedal (cc 66)\n"
	"cc NUMBER VALUE         any controller change, 0..127\n"
	"note C4 [vel] [ms]      one note, default velocity 100, default 500 ms\n"
	"on C4 [vel]             note on, stays until off\n"
	"off C4                  note off\n"
	"chord C4 E4 G4          a chord for half a second\n"
	"pc NUMBER               program change, 0..127\n"
	"bend NUMBER             pitch bend, -8192..8191\n"
	"mod|volume|expression|pan VALUE   common controllers\n"
	"channel N               channel 1..16 for what follows, default 1\n"
	"velocity N              default velocity 1..127\n"
	"panic                   release everything, pedals up\n"
	"help                    this list\n"
	"quit                    leave (also q, exit, Ctrl+D)\n";

static void default_chord(Keyboard &kb, Scheduler &sch) {
	for (int n : {60, 64, 67}) {
		kb.note_on(kb.channel, n, kb.velocity);
		sch.note_off_in(500, kb.channel, n);
	}
}

static bool onoff(const std::string &w, int &val) {
	if (w == "on" || w == "down" || w == "1" || w == "true") { val = 127; return true; }
	if (w == "off" || w == "up" || w == "0" || w == "false") { val = 0; return true; }
	return false;
}

// Returns false to quit.
static bool handle_text(Keyboard &kb, Scheduler &sch, const std::string &raw) {
	std::string line = trim(raw);
	if (line.empty()) { default_chord(kb, sch); std::cout << "chord C4 E4 G4\n"; return true; }

	std::istringstream ss(line);
	std::vector<std::string> w;
	for (std::string t; ss >> t;) w.push_back(t);
	std::string cmd = lower(w[0]);
	auto arg = [&](size_t i) { return i < w.size() ? w[i] : std::string(); };
	auto num = [&](size_t i, int &out) {
		if (i >= w.size()) return false;
		char *end = nullptr; long x = std::strtol(w[i].c_str(), &end, 10);
		if (*end) return false;
		out = (int)x; return true;
	};

	if (cmd == "quit" || cmd == "q" || cmd == "exit") return false;
	if (cmd == "help" || cmd == "?" || cmd == "h") { std::cout << HELP; return true; }
	if (cmd == "panic") { sch.flush(); kb.panic(); std::cout << "panic sent\n"; return true; }

	struct Pedal { const char *name; int cc; };
	static const Pedal pedals[] = {{"sustain", 64}, {"soft", 67}, {"sostenuto", 66}};
	for (auto &pd : pedals) {
		if (cmd == pd.name) {
			int val;
			if (!onoff(lower(arg(1)), val)) { std::cout << "usage: " << pd.name << " on|off\n"; return true; }
			kb.cc(kb.channel, pd.cc, val);
			std::cout << pd.name << (val ? " on" : " off") << "\n";
			return true;
		}
	}
	static const Pedal ctrls[] = {{"mod", 1}, {"modulation", 1}, {"volume", 7}, {"expression", 11}, {"pan", 10}};
	for (auto &c : ctrls) {
		if (cmd == c.name) {
			int val;
			if (!num(1, val)) { std::cout << "usage: " << c.name << " VALUE\n"; return true; }
			kb.cc(kb.channel, c.cc, val);
			std::cout << "cc " << c.cc << " " << clamp(val, 0, 127) << "\n";
			return true;
		}
	}
	if (cmd == "cc") {
		int c, val;
		if (!num(1, c) || !num(2, val) || c < 0 || c > 127) { std::cout << "usage: cc NUMBER VALUE\n"; return true; }
		kb.cc(kb.channel, c, val);
		std::cout << "cc " << c << " " << clamp(val, 0, 127) << "\n";
		return true;
	}
	if (cmd == "pc" || cmd == "program") {
		int p;
		if (!num(1, p)) { std::cout << "usage: pc NUMBER\n"; return true; }
		kb.program(kb.channel, p);
		std::cout << "program " << clamp(p, 0, 127) << "\n";
		return true;
	}
	if (cmd == "bend") {
		int b;
		if (!num(1, b)) { std::cout << "usage: bend NUMBER (-8192..8191)\n"; return true; }
		kb.bend(kb.channel, b);
		std::cout << "bend " << clamp(b, -8192, 8191) << "\n";
		return true;
	}
	if (cmd == "channel" || cmd == "ch") {
		int c;
		if (!num(1, c) || c < 1 || c > 16) { std::cout << "usage: channel 1..16\n"; return true; }
		kb.channel = c;
		std::cout << "channel " << c << "\n";
		return true;
	}
	if (cmd == "velocity" || cmd == "vel") {
		int v;
		if (!num(1, v) || v < 1 || v > 127) { std::cout << "usage: velocity 1..127\n"; return true; }
		kb.velocity = v;
		std::cout << "velocity " << v << "\n";
		return true;
	}
	if (cmd == "note" || cmd == "on" || cmd == "off") {
		int n = parse_note(arg(1));
		if (n < 0) { std::cout << "usage: " << cmd << " C4" << (cmd == "off" ? "" : " [velocity]") << "\n"; return true; }
		if (cmd == "off") { kb.note_off(kb.channel, n); std::cout << "off " << n << "\n"; return true; }
		int vel = kb.velocity;
		if (w.size() > 2) num(2, vel);
		kb.note_on(kb.channel, n, vel);
		if (cmd == "note") {
			int ms = 500;
			if (w.size() > 3) num(3, ms);
			sch.note_off_in(ms, kb.channel, n);
		}
		std::cout << "on " << n << " velocity " << clamp(vel, 1, 127) << "\n";
		return true;
	}
	if (cmd == "chord") {
		std::vector<int> ns;
		for (size_t i = 1; i < w.size(); i++) {
			int n = parse_note(w[i]);
			if (n < 0) { std::cout << "bad note: " << w[i] << "\n"; return true; }
			ns.push_back(n);
		}
		if (ns.empty()) { default_chord(kb, sch); return true; }
		for (int n : ns) { kb.note_on(kb.channel, n, kb.velocity); sch.note_off_in(500, kb.channel, n); }
		std::cout << "chord of " << ns.size() << " notes\n";
		return true;
	}
	// A bare note name plays that note.
	int n = parse_note(cmd);
	if (n >= 0) {
		kb.note_on(kb.channel, n, kb.velocity);
		sch.note_off_in(500, kb.channel, n);
		std::cout << "note " << n << "\n";
		return true;
	}
	std::cout << "unknown command, type help\n";
	return true;
}

// ---------------------------------------------------------------- main

static void usage() {
	std::cout <<
		"nvmidi-tester - a fake MIDI keyboard\n"
		"\n"
		"  nvmidi-tester [options]\n"
		"\n"
		"  -a          automation mode: stdin is one JSON message per line\n"
		"  -n NAME     virtual port name (default \"nvmidi virtual\")\n"
		"  -p NAME     use the existing output port whose name contains NAME\n"
		"              (Windows: needed when the port is not called \"nvmidi virtual\")\n"
		"  -l          list output ports and exit\n"
		"  --loopmidi  Windows: skip the built-in virtual device, use a loopMIDI port\n"
		"  -c N        channel 1..16 (default 1)\n"
		"  -h          this help\n"
		"\n"
		"Without -a: Enter plays a C E G chord for half a second; type help for commands.\n"
		"JSON examples (with -a):\n"
		"  {\"type\":\"note_on\",\"note\":\"C4\",\"velocity\":100,\"duration\":500}\n"
		"  {\"type\":\"note_off\",\"note\":60}\n"
		"  {\"type\":\"sustain\",\"value\":\"on\"}\n"
		"  {\"type\":\"cc\",\"control\":67,\"value\":127}\n"
		"  {\"type\":\"chord\",\"notes\":[\"C4\",\"E4\",\"G4\"],\"duration\":500}\n"
		"  {\"bytes\":[144,60,100]}\n"
		"Types: note_on note_off chord cc sustain soft sostenuto program_change pitch_bend\n"
		"       aftertouch channel_pressure panic. Channels are 1..16. Middle C is C4 = 60.\n";
}

int main(int argc, char **argv) {
	bool automation = false, list_only = false, force_loopmidi = false;
	std::string vname = "nvmidi virtual", pname;
	int channel = 1;

	for (int i = 1; i < argc; i++) {
		std::string a = argv[i];
		if (a == "-a") automation = true;
		else if (a == "-l") list_only = true;
		else if (a == "--loopmidi") force_loopmidi = true;
		else if (a == "-h" || a == "--help" || a == "-?") { usage(); return 0; }
		else if (a == "-n" && i + 1 < argc) vname = argv[++i];
		else if (a == "-p" && i + 1 < argc) pname = argv[++i];
		else if (a == "-c" && i + 1 < argc) channel = std::atoi(argv[++i]);
		else { std::cerr << "unknown option: " << a << "\n"; usage(); return 2; }
	}
	if (channel < 1 || channel > 16) { std::cerr << "channel must be 1..16\n"; return 2; }

	(void)force_loopmidi; // only the Windows MSVC build reads it
	Keyboard kb;
	kb.channel = channel;
	try {
		kb.out.reset(new RtMidiOut());
	} catch (RtMidiError &e) {
		std::cerr << "error: " << e.getMessage() << "\n"
		          << "No MIDI system available (on Linux: is the ALSA sequencer loaded? try: sudo modprobe snd-seq)\n";
		return 1;
	}
	kb.out->setErrorCallback([](RtMidiError::Type t, const std::string &m, void *) {
		if (t != RtMidiError::WARNING && t != RtMidiError::DEBUG_WARNING) std::cerr << "error: " << m << "\n";
	});

	unsigned count = kb.out->getPortCount();
	if (list_only) {
		for (unsigned i = 0; i < count; i++) std::cout << i << ": " << kb.out->getPortName(i) << "\n";
		if (!count) std::cout << "no output ports\n";
		return 0;
	}

	try {
#ifdef _WIN32
#ifdef NVMIDI_WMS
		if (!force_loopmidi && pname.empty()) {
			wms::Result r = wms::start(vname);
			if (r.status == wms::Status::Ok) {
				kb.use_wms = true;
				std::cout << "ready, virtual port \"" << vname << "\" is open (Windows MIDI Services)\n";
			} else {
				switch (r.status) {
					case wms::Status::NotWindows11:
						std::cerr << "This is not Windows 11 (" << r.detail << "), so there are no built-in virtual MIDI ports.\n";
						break;
					case wms::Status::OldWindows11:
						std::cerr << "This Windows 11 is older than 25H2 (" << r.detail << "), so there are no built-in virtual MIDI ports. Updating Windows fixes that.\n";
						break;
					default:
						std::cerr << "Windows MIDI Services is not usable here: " << r.detail << "\n";
				}
				std::cerr << "Download loopMIDI instead: https://www.tobias-erichsen.de/software/loopmidi.html\n"
				          << "Install it, create a port named \"" << vname << "\", then start this again.\n";
			}
		}
#endif
		if (!kb.use_wms) {
			// winmm cannot create a port, so find one somebody else made.
			std::string want = lower(pname.empty() ? vname : pname);
			int found = -1;
			for (unsigned i = 0; i < count; i++)
				if (lower(kb.out->getPortName(i)).find(want) != std::string::npos) { found = (int)i; break; }
			if (found < 0) {
				std::cerr << "error: no output port with \"" << want << "\" in its name.\n";
#ifndef NVMIDI_WMS
				std::cerr << "Windows cannot create virtual MIDI ports by itself. Download loopMIDI: https://www.tobias-erichsen.de/software/loopmidi.html\n"
				          << "Install it, create a port named \"" << vname << "\", then start this again.\n";
#endif
				std::cerr << "Output ports I can see:\n";
				for (unsigned i = 0; i < count; i++) std::cerr << "  " << i << ": " << kb.out->getPortName(i) << "\n";
				if (!count) std::cerr << "  none\n";
				return 1;
			}
			kb.out->openPort((unsigned)found, "nvmidi-tester");
			std::cout << "ready, sending to port " << kb.out->getPortName((unsigned)found) << "\n";
		}
#else
		if (!pname.empty()) {
			std::string want = lower(pname);
			int found = -1;
			for (unsigned i = 0; i < count; i++)
				if (lower(kb.out->getPortName(i)).find(want) != std::string::npos) { found = (int)i; break; }
			if (found < 0) { std::cerr << "error: no output port with \"" << pname << "\" in its name\n"; return 1; }
			kb.out->openPort((unsigned)found, "nvmidi-tester");
			std::cout << "ready, sending to port " << kb.out->getPortName((unsigned)found) << "\n";
		} else {
			kb.out->openVirtualPort(vname);
			std::cout << "ready, virtual port \"" << vname << "\" is open\n";
		}
#endif
	} catch (RtMidiError &e) {
		std::cerr << "error: " << e.getMessage() << "\n";
		return 1;
	}
	std::cout << std::flush;

	Scheduler sch(kb);
	if (!automation) std::cout << "Press Enter to play a chord. Type help for commands.\n" << std::flush;

	std::string line;
	while (std::getline(std::cin, line)) {
		if (!line.empty() && line.back() == '\r') line.pop_back();
		if (automation) {
			if (trim(line).empty()) continue;
			std::string err;
			if (!handle_json(kb, sch, line, err)) std::cerr << "error: " << err << "\n";
		} else {
			if (!handle_text(kb, sch, line)) break;
		}
		std::cout << std::flush;
	}

	// Leave nothing sounding: a stuck note in the game under test is a false bug.
	sch.flush();
	sch.stop();
	kb.panic();
	std::this_thread::sleep_for(std::chrono::milliseconds(50));
#ifdef NVMIDI_WMS
	if (kb.use_wms) wms::stop();
#endif
	return 0;
}
