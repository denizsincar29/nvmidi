// Ask the built nvmidi.dll one question, as a number: which plugin api version
// does it accept?
//
// The e2e's script-side names are all refused and the plugin's stderr is empty
// in every run, which means the register loop never ran against a live engine.
// prepare_plugin is the gate in front of it: it compares shared->version
// against NVGT_PLUGIN_API_VERSION and returns false on a mismatch, after which
// the plugin prints one line naming both numbers. That line is absent from
// every artifact, and absence is ambiguous - a failed handshake whose line was
// lost reads exactly like a handshake that passed.
//
// So this calls plugin_main once per candidate version and prints what it
// returns. The blob it hands over is the struct below: the version first, the
// rest zeroes. A version the plugin accepts carries it past the check and onto
// the zero engine pointer, where it stops; a version it rejects stops it at
// the handshake. Either way the accepted version is the one that returns true.
//
// This is a probe. It is not the shipping fix for the runner half, and it goes
// away once the number has been read.

#include <stdio.h>
#include <stdlib.h>
#include <windows.h>

#include "plug_blob.h"

int main(int argc, char** argv) {
    unsigned int v;
    HMODULE h;
    pfn pm;
    int r;

    if (argc < 2) { printf("usage: plug_api <version>\n"); return 2; }

    v = (unsigned int)strtoul(argv[1], NULL, 10);
    blob.version = v;

    h = LoadLibraryA("nvmidi.dll");
    if (!h) { printf("LoadLibrary failed %lu\n", (unsigned long)GetLastError()); return 3; }

    pm = (pfn)GetProcAddress(h, "plugin_main");
    printf("plugin_main=%p\n", (void*)pm);
    if (!pm) { printf("no plugin_main export\n"); return 4; }

    r = pm(&blob);
    printf("claimed %u -> plugin_main returned %d (%s)\n",
           v, r, r ? "ACCEPTED" : "refused at the handshake");
    return 0;
}
