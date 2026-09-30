// probe_flag_arm.cpp - is the three-probe arm in register_midi_array() the
// thing that poisons the engine?
//
// The chain that leads here, each link measured rather than reasoned:
//
//   module->Build() returns -17 (asINVALID_CONFIGURATION) for EVERY script,
//   including a one-line `void canary() { int x = 1; x += 1; }` that cannot
//   fail to compile. That is the asCModule::Build path reading
//   m_engine->configFailed and reporting the flag instead of compiling.
//
//   configFailed is set in exactly one place for this engine:
//   asCScriptEngine::ConfigError(), reached from the Register* methods when
//   one of them refuses an argument outright.
//
//   The engine's own validation loop over registeredObjTypes is therefore not
//   the cause: walking that same list by hand reports 8 types, 3 subject to a
//   rule, 0 refused.
//
//   With 133 registration calls and 0 refusals as counted by the plugin's own
//   wrapper, the candidate has to be a call the wrapper does NOT count through
//   the plugin's table - and register_midi_array() has one: three direct
//   engine->RegisterObjectType calls used as a blindness probe, each with the
//   bare flags asOBJ_REF.
//
//   asOBJ_REF | asOBJ_SCRIPT_OBJECT is bit 0 | bit 21. The engine's
//   asOBJ_MASK_VALID_FLAGS is 0x1801FFFFF: bits 0..20 plus bits 27 and 28.
//   Bit 21 is NOT in it.
//
// So this file asks the one question that settles it, with no plugin, no
// probing and no interpretation: register a type with those exact flags and
// then try to build a script that cannot fail. If the build returns -17 the
// link is proven, because there is nothing else in this program.
//
// Nothing here is shipped - this is a diagnostic in tools/, and it exists so
// that the claim above can be re-measured by anyone who doubts it.

#include <angelscript.h>

#include <cstdio>

static void say(const char* text, int value) {
	std::printf("%s = %d\n", text, value);
	std::fflush(stdout);
}

int main() {
	asIScriptEngine* engine = asCreateScriptEngine();
	if (!engine) { std::printf("no engine\n"); return 1; }

	// 1. A clean engine builds the canary. If this fails, everything below is
	//    meaningless and the harness says so instead of pretending.
	{
		asIScriptModule* m = engine->GetModule("before", asGM_ALWAYS_CREATE);
		m->AddScriptSection("before", "void canary() { int x = 1; x += 1; }");
		say("before any registration, Build", m->Build());
	}

	// 2. The suspect call, verbatim: the same flags the probe arm uses.
	const int refused = engine->RegisterObjectType("probe_script_object", 0,
		asOBJ_REF | asOBJ_SCRIPT_OBJECT);
	say("RegisterObjectType(\"probe\", 0, asOBJ_REF | asOBJ_SCRIPT_OBJECT)", refused);
	std::printf("  (asINVALID_ARG is -3, asALREADY_REGISTERED is -13)\n");

	// 3. The same canary again, byte for byte, in a module of its own.
	{
		asIScriptModule* m = engine->GetModule("after", asGM_ALWAYS_CREATE);
		m->AddScriptSection("after", "void canary() { int x = 1; x += 1; }");
		say("after that one call, Build of the same script", m->Build());
	}

	// 4. And the control: a registration the engine accepts, to show the
	//    engine is alive and the fault is the flags and not the build path.
	{
		const int ok = engine->RegisterObjectType("probe_plain", 0, asOBJ_REF | asOBJ_NOCOUNT);
		say("RegisterObjectType(\"probe_plain\", 0, asOBJ_REF | asOBJ_NOCOUNT)", ok);
		asIScriptModule* m = engine->GetModule("control", asGM_ALWAYS_CREATE);
		m->AddScriptSection("control", "void canary() { int x = 1; x += 1; }");
		say("Build after an ACCEPTED registration (flag is already set, so this stays -17)", m->Build());
	}

	engine->Release();
	return 0;
}
