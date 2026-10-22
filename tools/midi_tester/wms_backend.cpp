// See wms_backend.h. Written against the Windows MIDI Services C++/WinRT
// samples (github.com/microsoft/MIDI, samples/cpp-winrt).

#include "wms_backend.h"

#include <windows.h>
#include <mmsystem.h>

#include <atomic>
#include <chrono>
#include <iostream>
#include <thread>

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Devices.Midi2.h>
#include <winrt/Windows.Devices.Midi2.Enumeration.h>
#include <winrt/Windows.Devices.Midi2.Transports.Virtual.h>

using namespace winrt::Windows::Devices::Midi2;
using namespace winrt::Windows::Devices::Midi2::Enumeration;
using namespace winrt::Windows::Devices::Midi2::Transports::Virtual;

namespace {

constexpr DWORD Build11 = 22000;     // Windows 11 21H2
constexpr DWORD Build25H2 = 26200;   // the first build the API is supported on

MidiSession g_session{ nullptr };
MidiVirtualDevice g_device{ nullptr };
MidiEndpointConnection g_connection{ nullptr };
bool g_open = false;

DWORD windows_build() {
	using Fn = LONG(WINAPI *)(PRTL_OSVERSIONINFOW);
	auto fn = reinterpret_cast<Fn>(::GetProcAddress(::GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion"));
	RTL_OSVERSIONINFOW info{ sizeof(info) };
	if (!fn || fn(&info) != 0) return 0;
	return info.dwBuildNumber;
}

std::string utf8(const std::wstring &w) {
	if (w.empty()) return {};
	int n = ::WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
	std::string s((size_t)n, '\0');
	::WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), s.data(), n, nullptr, nullptr);
	return s;
}

std::wstring wide(const std::string &s) {
	if (s.empty()) return {};
	int n = ::MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
	std::wstring w((size_t)n, L'\0');
	::MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), w.data(), n);
	return w;
}

std::string hex(uint32_t v) {
	char b[16];
	std::snprintf(b, sizeof b, "0x%08x", v);
	return b;
}

// Tell the user how the device shows up to a classic WinMM app, because that
// is the name the game will list.
void print_winmm_names(const std::string &needle) {
	std::string want;
	for (char c : needle) want += (char)std::tolower((unsigned char)c);
	bool any = false;
	for (UINT i = 0, n = ::midiInGetNumDevs(); i < n; i++) {
		MIDIINCAPSW caps{};
		if (::midiInGetDevCapsW(i, &caps, sizeof caps) != MMSYSERR_NOERROR) continue;
		std::string name = utf8(caps.szPname), low;
		for (char c : name) low += (char)std::tolower((unsigned char)c);
		if (low.find(want) != std::string::npos) {
			std::cout << "MIDI input seen by ordinary apps: " << name << " (number " << i << ")\n";
			any = true;
		}
	}
	if (!any) std::cout << "note: the port is not in the classic MIDI input list yet. Give it a second, or restart the game.\n";
}

} // namespace

namespace wms {

Result start(const std::string &name) {
	Result r;
	r.build = windows_build();

	if (r.build < Build11) {
		r.status = Status::NotWindows11;
		r.detail = "Windows build " + std::to_string(r.build);
		return r;
	}
	if (r.build < Build25H2) {
		r.status = Status::OldWindows11;
		r.detail = "Windows 11 build " + std::to_string(r.build);
		return r;
	}

	try {
		winrt::init_apartment(winrt::apartment_type::multi_threaded);

		// Resolves from Windows itself, or from Windows.Devices.Midi2.dll
		// lying next to this exe. Throws when neither is there.
		bool available = false;
		try {
			available = MidiApi::EnsureServiceAvailable();
		} catch (winrt::hresult_error const &e) {
			r.status = Status::ApiMissing;
			r.detail = "the MIDI API did not load, error " + hex((uint32_t)e.code()) +
			           " (is Windows.Devices.Midi2.dll next to the exe?)";
			return r;
		}
		if (!available) {
			r.status = Status::ServiceOff;
			r.detail = "the MIDI service is off, or Windows is set to the Legacy MIDI API";
			return r;
		}

		std::wstring wname = wide(name);

		MidiDeclaredEndpointInfo info;
		info.Name(wname);
		info.ProductInstanceId(L"NVMIDITESTER0001");
		info.SupportsMidi10Protocol(true);
		info.SupportsMidi20Protocol(false);
		info.SupportsReceivingJitterReductionTimestamps(false);
		info.SupportsSendingJitterReductionTimestamps(false);
		info.SpecificationVersionMajor(1);
		info.SpecificationVersionMinor(1);
		info.HasStaticFunctionBlocks(true);

		MidiVirtualDeviceCreationConfig config(
			wname, L"nvmidi-tester fake MIDI keyboard", L"nvmidi", info);

		// One function block, one group. The block's name is also the name a
		// classic WinMM app sees, so it stays short.
		MidiFunctionBlock block;
		block.Number(0);
		block.IsActive(true);
		block.Name(wname);
		block.FirstGroup(MidiGroup(static_cast<uint8_t>(0)));
		block.GroupCount(1);
		block.Direction(MidiFunctionBlockDirection::Bidirectional);
		block.RepresentsMidi10Connection(MidiFunctionBlockRepresentsMidi10Connection::Not10);
		config.FunctionBlocks().Append(block);

		g_session = MidiSession::Create(L"nvmidi-tester");
		if (!g_session) { r.detail = "could not create a MIDI session"; return r; }

		g_device = MidiVirtualDeviceManager::CreateVirtualDevice(config);
		if (!g_device) { r.detail = "could not create the virtual device"; return r; }

		g_connection = g_session.CreateEndpointConnection(g_device.DeviceEndpointDeviceId());
		if (!g_connection) { r.detail = "could not connect to the virtual device"; return r; }

		if (g_connection.AddMessageProcessingPlugin(g_device) != MidiMessageProcessingPluginAddResult::Succeeded) {
			r.detail = "could not attach the virtual device to its connection";
			return r;
		}
		if (!g_connection.Open()) { r.detail = "could not open the virtual device"; return r; }
		g_open = true;

		// The app-facing endpoint is made by the service a moment later.
		std::this_thread::sleep_for(std::chrono::milliseconds(1200));
		print_winmm_names(name);

		r.status = Status::Ok;
		return r;
	} catch (winrt::hresult_error const &e) {
		r.status = Status::Failed;
		r.detail = "error " + hex((uint32_t)e.code()) + ": " + utf8(std::wstring(e.message()));
		return r;
	} catch (std::exception const &e) {
		r.status = Status::Failed;
		r.detail = e.what();
		return r;
	}
}

bool send(const std::vector<unsigned char> &m, std::string &error) {
	if (!g_open) { error = "the virtual device is not open"; return false; }
	if (m.empty() || m[0] < 0x80 || m[0] >= 0xF0) {
		error = "only channel messages (status 0x80..0xEF) work with the Windows MIDI Services backend";
		return false;
	}
	uint32_t status = m[0];
	uint32_t d1 = m.size() > 1 ? (m[1] & 0x7f) : 0;
	uint32_t d2 = m.size() > 2 ? (m[2] & 0x7f) : 0;
	// UMP type 2: MIDI 1.0 channel voice message, group 0.
	uint32_t word = (0x2u << 28) | (status << 16) | (d1 << 8) | d2;
	try {
		auto result = g_connection.SendSingleMessageWords(MidiClock::Now(), word);
		if (MidiEndpointConnection::SendMessageFailed(result)) {
			error = "the message was not sent";
			return false;
		}
		return true;
	} catch (winrt::hresult_error const &e) {
		error = "send failed, error " + hex((uint32_t)e.code());
		return false;
	}
}

void stop() {
	if (!g_open) return;
	g_open = false;
	// Older Windows MIDI Services builds can hang while a virtual device is
	// torn down. Give it two seconds, then end the process: the service drops
	// the device when the process is gone either way.
	std::atomic<bool> done{ false };
	std::thread t([&done] {
		try {
			g_connection.RemoveMessageProcessingPlugin(g_device.PluginId());
			g_session.Close();
		} catch (...) {}
		done = true;
	});
	for (int i = 0; i < 40 && !done; i++) std::this_thread::sleep_for(std::chrono::milliseconds(50));
	if (!done) {
		t.detach();
		::TerminateProcess(::GetCurrentProcess(), 0);
	} else {
		t.join();
	}
}

} // namespace wms
