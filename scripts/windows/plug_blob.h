// A stand-in for whatever the engine hands plugin_main, used only to ask the
// plugin one question: which api version does it accept?
//
// This is not a declaration of nvgt's real plugin-shared struct and does not
// pretend to be. It is a locally defined prefix: the version field, which
// prepare_plugin reads first, and enough zeroed space behind it that the lane
// the engine really uses is not narrower than what the plugin was built
// against. If prepare_plugin accepts the version it proceeds to the engine
// pointer, which is zero here, and stops - and that stop is itself the answer,
// because it could only be reached by a version the plugin agreed with.
//
// The size is deliberately larger than the engine's own struct. The call is
// made by this probe, so nothing the plugin writes here can reach a caller:
// the danger with a too-small stand-in is a plugin that writes past the end of
// it, and there is no reason to accept that risk to ask a one-word question.
#ifndef NVMIDI_PLUG_BLOB_H
#define NVMIDI_PLUG_BLOB_H

#include <windows.h>

typedef int (*pfn)(void*);

typedef struct plug_blob {
    unsigned int version;
    unsigned char rest[252];   // zeroed; only the version is given a value
} plug_blob;

// One definition in the repo, shared by the probe's own translation unit. The
// first version of this had the powershell step build the source as a string
// literal, and the embedded quotes in the printf calls ended the literal early
// - the step died on a parse error inside its own script text rather than on
// anything about the plugin. A source file is a source file.
static plug_blob blob;

#endif
