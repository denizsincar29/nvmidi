// A stand-in for whatever the engine hands a plugin's entry point.
//
// Nothing includes this any more. It was written for a probe that called the
// entry point once per candidate version with this struct in hand, and that
// probe no longer calls the entry point at all: src/nvgt_plugin.h exports
// nvgt_plugin_version(), which returns the compiled-in api version directly,
// so the question is answered by a zero-argument call rather than by handing
// the plugin a fabricated struct and reading the bool that comes back.
//
// It stays in the tree only because removing it and the line that copies it
// into the probe directory is part of the same cleanup as removing the probe
// itself. Do not build anything on it.
//
// Two things it got wrong, recorded so the next reader does not repeat them:
// the plugin's exported symbol is nvgt_plugin, not plugin_main; and a version
// the plugin accepts does not stop at the script_engine pointer, because
// prepare_plugin calls asPrepareMultithread(shared->script_thread_manager)
// before it returns.
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
