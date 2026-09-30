// probe_host.cpp - run a plugin the way nvgt runs it, but here, under a
// debugger and with a real Angelscript engine.
//
// Why this exists
// ---------------
//
// midi_probe.py handed the plugin a zeroed nvgt_plugin_shared. That structure
// is mostly a table of function pointers (see NVGT_PLUGIN_FUNCTIONS in
// src/nvgt_plugin.h - nvgt_wait, refresh_window, ticks, nvgt_datastream_create
// and about thirty more). prepare_plugin() copies every one of them into the
// plugin's globals and then calls asPrepareMultithread(shared->script_thread_manager).
//
// With a zeroed structure that call goes to address 0, so the plugin crashes
// during preparation - before it has looked at a single MIDI port. The probe
// reported that honestly and I read it as a bug in the plugin. It was not: a
// plugin that is given no engine cannot behave any other way. The measurement
// was of the stub, not of the plugin.
//
// This host removes the doubt. It builds the same structure nvgt builds:
//
//   * the Angelscript functions come from the real Angelscript library,
//     linked in here, so asPrepareMultithread() and every registration call
//     land on working code;
//   * the nvgt functions (nvgt_wait, ticks, ...) are local stand-ins, because
//     they belong to the engine rather than to Angelscript and only the engine
//     can supply them. They are counted, so the log shows whether the plugin
//     asked for any;
//   * script_engine is a real asIScriptEngine, so register_nvmidi() runs its
//     complete course - every type, every method, every global - and any
//     misuse of the registration api comes back as an Angelscript error code
//     or a crash here, in a process with a stack trace.
//
// A clean run is the strongest statement available without the engine itself:
// the plugin registered its whole surface against a working Angelscript.
//
// Build and run:
//
//   g++ -std=c++17 -O1 -fPIC -Isrc -Ithird_party/angelscript \
//       -DNVGT_PLUGIN_STATIC -c src/nvmidi.cpp -o nvmidi_static.o
//   g++ -std=c++17 -O1 -fPIC -Isrc -Ithird_party/angelscript \
//       -DNVGT_PLUGIN_STATIC tools/probe_host.cpp nvmidi_static.o \
//       third_party/rtmidi/RtMidi.cpp third_party/angelscript/scriptarray.cpp \
//       -langelscript -lpthread -o probe_host
//   ./probe_host
//
// NVGT_PLUGIN_STATIC is not a trick to avoid the dll: it is what the header
// asks for when the plugin is compiled into the host rather than loaded from
// disk, and it makes the plugin call the Angelscript functions directly
// instead of through the pointer table. Same code either way - the table is
// only how the functions are found, not what they do.

#define NVGT_PLUGIN_STATIC 1

// The plugin's own translation unit defines the entry point and the version
// function through the header's macros; this one declares the two names it
// will call.
#include "nvgt_plugin.h"

// Load-bearing, and not an oversight: the vendored RtMidi keeps its template
// definitions in RtMidi.cpp instead of the header, so the header alone only
// gives declarations. Including it here is what makes this translation unit
// instantiate them; drop it and every RtMidi call in the plugin object becomes
// an undefined reference at link time.
#include <RtMidi.h>

#include <cstdio>
#include <cstdint>
#include <string>

// Only the plugin needs the RtMidi header; the host itself never names
// an RtMidi type. Including it here is what tells the compiler to
// instantiate the vendored template definitions - a host that does not
// would link nvmidi_static.o against an empty RtMidi and only find out
// at run time, when the plugin asks for a port.
namespace {

// The nvgt half of the table. Every one of these is something the engine does
// and a bare host cannot: waiting, redrawing, reading its own clock, packing
// assets. The plugin touches none of them on the registration path, which is
// exactly the claim this host is built to test - so each one counts its calls
// and the summary at the end says whether that held.
int nvgt_calls = 0;

#define NVGT_STANDIN(ret, name, args) \
	ret name args { \
		++nvgt_calls; \
		std::fprintf(stderr, "  nvgt call: %s (the plugin asked the engine for something)\n", #name); \
		return ret(); \
	}

void* nvgt_datastream_create_standin(std::ios*, const std::string&, int) { ++nvgt_calls; std::fprintf(stderr, "  nvgt call: nvgt_datastream_create\n"); return nullptr; }
std::ios* nvgt_datastream_get_ios_standin(void*) { ++nvgt_calls; std::fprintf(stderr, "  nvgt call: nvgt_datastream_get_ios\n"); return nullptr; }
void nvgt_bundle_shared_library_standin(const std::string&) { ++nvgt_calls; std::fprintf(stderr, "  nvgt call: nvgt_bundle_shared_library\n"); }
bool find_embedded_pack_standin(std::string&, uint64_t&, uint64_t&) { ++nvgt_calls; std::fprintf(stderr, "  nvgt call: find_embedded_pack\n"); return false; }
void nvgt_wait_standin(int) { ++nvgt_calls; std::fprintf(stderr, "  nvgt call: nvgt_wait\n"); }
void refresh_window_standin() { ++nvgt_calls; std::fprintf(stderr, "  nvgt call: refresh_window\n"); }
uint64_t ticks_standin(bool) { ++nvgt_calls; return 0; }
uint64_t microticks_standin(bool) { ++nvgt_calls; return 0; }
std::string string_aes_encrypt_standin(const std::string&, std::string) { ++nvgt_calls; return ""; }
std::string string_aes_decrypt_standin(const std::string&, std::string) { ++nvgt_calls; return ""; }
void nvgt_audio_plugin_node_register_standin(const std::string&) { ++nvgt_calls; }
plugin_node* nvgt_audio_plugin_node_create_standin(audio_plugin_node_interface*, unsigned char, unsigned char, unsigned int, audio_engine*) { ++nvgt_calls; return nullptr; }
audio_plugin_node_interface* nvgt_audio_plugin_node_get_standin(plugin_node*) { ++nvgt_calls; return nullptr; }
bool running_on_mobile_standin() { ++nvgt_calls; return false; }

} // namespace

// The entry point the plugin's own translation unit defines. The header only
// gives the macro that *spells* its name, so the declaration is written out.
#ifdef NVGT_PLUGIN_STATIC
#define PROBE_CAT_(a, b) a##b
#define PROBE_CAT(a, b) PROBE_CAT_(a, b)
#define PROBE_PLUGIN_MAIN PROBE_CAT(nvgt_plugin_, NVGT_PLUGIN_STATIC)
#define PROBE_PLUGIN_VERSION PROBE_CAT(nvgt_plugin_version_, NVGT_PLUGIN_STATIC)
#else
#define PROBE_PLUGIN_MAIN nvgt_plugin
#define PROBE_PLUGIN_VERSION nvgt_plugin_version
#endif
extern "C" bool PROBE_PLUGIN_MAIN(nvgt_plugin_shared*);

int main() {
	// The header's own accessors, not hand-written symbol names: in
	// NVGT_PLUGIN_STATIC mode the entry point is nvgt_plugin_<NVGT_PLUGIN_STATIC>,
	// and spelling that out here is how a host ends up declaring a symbol
	// nothing defines. plugin_version() is defined by the header in this same
	// translation unit.
	// The version function is a definition, not a callable name, in this
	// translation unit - the header's own definition lives in whichever file it
	// is expanded in. The plugin's file defines it, so it is declared and called
	// by its composed name.
	std::printf("the plugin's api version is %d\n", PROBE_PLUGIN_VERSION());

	nvgt_plugin_shared shared{};
	shared.version = NVGT_PLUGIN_API_VERSION;

	// The Angelscript half, real. asCreateScriptEngine is the engine the plugin
	// will register into; its own helpers are linked from libangelscript.
	shared.script_engine = asCreateScriptEngine();
	if (!shared.script_engine) {
		std::printf("could not create an Angelscript engine; nothing can be tested\n");
		return 2;
	}
	std::printf("a real Angelscript engine was created (version %s)\n", asGetLibraryVersion());

	// The nvgt half, stand-ins that report if they are ever called.
	shared.f_nvgt_datastream_create = &nvgt_datastream_create_standin;
	shared.f_nvgt_datastream_get_ios = &nvgt_datastream_get_ios_standin;
	shared.f_nvgt_bundle_shared_library = &nvgt_bundle_shared_library_standin;
	shared.f_find_embedded_pack = &find_embedded_pack_standin;
	shared.f_nvgt_wait = &nvgt_wait_standin;
	shared.f_refresh_window = &refresh_window_standin;
	shared.f_ticks = &ticks_standin;
	shared.f_microticks = &microticks_standin;
	shared.f_string_aes_encrypt = &string_aes_encrypt_standin;
	shared.f_string_aes_decrypt = &string_aes_decrypt_standin;
	shared.f_nvgt_audio_plugin_node_register = &nvgt_audio_plugin_node_register_standin;
	shared.f_nvgt_audio_plugin_node_create = &nvgt_audio_plugin_node_create_standin;
	shared.f_nvgt_audio_plugin_node_get = &nvgt_audio_plugin_node_get_standin;
	shared.f_running_on_mobile = &running_on_mobile_standin;

	// script_thread_manager is deliberately left null: asPrepareMultithread
	// treats null as "make me a default thread manager", which is a supported
	// call rather than a hole. If it were not, this line would crash and the
	// stack trace would say so.
	std::printf("calling the entry point, with a real engine and no stub table...\n");
	std::fflush(stdout);
	const bool accepted = PROBE_PLUGIN_MAIN(&shared);
	std::printf("the entry point returned %s\n", accepted ? "true" : "false");

	if (accepted) {
		std::printf("REGISTERED: the plugin ran its whole registration against a live Angelscript\n");
		std::printf("the plugin asked the nvgt side of the table %d time(s); 0 means the\n", nvgt_calls);
		std::printf("registration path never reached for the engine at all\n");
	}

	shared.script_engine->Release();
	asUnprepareMultithread();
	return accepted ? 0 : 1;
}
