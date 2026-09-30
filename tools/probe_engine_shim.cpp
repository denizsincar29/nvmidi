// probe_engine_shim.cpp - the engine side of probe_host, kept away from
// nvgt_plugin.h. See probe_engine_shim.h for why the split exists.
//
// Nothing in this file may include nvgt_plugin.h. That is the whole point of
// it: angelscript.h and that header cannot be in one translation unit.

#include "probe_engine_shim.h"

#include <angelscript.h>

#include <cstdio>

// The string add-on, upstream's own (sdk/add_on/scriptstdstring). This calls
// the function nvgt itself calls rather than a hand-rolled registration: the
// type has to be the same one the engine would otherwise be given, and the
// shortest way to be sure of that is to run the same code.
#include "scriptstdstring.h"

// array<T> is registered by the plugin, not here - but the host asks the engine
// about it afterwards, and for that question the type id must be the same one
// the plugin's registration produced. RegisterScriptArray is not called from
// this file; the include is here only because the add-on's own header is the
// one the id convention belongs to.
#include "scriptarray.h"

// Where a build error's text goes.
//
// This version of angelscript has no asIScriptModule::GetBuildError - measured,
// not assumed: `error: 'class asIScriptModule' has no member named
// 'GetBuildError'` at probe_host.cpp:499. The only route to the messages is
// this callback, which the engine calls once per diagnostic while it compiles,
// so it has to be installed before Build() or the diagnostics are dropped on
// the floor and the caller is left with the bare code.
//
// What that costs without it, measured on this harness: module->Build()
// returned -17 and nothing else - no name, no line - five runs in a row. -17 is
// asINVALID_DECLARATION, which says the declaration is bad and not which one.
static void probe_message_callback(const asSMessageInfo* msg, void* param) {
	(void)param;
	const char* kind = "info";
	switch (msg->type) {
		case asMSGTYPE_ERROR:   kind = "error";   break;
		case asMSGTYPE_WARNING: kind = "warning"; break;
		default: break;
	}
	std::fprintf(stderr, "angelscript %s: %s (row %d, col %d, section \"%s\")\n",
		kind, msg->message, msg->row, msg->col, msg->section ? msg->section : "");
	std::fflush(stderr);
}

bool probe_engine_create(ProbeEngine* engine) {
	asIScriptEngine* made = asCreateScriptEngine();
	if (!made) return false;
	made->SetMessageCallback(asFUNCTION(probe_message_callback), nullptr, asCALL_CDECL);
	engine->handle = made;
	engine->version = asGetLibraryVersion();
	return true;
}

bool ProbeEngine::register_host_types(int* out_string_type_id, int* out_array_type_id) {
	asIScriptEngine* engine = static_cast<asIScriptEngine*>(handle);
	if (!engine) return false;

	RegisterStdString(engine);

	// Asked back immediately. -12 is asNO_TYPE: the registration was accepted
	// and the type is still not there - a different fault from a refusal, and
	// one that would otherwise surface as the same wall of -10s this shim
	// exists to remove.
	const int string_id = engine->GetTypeIdByDecl("string");
	if (out_string_type_id) *out_string_type_id = string_id;

	// array<T> is deliberately NOT registered here. The plugin does it, and it
	// does it conditionally: it asks whether the engine already knows the type
	// and only calls RegisterScriptArray when it does not. Registering it here
	// first would silence that branch rather than exercise it - and the branch
	// is part of what this host is meant to run. So the id is read back after
	// the plugin has been entered, which is what probe_host.cpp does.
	if (out_array_type_id) *out_array_type_id = -1;

	return string_id >= 0;
}

// Which registered type FAILS the engine's validation, in the engine's own
// terms, with every field the rules read printed whether or not it is the
// reason. The previous version of this walk reported offenders as a count and
// a sentence, and it reported zero - which was true and useless: the flag is
// already set by the time this code can run, so the question is no longer
// "does anything fail" but "which type did the engine look at, and what did
// each of the five rules see when it did".
//
// So every field is printed. The rules are read off the type's flags, and a
// type whose flags make it subject to a rule is marked with the fields that
// rule consults. A rule that would refuse is printed as REFUSED and the walk
// keeps going, because the engine's loop keeps going too and there may be more
// than one.
//
// What is deliberately NOT trusted here: the engine's own message callback.
// It printed "" for section and row 0 col 0, and the type name never arrived.
// The reason is that ConfigError() writes through a path that does not reach a
// callback installed before the offending call - so this walk is the
// instrument, and the engine's message is treated as a second opinion that
// happened not to show up.
int probe_engine_validate_types(ProbeEngine* engine) {
	asIScriptEngine* e = static_cast<asIScriptEngine*>(engine->handle);
	if (!e) return -1;

	const asUINT count = e->GetObjectTypeCount();
	int offenders = 0;
	int subject = 0;   // types whose flags make them subject to at least one rule

	std::fprintf(stderr, "probe: %u registered object type(s); fields below are what the engine's rules read\n",
		(unsigned)count);

	for (asUINT i = 0; i < count; ++i) {
		asITypeInfo* type = e->GetObjectTypeByIndex(i);
		if (!type) continue;
		if (type->GetFlags() & asOBJ_SCRIPT_OBJECT) continue;

		const asQWORD flags = type->GetFlags();
		bool hasAddRef = false, hasRelease = false, hasConstruct = false, hasDestruct = false;
		bool hasGcGetRefCount = false, hasGcSetFlag = false, hasGcGetFlag = false;
		bool hasGcEnumRefs = false, hasGcReleaseAll = false;

		const asUINT beh_count = type->GetBehaviourCount();
		for (asUINT b = 0; b < beh_count; ++b) {
			asEBehaviours beh = asBEHAVE_CONSTRUCT;
			type->GetBehaviourByIndex(b, &beh);
			switch (beh) {
				case asBEHAVE_ADDREF:      hasAddRef = true;        break;
				case asBEHAVE_RELEASE:     hasRelease = true;       break;
				case asBEHAVE_CONSTRUCT:   hasConstruct = true;     break;
				case asBEHAVE_DESTRUCT:    hasDestruct = true;      break;
				case asBEHAVE_GETREFCOUNT: hasGcGetRefCount = true; break;
				case asBEHAVE_SETGCFLAG:   hasGcSetFlag = true;     break;
				case asBEHAVE_GETGCFLAG:   hasGcGetFlag = true;     break;
				case asBEHAVE_ENUMREFS:    hasGcEnumRefs = true;    break;
				case asBEHAVE_RELEASEREFS: hasGcReleaseAll = true;  break;
				default: break;
			}
		}

		const bool gc     = (flags & asOBJ_GC)     != 0;
		const bool ref    = (flags & asOBJ_REF)    != 0;
		const bool val    = (flags & asOBJ_VALUE)  != 0;
		const bool scoped = (flags & asOBJ_SCOPED) != 0;
		const bool pod    = (flags & asOBJ_POD)    != 0;
		const bool nohandle = (flags & asOBJ_NOHANDLE) != 0;
		const bool nocount  = (flags & asOBJ_NOCOUNT)  != 0;

		// asOBJ_GC is 0x100 by this engine's own header; when the engine sets
		// the gc flag it also sets asOBJ_GC on the *type* only if the type was
		// registered for the garbage collector. A type registered with
		// RegisterObjectType and no behaviours at all still gets here, and the
		// question the engine asks of it is which of these five rules applies.
		const bool rule_gc_ref   = gc && ref;
		const bool rule_gc_value = gc && !ref;
		const bool rule_scoped   = scoped;
		const bool rule_ref      = ref && !scoped && !nohandle && !nocount;
		const bool rule_value    = val && !pod;

		const bool applies = rule_gc_ref || rule_gc_value || rule_scoped || rule_ref || rule_value;
		if (applies) ++subject;

		const char* refused = nullptr;
		if (rule_gc_ref && (!hasAddRef || !hasRelease || !hasGcGetRefCount ||
		                    !hasGcSetFlag || !hasGcGetFlag || !hasGcEnumRefs || !hasGcReleaseAll))
			refused = "gc ref";
		else if (rule_gc_value && !hasGcEnumRefs)
			refused = "gc value";
		else if (rule_scoped && !hasRelease)
			refused = "scoped";
		else if (rule_ref && (!hasAddRef || !hasRelease))
			refused = "ref";
		else if (rule_value && (!hasConstruct || !hasDestruct))
			refused = "value";

		if (refused) ++offenders;

		std::fprintf(stderr,
			"probe: type %u \"%s\" id=%d flags=0x%llx%s%s%s%s%s%s%s "
			"beh=%u[addref=%d release=%d ctor=%d dtor=%d gc=%d/%d/%d/%d/%d] "
			"rules[gc_ref=%d gc_val=%d scoped=%d ref=%d value=%d] -> %s\n",
			i, type->GetName() ? type->GetName() : "?", type->GetTypeId(),
			(unsigned long long)flags,
			gc ? " GC" : "", ref ? " REF" : "", val ? " VALUE" : "",
			scoped ? " SCOPED" : "", pod ? " POD" : "",
			nohandle ? " NOHANDLE" : "", nocount ? " NOCOUNT" : "",
			(unsigned)beh_count,
			hasAddRef ? 1 : 0, hasRelease ? 1 : 0, hasConstruct ? 1 : 0, hasDestruct ? 1 : 0,
			hasGcGetRefCount ? 1 : 0, hasGcSetFlag ? 1 : 0, hasGcGetFlag ? 1 : 0,
			hasGcEnumRefs ? 1 : 0, hasGcReleaseAll ? 1 : 0,
			rule_gc_ref ? 1 : 0, rule_gc_value ? 1 : 0, rule_scoped ? 1 : 0,
			rule_ref ? 1 : 0, rule_value ? 1 : 0,
			refused ? refused : "clean");
		std::fflush(stderr);
	}

	std::fprintf(stderr,
		"probe: %u type(s) subject to a rule, %d would be REFUSED by the engine's validation\n",
		(unsigned)subject, offenders);
	std::fflush(stderr);
	return offenders;
}
