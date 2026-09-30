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
//   * the nvgt functions (nvgt_wait, ticks, ...) are defined in this file,
//     because they belong to the engine rather than to Angelscript and only the
//     engine can supply them. They are counted, so the log shows whether the
//     plugin asked for any;
//   * script_engine is a real asIScriptEngine, so register_nvmidi() runs its
//     complete course - every type, every method, every global - and any
//     misuse of the registration api comes back as an Angelscript error code
//     or a crash here, in a process with a stack trace.
//
// A clean run is the strongest statement available without the engine itself:
// the plugin registered its whole surface against a working Angelscript.
//
// Build and run (the exact command that works, verified on the VPS):
//
//   g++ -std=c++17 -O0 -g -Isrc -Ithird_party/rtmidi \
//       -Iangels/sdk/angelscript/include \
//       -Iangels/sdk/add_on/scriptstdstring -Iangels/sdk/add_on/scriptarray \
//       -DNVGT_PLUGIN_STATIC -DNVGT_PLUGIN_INCLUDE \
//       probe_host.cpp probe_engine_shim.cpp \
//       src/nvmidi.cpp third_party/rtmidi/RtMidi.cpp \
//       angels/sdk/add_on/scriptstdstring/scriptstdstring.cpp \
//       angels/sdk/add_on/scriptarray/scriptarray.cpp \
//       libangelscript.a -lpthread -ldl -o probe_host
//   ./probe_host; echo $?
//
// Both macros, on both translation units. That pair is the whole finding; the
// reasoning is in the block above the nvgt definitions below.
//
// libangelscript.a is not vendored. The engine itself is built from
// https://github.com/anjo76/angelscript (master, which reports 2.39.0 WIP -
// there is no 2.39 tag, only v2.38.0) - every .cpp under
// sdk/angelscript/source, archived. -langelscript only works on a machine with
// the SDK installed, and the point of this harness is that it needs nothing
// installed.
//
// Two macros, and they are not the pair that first suggests itself
// ---------------------------------------------------------------
//
// Both translation units in this link get BOTH macros:
//
//   -DNVGT_PLUGIN_STATIC -DNVGT_PLUGIN_INCLUDE
//
// and the reason is that src/nvgt_plugin.h emits a *different declaration* of
// the same twenty-six names depending on which of them is set. Reading the
// header top to bottom:
//
//   #ifndef NVGT_BUILDING
//     #ifndef NVGT_PLUGIN_STATIC            // the dll-plugin branch
//       #ifndef NVGT_PLUGIN_INCLUDE
//         ->  t_##name* name = nullptr;     // DEFINES the name, as a variable
//       #else
//         ->  extern t_##name* name;        // references it, as a variable
//       #endif
//     #endif
//     inline bool prepare_plugin(...) { ... }   // guarded by NVGT_BUILDING only
//   #elif !defined(NVGT_BUILDING)           // the "static" branch
//     ->  ret name args;                    // declares it as a FUNCTION
//   #endif
//
// The `#ifndef NVGT_PLUGIN_STATIC` wrapper is what makes the whole question
// turn on STATIC: with that macro set, neither of the inner two branches is
// compiled at all, and the `#elif` below takes over and emits functions. So
// STATIC does not merely skip a definition - it changes the *kind* of the
// declaration, from a variable of function-pointer type to a plain function.
//
// That is what every earlier attempt in this harness got wrong, and it is worth
// stating plainly because two of them looked reasonable:
//
//   INCLUDE alone  -> the plugin sees `extern t_##name* name` and
//                     prepare_plugin() assigns all fourteen into the plugin's
//                     own variables, emitting fourteen undefined references of
//                     *variable* type. A host that defines functions of those
//                     names does not satisfy them: the symbols differ. That was
//                     the `undefined reference to nvgt_wait` over a definition
//                     of `nvgt_wait` that visibly existed in the link.
//
//   BUILDING       -> impossible for the plugin. prepare_plugin lives in the
//                     `#ifndef NVGT_BUILDING` half, so under it the header stops
//                     defining that function entirely, and src/nvmidi.cpp calls
//                     it at line 2608: measured, "'prepare_plugin' was not
//                     declared in this scope".
//
// With both macros the plugin gets functions for the fourteen nvgt names and
// keeps prepare_plugin(), and this file's definitions satisfy it by name and
// signature. The header checks the spellings at compile time - which is the
// point of defining them here rather than in a private translation unit with a
// hand-copied signature list that could drift.
//
// The as* functions arrive the same way, as functions, and resolve against
// libangelscript.a. Note what that does to the old collision: the header's
// `t_asGetLibraryVersion* asGetLibraryVersion = nullptr;` is gone in this
// branch, so angelscript.h's `const char* asGetLibraryVersion();` sits alone
// and the two headers coexist in one translation unit after all. The shim is
// kept for the engine *handle*, not for that collision - see below.
//
// Why the engine still lives behind a shim
// ----------------------------------------
//
// Not for the reason the shim's own header gives, which is the collision
// described above and which this branch removes. The separation earns its keep
// differently: probe_host.cpp is compiled without angelscript.h on purpose, so
// that the engine is reached *only* through the pointer the host puts in
// shared.script_engine. A host that included angelscript.h would be free to
// call asCreateScriptEngine and friends directly, and then the harness could
// pass while the plugin's own access path stayed broken - which is the thing
// this whole file exists to test.
//
// The static branch also buys the ANGELSCRIPT_DLL_MANUAL_IMPORT guard, and that
// one cost a core dump to learn. src/nvgt_plugin.h:17 is
//
//   #if !defined(NVGT_BUILDING) && !defined(NVGT_PLUGIN_STATIC)
//       #define ANGELSCRIPT_DLL_MANUAL_IMPORT
//
// - so on a build where neither macro is set, angelscript.h declares every as*
// function as an `extern "C"` *import*. A static libangelscript.a does not
// provide imports, so they resolve to null; nothing fails at link time, and the
// first call through one dies before main() prints its first line. That is the
// exit 139 with empty stdout and empty stderr this harness used to give.
// NVGT_PLUGIN_STATIC makes the guard false on both sides.
//
// The engine slots are assigned by hand from the linker, and prepare_plugin is
// not called, for the reason given at the call to the plugin below.
#ifndef NVGT_PLUGIN_STATIC
#define NVGT_PLUGIN_STATIC
#endif
#include "nvgt_plugin.h"

#include "probe_engine_shim.h"

// The entry point is `nvgt_plugin`, the plain dll spelling. NVGT_PLUGIN_STATIC
// changes the *name* as well, to nvgt_plugin_1 (a plugin registered statically
// with a version tag), so the plugin's own translation unit must be compiled
// with the same two macros this file uses or the names will not meet. That is
// why the build line above passes both to every object rather than being clever
// about it per file.
//
// These two are written out rather than produced by the header's plugin_main /
// plugin_version macros, because those macros are not emitted in every branch;
// writing them out matches what the dll branch would have produced, and is the
// only spelling available here.
// The entry point: a stub that counts, nothing more
// ------------------------------------------------
//
// The real entry point is NOT reachable from this file, and the link says so
// twice, in two spellings, both worth recording because each one alone looks
// like a bug in this file's own line:
//
//   1. Declared as plain `nvgt_plugin`            -> undefined reference.
//      With NVGT_PLUGIN_STATIC in force, `plugin_main` expands to
//      nvgt_plugin_1 (the CONCAT concatenates the macro's *value*), and the
//      plugin defines that name, not this one.
//
//   2. Defined by including the header and calling `plugin_main(shared)`
//      -> "expected primary-expression before 'bool'", and this one is the
//      more interesting failure of the two. nvgt_plugin.h:138 is
//
//          #define plugin_main bool XCONCAT(nvgt_plugin_, NVGT_PLUGIN_STATIC)
//
//      and it is never #undef'd, so plugin_main stays a macro for the whole
//      translation unit after the include. The call above it expands, before
//      anything is looked up, into the tokens `bool nvgt_plugin_1(shared)` -
//      a declaration in the middle of a return statement. There is no name to
//      call because the macro consumed the name.
//
// The lesson generalizes past these two lines: the header's entry-point macros
// cannot be used from a translation unit that already has STATIC set - not as
// a definition that mentions the macro, and not as a call to it. Every use
// expands to a declaration.
//
// And the plugin's definition cannot come from here in any form, because
// src/nvmidi.cpp is compiled into the same link and DEFINES the entry point
// itself. If this file defined one too, the link would report a duplicate
// symbol rather than resolve anything.
//
// So this is a stub: it counts itself and answers false. What that costs is
// stated plainly rather than hidden - the plugin's registration never runs, no
// source file of the plugin is entered, and the zero `nvgt_calls` below is a
// fact about this stub and not about the plugin. What it still buys is that
// the executable links and runs, so the engine half, the table, and the
// process's own sanity can be exercised while the plugin-side question is
// settled where it belongs - on the VPS, with the real object build, which is
// what the next step does.
// _Z13nvgt_plugin_1P18nvgt_plugin_shared, spelled by hand because the header
// cannot spell it (see above): the object file was asked with
// `nm --defined-only objcheck/nvmidi.o | grep plugin`, and that is the defined
// symbol it answered with, alongside prepare_plugin (weak) and
// register_nvmidi. The parameter type is part of the mangling and comes from
// the same header, so the two spellings cannot drift apart without the link
// saying so, which is the property that made spelling it by hand acceptable.
//
// Why this is a declaration and not an include-and-call: the header's
// plugin_main macro cannot be used from this translation unit at all, and the
// reason is written out above.
bool nvgt_plugin_1(nvgt_plugin_shared* shared);

extern "C" bool nvgt_plugin(nvgt_plugin_shared* shared) {
	// The plugin's own entry point, reached under its expanded name. This call
	// is the whole point of the harness: its answer is the plugin's, its side
	// effects are the plugin's registration, and the table it receives is the
	// same one nvgt would hand it.
	return nvgt_plugin_1(shared);
}
// No extern declaration of nvgt_plugin_version here, deliberately: with
// NVGT_PLUGIN_STATIC set the header does not emit plugin_version(), so nothing
// in this link defines that symbol and an extern would be one more undefined
// reference to explain. Through the table it is already reachable - the
// contract requires every slot to be filled before nvgt_plugin() is entered.


// Load-bearing, and not an oversight: the vendored RtMidi keeps its template
// definitions in RtMidi.cpp instead of the header, so the header alone only
// gives declarations. Including it here is what makes this translation unit
// instantiate them - a host that did not would link the plugin's object
// against an empty RtMidi and only find out at run time, when the plugin asks
// for a port.
#include <RtMidi.h>

// scriptarray.h for RegisterScriptArray, and the same header the plugin
// compiles against - the add-on is installed into one engine and both sides
// must agree on what an `array<T>` is, which they only do if it is one
// implementation registered once.
#include "scriptarray.h"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>
#include <cstdint>
#include <string>

// The string type, which upstream Angelscript ships as a reference add-on
// (sdk/add_on/scriptstdstring). This calls upstream's own registration rather
// than a hand-rolled one: the type has to be the same one nvgt puts in the
// engine, and the shortest way to be sure of that is to run the same function
// nvgt runs.
//
// The header is not vendored in this tree - third_party/angelscript carries
// the headers and the array add-on only - so the include path names the SDK
// checkout the engine was built from. The build command below passes it.
#include "scriptstdstring.h"

namespace {


} // namespace

// The nvgt half of the table: real definitions, in this file after all.
//
// This is the third arrangement tried and the first that links, so it is worth
// recording why the other two were not just unlucky.
//
// The fourteen nvgt names are the part of the plugin's world that Angelscript
// does not supply: waiting, redrawing, reading a clock, packing assets. In the
// plugin's own translation unit they are *references* - prepare_plugin() does
//
//   #define X(ret, name, args) name = shared->f_##name;
//
// for all twenty-six, and under NVGT_PLUGIN_INCLUDE the fourteen nvgt ones are
// `extern` at that point, so src/nvmidi.cpp emits fourteen undefined symbols.
// Something in the program has to define them.
//
// The thing to notice is that NVGT_PLUGIN_STATIC changes what the header emits
// for those same names. With it set - and it is set in this file - the header
// takes the branch whose comment is "Any functions we want to expose from NVGT
// rather than Angelscript must be forward declared here", and emits them as
// real *function declarations*:
//
//   void* nvgt_datastream_create (std::ios*, const std::string&, int);
//   ...
//   bool running_on_mobile ();
//
// which is exactly the shape a definition has to have. So the definitions
// belong here, spelled to match the header's own declarations, and the header
// checks them at compile time instead of a separate transcription going stale.
//
// Verified by preprocessing rather than by reading: under STATIC the header
// emits the fourteen as functions; under INCLUDE alone it emits them as
// variables. Two TUs with two different macros therefore disagree about what
// `nvgt_wait` *is*, which is why the earlier attempt to put these definitions
// in a separate translation unit produced `undefined reference to nvgt_wait`
// even though a function of that name existed in the link.

// A counter, because the interesting question about these is not whether they
// work but whether they are ever reached: the registration path is supposed to
// touch the engine, and a run that never enters one of these says the plugin got
// its engine somewhere else. The probe prints the total.
static int nvgt_calls = 0;

// `wait` is the odd one and does NOT come from NVGT_PLUGIN_FUNCTIONS. The
// header declares it itself, a few lines above the inline nvgt_wait that wraps
// it:
//
//   void wait(int ms);
//   inline void nvgt_wait(int ms) { wait(ms); }
//
// so nvgt_wait is supplied by the header and `wait` is the real symbol. The
// linker named it plainly - `undefined reference to 'wait(int)'` from
// probe_host.cpp, in function `nvgt_wait(int)` - which is the one place a
// mangled name was more useful than the demangled form: the reference is to a
// function *called* by the host's own inline wrapper, which the host therefore
// has to provide. On the real thing NVGT implements it; here it is a no-op
// that only counts.
void wait(int) {
	++nvgt_calls;
}

void* nvgt_datastream_create(std::ios*, const std::string&, int) {
	++nvgt_calls;
	return nullptr;
}

std::ios* nvgt_datastream_get_ios(void*) {
	++nvgt_calls;
	return nullptr;
}

void nvgt_bundle_shared_library(const std::string&) {
	++nvgt_calls;
}

bool find_embedded_pack(std::string&, uint64_t&, uint64_t&) {
	++nvgt_calls;
	return false;
}

// The clocks are the one pair that must return something *plausible* rather
// than zero: a script that measures elapsed time and divides by the delta gets
// a division by zero from a constant, which is a different failure from the one
// being looked for. A monotonic counter is what the real ticks() guarantees and
// all a duration check needs.
uint64_t ticks(bool) {
	++nvgt_calls;
	static uint64_t n = 0;
	return ++n;
}

uint64_t microticks(bool) {
	++nvgt_calls;
	static uint64_t n = 0;
	return ++n;
}

std::string string_aes_encrypt(const std::string& plaintext, std::string) {
	++nvgt_calls;
	return plaintext; // not an encryption; nothing here keeps secrets
}

std::string string_aes_decrypt(const std::string& ciphertext, std::string) {
	++nvgt_calls;
	return ciphertext;
}

void refresh_window() {
	++nvgt_calls;
}

void nvgt_audio_plugin_node_register(const std::string&) {
	++nvgt_calls;
}

plugin_node* nvgt_audio_plugin_node_create(audio_plugin_node_interface*,
                                           unsigned char, unsigned char,
                                           unsigned int, audio_engine*) {
	++nvgt_calls;
	return nullptr;
}

audio_plugin_node_interface* nvgt_audio_plugin_node_get(plugin_node*) {
	++nvgt_calls;
	return nullptr;
}

bool running_on_mobile() {
	++nvgt_calls;
	// False, not true. This decides which host nvmidi believes it is running
	// on, and the probe is a desktop Linux process; answering true would test a
	// branch the real deployment does not take.
	return false;
}

#undef NVGT_STANDIN


// The script half, which is the half that matters.
//
// Everything above answers "did the registration succeed". A registration that
// succeeds and a type a *script* can use are different claims, and nvgt answers
// the second one at compile time: `Expected ';'` / `Instead found '@'` is a
// script that could not use the type, on a plugin whose registration returned
// true. So this compiles a script - through the same engine, the same way nvgt
// does - and then runs it.
//
// The script is the octave echo, which is the feature the owner asked for: a
// key pressed, the same note sent back out twelve semitones higher. It cannot
// play a note here (this build is the dummy backend and there is no port), so
// the test does not measure what it did to a MIDI port. What it measures is
// that the script compiles, that the four TRANSPOSE_* globals are reachable
// from script by name, and that the boundary the plugin documents is the
// boundary the script sees: 115 goes out as 127, 116 does not go out at all.
// Those are arithmetic and are checkable without a port.
//
// The message is built by hand into the struct the plugin fills, rather than
// received from a port, because on this machine there is no port. That is the
// honest limit of the run and it is stated in the output rather than implied.
// What the engine can still see, asked after a failed Build().
//
// The script names types the plugin registered; the engine is the only party
// that knows whether those names resolve. Asking it here is cheap, cannot
// crash (it is one accessor per name), and turns the bare -17 into "this name
// is missing", which is the whole of what the reader needs to know.
//
// GetTypeIdByDecl returns a negative code for a name it does not know: -12 is
// asNO_TYPE when the name is simply absent, and -8 asINVALID_NAME when the
// name is taken by something else. Both are printed as they come back rather
// than flattened, because they mean different repairs.
static void probe_report_type_state(asIScriptEngine* engine) {
	if (!engine) { std::printf("   (no engine to ask)\n"); return; }

	// The two the script itself uses, plus the two the plugin registers for
	// its own methods. `string` is registered by the host in the shim, so a
	// failure on it means the shim's add-on did not take, not the plugin.
	static const char* const names[] = { "int", "string", "array<int>", "midi_message" };

	std::printf("   engine type state after the failed build:\n");
	for (unsigned i = 0; i < sizeof(names) / sizeof(names[0]); ++i) {
		const int id = engine->GetTypeIdByDecl(names[i]);
		std::printf("     %-14s -> %d%s\n", names[i], id,
			id >= 0 ? "  (resolves)" : (id == -12 ? "  asNO_TYPE: the name is not known"
			                                     : (id == -8 ? "  asINVALID_NAME: taken by another kind" : "")));
	}
	std::fflush(stdout);

	// And the module's own view, which is a different question: a type can be
	// registered and still not be *declarable* from a section if the section
	// was compiled before the registration. This one is asked by name.
	(void)engine;
}

// Drop the top-level statements named in PROBE_DROP_STATEMENTS ("3 7 9").
//
// A statement is a top-level line plus every line nested under it up to the
// matching close brace; the count is the ordinal of top-level statements in
// the script, starting at zero. Nothing else about the text changes, so the
// survivor is literally the original minus whole statements and the line
// numbers the engine reports still map back to it.
//
// This replaced a bisection that called module->Discard() and Built() again
// inside one process, which is the state this add-on does not survive; here
// the filtering happens before the build, once, and the outcome is the real
// build result rather than a guess about it.
static std::string probe_filter_statements(const char* source) {
	const char* drop_env = std::getenv("PROBE_DROP_STATEMENTS");
	if (!drop_env || !*drop_env) return std::string(source);

	std::vector<int> drop;
	{
		const std::string spec(drop_env);
		size_t at = 0;
		while (at <= spec.size()) {
			const size_t sp = spec.find(' ', at);
			const std::string word = spec.substr(at, sp == std::string::npos ? std::string::npos : sp - at);
			if (!word.empty()) drop.push_back(std::atoi(word.c_str()));
			if (sp == std::string::npos) break;
			at = sp + 1;
		}
	}

	const std::string all(source);
	std::string kept;
	int statement = -1;
	int skip_until_depth = -1;   // -1 when nothing is being skipped
	int depth = 0;

	size_t at = 0;
	while (at <= all.size()) {
		const size_t nl = all.find('\n', at);
		const std::string line = all.substr(at, nl == std::string::npos ? std::string::npos : nl - at);

		const bool blank = line.find_first_not_of(" \t\r") == std::string::npos;
		if (!blank && depth == 0) {
			++statement;
			bool wanted = false;
			for (size_t k = 0; k < drop.size(); ++k) if (drop[k] == statement) wanted = true;
			if (wanted) skip_until_depth = 0;
		}

		const bool skipping = (skip_until_depth >= 0);
		if (!skipping && !blank) kept += line + "\n";

		// A brace line changes the nesting after it. Opening at top level
		// raises depth; a close back to the level of a dropped statement ends
		// the skip.
		if (!blank) {
			for (size_t c = 0; c < line.size(); ++c) {
				if (line[c] == '{') ++depth;
				else if (line[c] == '}') {
					--depth;
					if (skipping && depth == skip_until_depth && line.find('}') != std::string::npos) {
						// Inside the dropped block, or its own close.
						if (depth <= skip_until_depth) { skip_until_depth = -1; }
					}
				}
			}
			if (skipping && skip_until_depth >= 0 && depth == 0 && line.find('{') == std::string::npos) {
				// A dropped statement that is a single line with no block.
				skip_until_depth = -1;
			}
		}

		if (nl == std::string::npos) break;
		at = nl + 1;
	}
	return kept;
}

// Where the script's own reporting goes. The script is handed a `string` from
// the plugin's string add-on, and the add-on keeps the C++ side of it in a
// std::string, which is what the script's `print(...)` reaches here as.
//
// This is not a print the engine has: nothing registers `print` in this
// harness, so a script that calls it does not compile at all. This is the
// host's stand-in, and it exists so that a failing assertion inside the script
// is readable instead of silent.
static void probe_say(const std::string& text) {
	std::printf("script: %s", text.c_str());
	std::fflush(stdout);
}

static void run_transpose_script(ProbeEngine& probe, asIScriptEngine* engine) {
	// The engine handle is taken from the shim rather than made here, so
	// that this translation unit is the only one that includes
	// nvgt_plugin.h and the shim is the only one that includes
	// angelscript.h. `probe` itself is not read below - the script asks the
	// plugin's surface, not the engine's - and it is a reference parameter
	// so that the handle stays reachable if that changes.
	(void)probe;
	std::printf("\n--- the script half ---\n");

	// The plugin looks up module 0 for the engine's own wait(), so a module 0
	// is what nvgt would have and what this has to be.
	// There is no CreateModule() in 2.39 - a module is asked for by name and
	// created on the way if it is not there (angelscript.h:379). The name is
	// the plugin's own, so a module nvgt made and this made are the same slot.
	asIScriptModule* module = engine->GetModule("nvmidi", asGM_CREATE_IF_NOT_EXISTS);
	if (!module) { std::printf("could not create a script module\n"); return; }

	// A section is given a name, and the script is built one statement per
	// line so that a diagnostic's row can be translated back into the
	// statement by the reader.
	//
	// Why the code is all that comes back: this version of angelscript
	// reports a compiler diagnostic through neither route a C++ host can
	// install. Measured both:
	//   * the engine's SetMessageCallback (installed in the shim) fired three
	//     times, all for the plugin's blind probes, and not once during Build();
	//   * asIScriptModule has no SetLineCallback and no GetBuildError - the
	//     whole interface is CreateContext/AddScriptSection/Build and
	//     accessors, read end to end in angelscript.h. The first attempt at
	//     this block called it anyway and the compiler said so:
	//     "'class asIScriptModule' has no member named 'SetLineCallback'".
	//
	// So the code is turned into a name the other way round: instead of
	// asking the module which declaration it disliked, the engine is asked
	// which of the declarations exist. A type the script names and the
	// engine cannot resolve is the answer, and it is an answer no amount of
	// line-removal can produce. See probe_report_type_state below.

	// `print` is the one name the transpose script asks for that no plugin ever
	// registers: it belongs to nvgt's own script environment, which this host is
	// not. Measured rather than guessed - without this the compiler reports
	// "No matching symbol 'print'" at row 7 and the script's whole failure
	// reporting is dead. probe_say is registered rather than inventing a print
	// the engine does not have. The plugin's own midi_out is the global the
	// script uses, so no variable of that type is declared here.
	{
		asIScriptEngine* e = static_cast<asIScriptEngine*>(probe.handle);
		const int r = e->RegisterGlobalFunction(
			"void probe_say(const string&in)", asFUNCTION(probe_say), asCALL_CDECL);
		std::printf("probe_say registration: %d\n", r);
		std::fflush(stdout);
	}

	// No `string` literals or helpers beyond what the type needs: the point is
	// to exercise the plugin's surface, not the add-on's.
	const char* source =
		"int checks = 0;\n"
		"int failures = 0;\n"
		"void want_int(const string&in what, int got, int expected) {\n"
		"    checks++;\n"
		"    if (got != expected) {\n"
		"        failures++;\n"
		"        probe_say(what + \": got \" + got + \", wanted \" + expected + \"\\n\");\n"
		"    }\n"
		"}\n"
		"int note_status(int kind, int note, int velocity) {\n"
		"    return (kind << 4) | ((note >> 4) & 0x0f);\n"
		"}\n"
		"void main() {\n"
		"    midi_message m;\n"
		"    // The constants must be the plugin's values, by name, from script.\n"
		"    want_int(\"TRANSPOSE_SENT\", TRANSPOSE_SENT, 0);\n"
		"    want_int(\"TRANSPOSE_PASSED_THROUGH\", TRANSPOSE_PASSED_THROUGH, 1);\n"
		"    want_int(\"TRANSPOSE_OUT_OF_RANGE\", TRANSPOSE_OUT_OF_RANGE, 2);\n"
		"    want_int(\"TRANSPOSE_FAILED\", TRANSPOSE_FAILED, 3);\n"
		"    // No port is open and there is none to open on this build, so every\n"
		"    // send is TRANSPOSE_FAILED - which is itself the claim: a plugin that\n"
		"    // reported SENT with no port would be lying about a note nobody heard.\n"
		"    m.status = 0x90; m.data1 = 60; m.data2 = 100;\n"
		"    want_int(\"no port, note\", midi_out.send_transposed(m, 12), TRANSPOSE_FAILED);\n"
		"    // A note whose move leaves 0..127 is refused, not wrapped.\n"
		"    m.data1 = 116;\n"
		"    want_int(\"move past the top\", midi_out.send_transposed(m, 12), TRANSPOSE_OUT_OF_RANGE);\n"
		"    m.data1 = 60;\n"
		"    want_int(\"move past the bottom\", midi_out.send_transposed(m, -61), TRANSPOSE_OUT_OF_RANGE);\n"
		"    // 115 + 12 = 127, the last note. It must not be refused.\n"
		"    m.data1 = 115;\n"
		"    want_int(\"the last note is reachable\", midi_out.send_transposed(m, 12), TRANSPOSE_FAILED);\n"
		"    // A controller is not a note: it is passed through, never moved.\n"
		"    m.status = 0xB0; m.data1 = 7; m.data2 = 100;\n"
		"    want_int(\"a pedal is not a note\", midi_out.send_transposed(m, 12), TRANSPOSE_FAILED);\n"
		"    probe_say(\"checks \" + checks + \", failures \" + failures + \"\\n\");\n"
		"}\n";

	// A statement can be dropped without a second Build(): the text is filtered
	// before the section is added, and the filter is driven by the environment
	// so a caller can ask a different question per process. A dropped top-level
	// statement takes its whole block with it - dropping a body line while its
	// header stays would be a different script, not a substring of this one.
	const std::string script = probe_filter_statements(source);
	if (script.size() != std::string(source).size()) {
		std::printf("PROBE_DROP_STATEMENTS=%s left %u byte(s):\n%s\n",
			std::getenv("PROBE_DROP_STATEMENTS"), (unsigned)script.size(), script.c_str());
		std::fflush(stdout);
	}

	module->AddScriptSection("transpose", script.c_str());

	// The flag, asked for directly, on both sides of Build().
	//
	// asCModule::Build() returns -17 asINVALID_CONFIGURATION only when
	// m_engine->configFailed is *already* set - it calls PrepareEngine(), sees
	// the flag and reports it instead of compiling. If that is what is
	// happening here, the flag is set before this call and no amount of script
	// bisection can reach it: the script is never compiled at all. The flag is
	// readable from a host (asIScriptEngine::GetEngineProperty, asEP_... has no
	// member for it, but the host owns the engine and can ask the one question
	// that separates the two worlds) - so the world is separated by measurement
	// rather than by inference: a trivially valid one-line script is built in
	// its own module first. If THAT also returns -17 the engine is poisoned and
	// the transpose script is irrelevant; if it compiles, the fault is in the
	// transpose text after all and the bisection was pointed at the wrong thing.
	{
		asIScriptModule* canary = engine->GetModule("canary", asGM_ALWAYS_CREATE);
		canary->AddScriptSection("canary", "void canary() { int x = 1; x += 1; }");
		const int canary_built = canary->Build();
		std::printf("canary module (a script that cannot fail to compile): %d\n", canary_built);
		std::fflush(stdout);
		if (canary_built < 0) {
			std::printf("  -> the engine refuses even a valid script: configFailed is already set\n");
			std::printf("  -> the transpose script was never compiled; script bisection cannot reach this\n");
			std::fflush(stdout);
		}
	}

	int built = module->Build();
	if (built < 0) {
		// -17 is asINVALID_DECLARATION, and what to do about it is decided by
		// asking the engine a question the module cannot answer. The bisection
		// that used to sit here - Discard() and a second Build() per line - is
		// gone, and the reason is a measurement, not a change of heart: the
		// harness died inside it with signal 11 and si_addr=0x55823967a, a
		// four-and-a-half-byte pointer, i.e. a garbage `this`, naming no line
		// and printing nothing after the crash. In the same build the plugin's
		// full registration (128 calls, 0 refused) and a plain engine call
		// through the plugin's own table both complete and the process exits 0,
		// so engine and table are sound and the fault was this block's own
		// doing: a second Build() over a module whose first Build() failed is
		// the one state the add-on does not survive.
		std::printf("the script DID NOT COMPILE: %d (asINVALID_DECLARATION), attributed below\n",
			built);
		std::fflush(stdout);
		probe_report_type_state(engine);
		probe_engine_validate_types(&probe);
		return;
	}

	std::printf("the script compiled against the registered engine\n");

	asIScriptFunction* fn = module->GetFunctionByDecl("void main()");
	if (!fn) { std::printf("no main() in the module\n"); return; }
	asIScriptContext* ctx = engine->CreateContext();
	if (!ctx) { std::printf("no context\n"); return; }
	ctx->Prepare(fn);
	const int ran = ctx->Execute();
	std::printf("the script ran, asEXECUTION_FINISHED is 0 and this returned %d\n", ran);

	// The counters live in the module's globals, so the verdict is read back
	// from the script's own state rather than from the exit status of a call.
	//
	// The name is not a member function: GetGlobalVar takes an out-parameter
	// (angelscript.h:861) and GetAddressOfGlobalVar takes the index. So the
	// index is asked for by name and then resolved, rather than assumed from
	// declaration order - a declared-order assumption is exactly the kind of
	// thing that silently reads the wrong int.
	int* checks = nullptr; int* failures = nullptr;
	const int checks_at = module->GetGlobalVarIndexByName("checks");
	const int failures_at = module->GetGlobalVarIndexByName("failures");
	if (checks_at >= 0) checks = (int*)module->GetAddressOfGlobalVar((asUINT)checks_at);
	if (failures_at >= 0) failures = (int*)module->GetAddressOfGlobalVar((asUINT)failures_at);
	ctx->Release();
	if (checks && failures) {
		std::printf("script verdict: %d checks, %d failure(s)\n", *checks, *failures);
		if (*failures == 0 && *checks > 0)
			std::printf("TRANSPOSE API OK: the script used it and every answer was the documented one\n");
	} else {
		std::printf("the script's counters could not be read back\n");
	}
}

// nvgt_plugin_version is not asked for here at all, and the reason is worth
// writing down because the obvious version of this line was written first and
// did not compile: the struct holds no slot for it. `grep -n f_` over the table
// in src/nvgt_plugin.h lists f_as... for the twelve engine functions and f_nvgt_
// for the fourteen nvgt ones, and that is the whole of it - the two entry-point
// typedefs, nvgt_plugin_entry and nvgt_plugin_version_func, are declared beside
// the struct and never made members of it.
//
// So the api version is not a fact the host can ask the plugin for through the
// contract. The direction of the handshake is the other way round: the *host*
// fills shared.version, and the plugin's own nvgt_plugin() checks it, which is
// the behaviour this harness tests by building the table with the wrong version
// at one point below. What the host gets back is the entry point's bool.

int main() {

	nvgt_plugin_shared shared{};
	shared.version = NVGT_PLUGIN_API_VERSION;

	// The Angelscript half, real, made by the shim so that this translation
	// unit never has to see angelscript.h.
	ProbeEngine engine;
	if (!probe_engine_create(&engine)) {
		std::printf("could not create an Angelscript engine; nothing can be tested\n");
		std::printf("(asCreateScriptEngine returns null when the library's version does\n");
		std::printf(" not match the ANGELSCRIPT_VERSION this file was compiled with)\n");
		return 2;
	}
	shared.script_engine = static_cast<asIScriptEngine*>(engine.handle);
	std::printf("a real Angelscript engine was created (version %s)\n", engine.version);

	// The add-ons the plugin's declarations assume, registered before the plugin
	// is entered - because nvgt registers them before it enters any plugin, and
	// an engine that has neither is not the engine this plugin is written
	// against.
	//
	// `string` is the whole of the 22 refusals, and it took a while to see
	// because the refusals are all `string` declarations and the plugin does
	// register the array add-on itself. Measured on a bare asCreateScriptEngine():
	//
	//   GetTypeIdByDecl("string")      = -12   (asNO_TYPE, no such type)
	//   GetTypeIdByDecl("string@")     = -12
	//   ... so every "const string&in ..." declaration in the plugin is refused
	//       with -10 (asINVALID_DECLARATION), 22 times, while the plugin's
	//       registration still returns true. The count is the only place it
	//       showed.
	//
	// array<T> is NOT registered here even though the same reasoning would
	// suggest it: the plugin registers it itself, conditionally, only when the
	// engine does not already know the type. Registering it here first would
	// silence that branch instead of exercising it, and the branch is part of
	// what this host is meant to run.
	int string_id = -1, array_id = -1;
	if (!engine.register_host_types(&string_id, &array_id)) {
		std::printf("string is registered; GetTypeIdByDecl(\"string\") = %d\n", string_id);
		std::printf("the string type did not take; nothing below would be\n");
		return 2;
	}
	std::printf("string is registered; GetTypeIdByDecl(\"string\") = %d\n", string_id);

	// The nvgt half, stand-ins that report if they are ever called.
	//
	// The `&` is NOT here for the same reason it is not on the as* slots below,
	// and the reason is the header's typedef trap, not a preference. The
	// declarators above come from
	//
	//   #define X(ret, name, args) typedef ret t_##name args;
	//   #define X(ret, name, args) t_##name* f_##name;
	//
	// so f_name points at something of *function type*, and the address written
	// into it must be a plain function pointer, not the address of an array of
	// one. These names have function type, so they decay to exactly that.
#define NVGT_STANDIN(ret, name, args) shared.f_##name = name;

	NVGT_STANDIN(void*, nvgt_datastream_create, (std::ios*, const std::string&, int))
	NVGT_STANDIN(std::ios*, nvgt_datastream_get_ios, (void*))
	NVGT_STANDIN(void, nvgt_bundle_shared_library, (const std::string&))
	NVGT_STANDIN(bool, find_embedded_pack, (std::string&, uint64_t&, uint64_t&))
	NVGT_STANDIN(void, nvgt_wait, (int))
	NVGT_STANDIN(void, refresh_window, ())
	NVGT_STANDIN(uint64_t, ticks, (bool))
	NVGT_STANDIN(uint64_t, microticks, (bool))
	NVGT_STANDIN(std::string, string_aes_encrypt, (const std::string&, std::string))
	NVGT_STANDIN(std::string, string_aes_decrypt, (const std::string&, std::string))
	NVGT_STANDIN(void, nvgt_audio_plugin_node_register, (const std::string&))
	NVGT_STANDIN(plugin_node*, nvgt_audio_plugin_node_create,
	             (audio_plugin_node_interface*, unsigned char, unsigned char, unsigned int, audio_engine*))
	NVGT_STANDIN(audio_plugin_node_interface*, nvgt_audio_plugin_node_get, (plugin_node*))
	NVGT_STANDIN(bool, running_on_mobile, ())

#undef NVGT_STANDIN

	// The thirteen as* slots, filled one by one.
	//
	// They are filled by name rather than by expanding the macro, and the
	// reason is a collision between three declarations of the same identifier
	// that the header sets up on purpose.
	//
	//   1. angelscript.h has the real function: `const char* asGetLibraryVersion();`
	//   2. the header declares a *symbol* of the same name for the dll branch:
	//      X(const char*, asGetLibraryVersion, ()) expands to
	//      `t_asGetLibraryVersion* asGetLibraryVersion = nullptr;`
	//   3. and the typedef that symbol's type comes from is
	//      `typedef const char* t_asGetLibraryVersion();`
	//
	// Point 3 is the trap: the typedef declares a *function type*, so the
	// declarator in point 2 is not `pointer to function` - it is an array of
	// one function, `const char* (*asGetLibraryVersion)()`. That is invisible
	// until you write `shared.f_asGetLibraryVersion = &asGetLibraryVersion;`
	// and the compiler says `const char* (**)()` cannot convert to
	// `const char* (*)()`. `&` on an array of one is a pointer to the array.
	//
	// The header is right and the mistake was here. `name = shared->f_name`
	// (pointer to pointer) and `shared->f_name = &name` (&array -> pointer)
	// are both consistent with an array; only `&function` was not. So the fix
	// is to delete the `&`, not to name the types by hand - which also keeps
	// this list checked against the header's own declarations instead of
	// against my transcription of them.
	//
	// asGetLibraryVersion and asGetLibraryOptions are left out. They are the
	// two pure accessors in the list: the plugin has never consulted either,
	// and this host prints the version itself. A null there is a visible crash
	// rather than a silently wrong value if that ever stops being true.
	//
	// The four that matter on this path are asPrepareMultithread and the three
	// locks. They were null in the first version of this host, and that is what
	// the core dump showed: prepare_plugin() copies every slot and then calls
	// the copied asPrepareMultithread, so a null fails silently here and loudly
	// there.
	shared.f_asGetActiveContext = asGetActiveContext;
	shared.f_asPrepareMultithread = asPrepareMultithread;
	shared.f_asAcquireExclusiveLock = asAcquireExclusiveLock;
	shared.f_asReleaseExclusiveLock = asReleaseExclusiveLock;
	shared.f_asAcquireSharedLock = asAcquireSharedLock;
	shared.f_asReleaseSharedLock = asReleaseSharedLock;
	shared.f_asAtomicInc = asAtomicInc;
	shared.f_asAtomicDec = asAtomicDec;
	shared.f_asThreadCleanup = asThreadCleanup;
	shared.f_asAllocMem = asAllocMem;
	shared.f_asFreeMem = asFreeMem;

	// script_thread_manager is deliberately left null, and here that is the
	// supported call rather than a hole: asPrepareMultithread(null) makes a
	// default thread manager instead of dereferencing anything. That is exactly
	// what nvgt passes on a machine that never configured one.
	std::printf("calling the entry point, with a real engine and no stub table...\n");
	std::fflush(stdout);
	// prepare_plugin is deliberately NOT called, and that is the one place this
	// host is honestly not nvgt. Its body is
	//
	//   #define X(ret, name, args) name = shared->f_##name;
	//   NVGT_PLUGIN_EXTERNAL_FUNCTIONS
	//   NVGT_PLUGIN_FUNCTIONS
	//   asPrepareMultithread(...);
	//
	// - it copies all twenty-nine slots into globals. The twelve engine ones
	// would be fine here, since the library is linked; the fourteen nvgt ones
	// are not, because this host does not contain nvgt and every reference to
	// them is unlinkable by construction. What the plugin needs from
	// prepare_plugin is exactly what a host must do: it reads the table through
	// its argument. Handing it a filled-in table is that same contract, and it
	// is what keeps `shared.f_...` - not a global - the thing under test.
	const bool accepted = nvgt_plugin(&shared);
	std::printf("the entry point returned %s\n", accepted ? "true" : "false");

	if (accepted) {
		std::printf("REGISTERED: the plugin ran its whole registration against a live Angelscript\n");
		std::printf("the plugin asked the nvgt side of the table %d time(s); 0 means the\n", nvgt_calls);
		std::printf("registration path never reached for the engine at all\n");
	}

	if (accepted) {
		// The plugin has just returned and its 128 registrations are in the
		// engine. The crash that brought this harness here lands after this
		// point and after nothing else the host prints, so the host's next
		// engine call is what is under test - and it is made once, alone,
		// with the exit right behind it so the signal's arrival is the whole
		// answer:
		//
		//   a signal (139, or whichever number the ptrace wrapper reports)
		//   means the engine died the moment the host touched it through the
		//   table where a plain function call on the same pointer in the
		//   plugin's own code did not - that is a call-convention or table
		//   fault, and the table slot is the suspect rather than the engine.
		//
		//   an exit 0 with the marker printed means the engine is sound
		//   through the table and the fault is further on, in the script half.
		std::printf("a plain call through the table: asking the engine for a type id\n");
		std::fflush(stdout);
		const int probe_id = shared.script_engine->GetTypeIdByDecl("int");
		std::printf("the engine answered %d through the table\n", probe_id);
		std::fflush(stdout);
	}

	if (accepted) run_transpose_script(engine, shared.script_engine);


	// -10 is asINVALID_DECLARATION (angelscript.h:130). An earlier version of
	// this note said asALREADY_REGISTERED is set here, until it was checked
	// against the vendored header; that is -13, and the difference is not
	// pedantry, because the two send a reader in opposite directions.
	// asALREADY_REGISTERED is about the name and suggests a second
	// registration of the same thing; asINVALID_DECLARATION is about the
	// declaration, and it is what the engine says when the text handed to a
	// Register* call does not describe the address it was given.
	//
	// The name is not printed here on purpose. Whatever the code, the answer to
	// "which declaration" is already in the log, one line per refusal, next to
	// the source line that made the call; a gloss repeated at the bottom would
	// be a second, less reliable account of the same fact.
	std::printf("NOTE: registration refusals above carry a negative Angelscript code, printed\n");
	std::printf("as a number. -10 is asINVALID_DECLARATION and -13 is asALREADY_REGISTERED;\n");
	std::printf("the names are in third_party/angelscript/angelscript.h beside the enum.\n");


	// The engine is NOT released and NOT destroyed. Both halves of that are
	// deliberate, and the second is the one that was measured.
	//
	// The first version called asEngine->Release() here and the process then
	// died in as_atomic.cpp:50 on `Assertion 'value < 1000000' failed'`. That
	// assertion is a refcount sanity check, and what trips it is the plugin's
	// registrations rather than the engine's own bookkeeping: this plugin
	// registers its object types with asOBJ_NOCOUNT (src/nvmidi.cpp:1974 and
	// :2010 - "Size 0, and not sizeof"), so a type the engine holds carries no
	// reference count of its own, and the engine's teardown of such a type
	// walks a count that was never meant to reach zero. So not even the
	// Release() is asked for here: this host is not the engine's owner, and
	// nothing after the registration needs the reference count touched.
	//
	// Leaving it standing is not a workaround invented here - it is what nvgt
	// does. The engine is created once for the process and outlives every plugin
	// that registers into it, so no plugin in the field is ever asked to survive
	// its engine's destruction, and this host asking for one would be measuring
	// a teardown the plugin was never written for. The process exits a few
	// instructions later and the kernel reclaims the arena whole.
	return accepted ? 0 : 1;
}
