// Windows MIDI Services backend for nvmidi-tester (Windows 11 25H2 or newer).
// Creates a real virtual MIDI device from code, so nothing has to be
// installed. Only compiled into the MSVC build (NVMIDI_WMS).
#pragma once
#include <string>
#include <vector>

namespace wms {

enum class Status {
	Ok,
	NotWindows11,   // Windows 10 or older
	OldWindows11,   // Windows 11 before 25H2: no usable API
	ApiMissing,     // the API dll could not be loaded
	ServiceOff,     // service disabled or the PC is in Legacy API mode
	Failed          // anything else; detail says what
};

struct Result {
	Status status = Status::Failed;
	unsigned build = 0;
	std::string detail;
};

// Creates the device called `name` and opens it. On success the device is
// visible to every MIDI app, old WinMM ones included.
Result start(const std::string &name);

// Sends one MIDI 1.0 channel voice message (status 0x80..0xEF).
bool send(const std::vector<unsigned char> &message, std::string &error);

// Removes the device. Never blocks longer than a couple of seconds.
void stop();

} // namespace wms
