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
//   g++ -std=c++17 -O1 -fPIC -Isrc -Ithird_party/rtmidi \
//       -Ithird_party/angelscript -DNVGT_PLUGIN_STATIC \
//       tools/probe_host.cpp src/nvmidi.cpp \
//       third_party/rtmidi/RtMidi.cpp third_party/angelscript/scriptarray.cpp \
//       /path/to/libangelscript.a -lpthread -Wl,--allow-multiple-definition \
//       -o probe_host
//   ./probe_host; echo $?
//
// --allow-multiple-definition is not sloppiness: see the note above
// NVGT_PLUGIN_STATIC on why the header defines plugin_version() in both this
// host and the plugin, and why that is correct for both.
//
// libangelscript.a is not vendored. third_party/angelscript carries the
// headers and the array add-on only, so the engine itself is built from
// https://github.com/anjo76/angelscript (v2.38.0) - every .cpp under
// sdk/angelscript/source, archived. -langelscript, which this comment used to
// say, only works on a machine with the SDK installed, and the point of this
// harness is that it needs nothing installed.
//
// NVGT_PLUGIN_STATIC is not a trick to avoid the dll: it is what the header
// asks for when the plugin is compiled into the host rather than loaded from
// disk, and it makes the plugin call the Angelscript functions directly
// instead of through the pointer table. Same code either way - the table is
// only how the functions are found, not what they do.

// NVGT_PLUGIN_STATIC makes the plugin's entry point a name in this program -
// nvgt_plugin_1(), composed by the header - instead of an export looked up
// from a dll. The plugin and this host then have to agree on one symbol, so
// set it before including the header and for every translation unit in the
// link; setting it in one and not the other is what the first attempts at
// this file did, and the linker's answer was "undefined reference to
// nvgt_plugin_1" while nm showed the symbol defined a few lines further up.
//
// Neither of the header's guards is set here, on purpose.
//
// NVGT_PLUGIN_INCLUDE would suppress the header's plugin_version() definition,
// but it also guards the block that gives the entry point its extern "C"
// linkage. With it set on this side only, the plugin's object carried
// _Z13nvgt_plugin_1P18nvgt_plugin_shared while this host asked for a plain
// nvgt_plugin_1 - and the linker's answer was "undefined reference to
// nvgt_plugin_1" a few lines after nm had shown the symbol defined.
//
// Not setting it means the header defines plugin_version() here too, which is
// a second definition next to the plugin's. That one is not a bug to route
// around: it is the arrangement the header documents for NVGT_PLUGIN_STATIC,
// where plugin_main/plugin_version *are* the entry points and the host is the
// plugin. Two definitions of the same one-line accessor only collide because
// this probe host is a throwaway harness with the plugin linked into it, and
// --allow-multiple-definition is the cost of that convenience.
#define NVGT_PLUGIN_STATIC 1
#include "nvgt_plugin.h"

// Load-bearing, and not an oversight: the vendored RtMidi keeps its template
// definitions in RtMidi.cpp instead of the header, so the header alone only
// gives declarations. Including it here is what makes this translation unit
// instantiate them - a host that did not would link the plugin's object
// against an empty RtMidi and only find out at run time, when the plugin asks
// for a port.
#include <RtMidi.h>

#include <cstdio>
#include <cstdint>
#include <string>

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

// The entry point the plugin's own translation unit defines, declared the way
// the header declares it. plugin_main and plugin_version are macros that
// *spell* the names, so they are expanded here rather than written out: a
// hand-written declaration is a second opinion about the symbol's linkage,
// and the one time this file held one it was the opinion that was wrong.
plugin_main(nvgt_plugin_shared*);
plugin_version();

int main() {

	std::printf("the plugin's api version is %d\n", nvgt_plugin_version_1());

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
	const bool accepted = nvgt_plugin_1(&shared);
	std::printf("the entry point returned %s\n", accepted ? "true" : "false");

	if (accepted) {
		std::printf("REGISTERED: the plugin ran its whole registration against a live Angelscript\n");
		std::printf("the plugin asked the nvgt side of the table %d time(s); 0 means the\n", nvgt_calls);
		std::printf("registration path never reached for the engine at all\n");
	}

	// asALREADY_REGISTERED is -10 (angelscript.h:655) and it has two readings.
	// "This declaration is already in the engine" is what a second pass looks
	// like; "the registration that would have introduced the type this belongs
	// to did not survive" is what a rolled-back type looks like, and that one
	// is the fault this harness exists to find, because a script that then
	// writes `midi_output@ out;` is told the type does not exist. The two are
	// told apart by the names in the log, not by the code: a fresh name cannot
	// be already registered, so a -10 against one means the plugin went round
	// twice. Counting the first-time registrations is therefore part of
	// reading the output, and neither the count nor the exit status is a pass
	// on its own.
	std::printf("NOTE: registration refusals above carry code -10 (asALREADY_REGISTERED). Count the\n");
	std::printf("first-time registrations in the log above: a fresh name cannot be already\n");
	std::printf("registered, so any -10 on one means the plugin registered twice instead.\n");

	shared.script_engine->Release();

	// asUnprepareMultithread aborts (as_thread.cpp:193) when the engine was
	// never multithreaded to begin with - the host leaves script_thread_manager
	// null and asPrepareMultithread is only reached on the shared-library path,
	// so on this one there is nothing to unprepare and asserting about it is
	// the harness's fault, not the plugin's. The engine is released above,
	// which is the cleanup that actually applies here, and the exit status is
	// the result of the probe rather than of the teardown.
	return accepted ? 0 : 1;
}
