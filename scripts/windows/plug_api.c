// Ask the built nvmidi.dll one question, as a number: which plugin api version
// was this build compiled against?
//
// The e2e's script-side names are all refused and the plugin's stderr is empty
// in every run, which means the register loop never ran against a live engine.
// prepare_plugin is the gate in front of it: it compares shared->version
// against NVGT_PLUGIN_API_VERSION and returns false on a mismatch, after which
// the plugin prints one line naming both numbers. That line is absent from
// every artifact, and absence is ambiguous - a failed handshake whose line was
// lost reads exactly like a handshake that passed.
//
// The first cut of this file called the entry point once per candidate version
// with a hand-rolled stand-in struct, on the theory that the returned bool
// would name the accepted version. Two things in src/nvgt_plugin.h kill that:
//
//   * the entry point's exported symbol is nvgt_plugin, not plugin_main.
//     #define plugin_main extern "C" plugin_export bool nvgt_plugin (line
//     148) - so a lookup of "plugin_main" would only ever return NULL, and
//     the probe would have reported "no export" for a dll that exports
//     exactly the right one.
//   * the returned bool was never clean evidence anyway. prepare_plugin
//     accepts a matching version and then calls
//     asPrepareMultithread(shared->script_thread_manager) (line 163) before
//     returning true - through the macro-expanded function pointers it
//     assigned on line 159 from the same blob. With an all-zero blob that is
//     a call through a null function pointer, so an accepted version would
//     fault rather than return. The previous comment claimed the accepted
//     version "stops on the zero engine pointer, where it stops"; it does
//     not reach the engine pointer at all. That reading was written before
//     this header was read.
//
// The header generates a better answer on line 152:
//     plugin_version() { return NVGT_PLUGIN_API_VERSION; }
// compiled into the dll as the export nvgt_plugin_version (line 149), taking
// no arguments and returning the number directly. So this asks the dll for
// its version instead of guessing at it: no struct, no blob, no entry-point
// call, no crash window. If the number this prints equals the number nvgt
// reports for itself, the handshake is not the gate - and if it differs, the
// difference is the finding.
//
// It also calls the entry point once, with a deliberately wrong version, to
// put the plugin's own mismatch line on stdout. See the comment at that call.
//
// This is a probe. It is not the shipping fix for the runner half, and it goes
// away once the number has been read.

#include <stdio.h>
#include <windows.h>

#include "plug_blob.h"

typedef int (*version_func)();
typedef bool (*entry_func)(void*);

int main(void) {
    HMODULE h;
    version_func vf;
    entry_func entry;
    int v;

    h = LoadLibraryA("nvmidi.dll");
    if (!h) { printf("LoadLibrary failed %lu\n", (unsigned long)GetLastError()); return 3; }

    vf = (version_func)GetProcAddress(h, "nvgt_plugin_version");
    printf("nvgt_plugin_version=%p\n", (void*)vf);
    if (!vf) { printf("no nvgt_plugin_version export\n"); return 4; }

    v = vf();
    printf("the dll was compiled against plugin api version %d\n", v);

    // Read: is the line the plugin writes on a version mismatch the engine's
    // own line, or the plugin's? That decides whether the plugin's stderr
    // reaches anything at all - and until it does, every silent registration
    // has two readings and no way to pick between them.
    //
    // The blob is the struct in plug_blob.h: version first, zeroes behind it.
    // It is NOT nvgt's real nvgt_plugin_shared - only the version field is
    // read before the mismatch returns, so only that field has to be right.
    //
    // Give it the WRONG version on purpose. prepare_plugin compares the field
    // against its own NVGT_PLUGIN_API_VERSION and returns false *before*
    // touching a single function pointer (src/nvgt_plugin.h:157), so the
    // plugin prints its one line and stops. No call through a null pointer is
    // reachable this way, which is what makes the deliberately-wrong number
    // the safe one to hand it - the version it actually wants is the one that
    // would carry it on to asPrepareMultithread and fault.
    blob.version = (unsigned int)(v + 1);
    entry = (entry_func)GetProcAddress(h, "nvgt_plugin");
    printf("nvgt_plugin=%p\n", (void*)entry);
    if (!entry) { printf("no nvgt_plugin export\n"); return 5; }

    printf("--- calling the entry point with a deliberately wrong version ---\n");
    fflush(stdout);
    printf("returned %d\n", entry(&blob) ? 1 : 0);
    printf("--- end of the entry point's attempt ---\n");
    return 0;
}
