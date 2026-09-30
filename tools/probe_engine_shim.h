// probe_engine_shim.h - the two things a host must do to a plugin's engine,
// kept in a translation unit that never sees nvgt_plugin.h.
//
// Why this is a separate file
// ---------------------------
//
// src/nvgt_plugin.h and the real angelscript.h cannot be included together in
// one translation unit. The header declares, for the dll branch,
//
//   X(const char*, asGetLibraryVersion, ())
//     -> t_asGetLibraryVersion* asGetLibraryVersion = nullptr;
//
// which is a *variable* named asGetLibraryVersion, while angelscript.h has a
// *function* of the same name. The compiler reports it as
//
//   error: 'const char* (* asGetLibraryVersion)()' redeclared as
//          different kind of entity
//
// and no include order fixes it: order only decides which declaration the
// compiler has already seen when it objects. NVGT_PLUGIN_INCLUDE changes the
// variable into `extern t_<name>* <name>;`, and an extern variable collides
// with the function just as a definition does - measured, not assumed.
//
// The header is written for a plugin (which reaches the engine through the
// table and never includes angelscript.h itself) or for an addon that does not
// pull in the engine. A host needs both halves, so it takes them separately:
// this file owns the engine, probe_host.cpp owns the header, and the two meet
// at an opaque pointer. The coupling is one function call wide and is declared
// below.
//
// What has to happen before the plugin is entered
// -----------------------------------------------
//
// The plugin registers array<T> itself - src/nvmidi.cpp calls
// RegisterScriptArray when the engine does not already know the type - but it
// never registers string. It cannot: a string is the host's type, with the
// host's allocator behind it, and a plugin that defined its own would hand the
// engine a type nvgt's own scripts do not share.
//
// Measured on a bare asCreateScriptEngine(), which is the state this host was
// in before the shim: GetTypeIdByDecl("string") returns -12 (asNO_TYPE), and
// every "const string&in ..." the plugin declares is then refused with -10
// (asINVALID_DECLARATION) - 22 of them. The plugin's registration still
// returned true, so the count is the only place it showed. Registering the
// string add-on here takes it to 0.
#pragma once

// An asIScriptEngine* with the header out of the way. Not a forward
// declaration of the class: writing `struct asIScriptEngine;` here would be a
// second opinion about a class the engine has already laid out, and the shim
// is compiled against the real definition in probe_engine_shim.cpp.
struct ProbeEngineOpaque;

struct ProbeEngine {
	void*   handle = nullptr;   // asIScriptEngine*, owned by probe_host
	const char* version = "?";  // asGetLibraryVersion(), owned by the library

	// Registers the host's add-ons and reports what it can see afterwards, so a
	// registration that was accepted but did not take is caught here rather
	// than 22 refusals further down. Returns false if either type is missing.
	bool register_host_types(int* out_string_type_id, int* out_array_type_id);
};

// Fills *engine from a fresh asCreateScriptEngine(). Returns false when the
// engine could not be made at all - which also happens when the caller's
// ANGELSCRIPT_VERSION does not match the library's, so a false here is worth
// reading as "the library and the headers disagree" as much as "no memory".
bool probe_engine_create(ProbeEngine* engine);

// Walk the engine's registered object types and name the one that fails the
// engine's own validation, for the reason the engine itself would give.
//
// Why this exists at all: module->Build() returns -17 asINVALID_CONFIGURATION
// and no more than that. Reading the engine's source (source/as_module.cpp
// ~1700) shows where -17 comes from - m_engine->PrepareEngine() sets
// configFailed, and Build() reports the flag rather than the cause. The cause
// is set in as_scriptengine.cpp's validation loop, which walks exactly the
// list asIScriptEngine::GetObjectTypeCount() exposes and flags a type whose
// registered behaviours are incomplete for its flags. That loop prints its own
// reason through WriteMessage - except that this engine's message callback for
// it does not reach a host that installed one before the types were
// registered, which is the whole reason this second walk exists. The rules are
// the same six the engine applies:
//
//   asOBJ_GC + asOBJ_REF   -> addref, release and all gc behaviours
//   asOBJ_GC + value       -> the gc enum-references behaviour
//   asOBJ_SCOPED           -> release
//   asOBJ_REF (not scoped, not nohandle, not nocount) -> addref and release
//   asOBJ_VALUE (not pod)  -> at least one constructor and the destructor
//
// Returns the number of offending types, 0 when the list is clean.
int probe_engine_validate_types(ProbeEngine* engine);
