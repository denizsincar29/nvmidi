// Linker shims for the Angelscript library on MSVC.
//
// nvgt_plugin.h declares the engine's functions as pointer *variables* in a
// plugin build (its X() list), and prepare_plugin() fills them from the shared
// struct. The plugin never calls them by their plain names.
//
// third_party/angelscript does. scriptarray.cpp takes the address of
// asAllocMem and asFreeMem outright (scriptarray.cpp:21-22) and calls others by
// name, so the translation unit asks the linker for a *function* symbol spelled
// asAllocMem - while the only definition in this dll is a variable, whose
// symbol carries a pointer-to-function type instead. MSVC answers:
//
//   scriptarray.obj : error LNK2001: unresolved external symbol asAllocMem
//     Hint: "void * (__cdecl* asAllocMem)(unsigned __int64)"
//
// A generated header of "#pragma comment(linker, "/alternatename:...")" lines
// was written for this and force-included; the link still failed with the same
// eight names on run 37010648892, so the pragma never reached the linker.
//
// The repair here is the /ALTERNATENAME switch on the link line, which cannot
// be lost the way a generated file can, plus this file to give it the defined
// side in a spelling that needs no mangling: each forwarder is extern "C", so
// its symbol is exactly its name. The alias then reads
//
//   /ALTERNATENAME:asAllocMem=asAllocMem_shim
//
// "the undefined symbol asAllocMem is, for this link, asAllocMem_shim". The
// forwarder calls through the pointer variable the header already declared,
// which prepare_plugin() has filled by the time any script compiles. The engine
// build never compiles this file; the guard is MSVC because that is where the
// error was measured.

#if defined(_MSC_VER)

// stddef.h, not cstddef: nvgt_plugin.h spells the type "size_t" unqualified,
// and cstddef only guarantees std::size_t.
#include <stddef.h>
// NVGT_PLUGIN_INCLUDE asks the header for extern declarations of the pointer
// variables rather than definitions: nvmidi.cpp already defines them, and a
// second definition in this translation unit would fail the link.
#define NVGT_PLUGIN_INCLUDE
#include "nvgt_plugin.h"

extern "C" {

// Each forwarder returns to the pointer variable of the same name. That
// variable is declared by nvgt_plugin.h above and set by prepare_plugin().
// A null there means the plugin ran without an engine, which cannot happen
// from NVGT and is a bug in a host that calls the plugin directly.

void* asAllocMem_shim(std::size_t n) { return asAllocMem(n); }
void asFreeMem_shim(void* p) { asFreeMem(p); }
const char* asGetLibraryOptions_shim() { return asGetLibraryOptions(); }
asIScriptContext* asGetActiveContext_shim() { return asGetActiveContext(); }
void asAcquireExclusiveLock_shim() { asAcquireExclusiveLock(); }
void asReleaseExclusiveLock_shim() { asReleaseExclusiveLock(); }
void asAcquireSharedLock_shim() { asAcquireSharedLock(); }
void asReleaseSharedLock_shim() { asReleaseSharedLock(); }
int asAtomicInc_shim(int& v) { return asAtomicInc(v); }
int asAtomicDec_shim(int& v) { return asAtomicDec(v); }
int asThreadCleanup_shim() { return asThreadCleanup(); }
const char* asGetLibraryVersion_shim() { return asGetLibraryVersion(); }
int asPrepareMultithread_shim(asIThreadManager* m) { return asPrepareMultithread(m); }

} // extern "C"

#endif // _MSC_VER
