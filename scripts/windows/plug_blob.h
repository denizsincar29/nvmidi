// A stand-in for whatever the engine hands a plugin's entry point.
//
// One include, from plug_api.c. The struct is not there to answer the version
// question - src/nvgt_plugin.h exports nvgt_plugin_version(), which returns the
// compiled-in api version directly, so that question is answered by a
// zero-argument call. It is there for the second half of the probe, which hands
// the entry point a deliberately wrong version: prepare_plugin reads only this
// field before returning false (src/nvgt_plugin.h:157), so only this field has
// to be right.
//
// It goes away together with plug_api.c once attribution is settled.
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
