/* alsa_dlopen.h - the ALSA build, without a hard link to libasound.so.2
 *
 * Included by src/nvmidi.cpp *instead of* <alsa/asoundlib.h> when the plugin
 * is built with MIDI_BACKEND=alsa-soft, and by third_party/rtmidi/RtMidi.cpp
 * the same way.
 *
 * Why this file exists.
 *
 * RtMidi's ALSA backend is the only one that can reach MIDI hardware on Linux,
 * and the ordinary way to build it - MIDI_BACKEND=alsa, i.e. -D__LINUX_ALSA__
 * and -lasound - puts a "needed: libasound.so.2" entry into nvmidi.so. The
 * dynamic loader refuses a library whose dependencies it cannot resolve: the
 * whole library, before a single symbol is looked up. The engine's only words
 * for that are
 *
 *     ERROR: failed to load plugin
 *
 * and the script is never run, so nothing on the user's side can say what
 * happened. That is the exact bug a user hit on release v0.7.1, and it is a
 * trap rather than a fallback: the library is present, complete and healthy,
 * and a missing system package makes it look like a missing file.
 *
 * So ALSA is not linked. It is opened by name at run time, on first use, and
 * every snd_* call goes through a pointer. A machine without ALSA then loads
 * the plugin, runs the script, and gets a sentence naming the missing package
 * from midi_api_name() / midi_last_error() instead of a death the engine
 * cannot describe.
 *
 * How.
 *
 * The trick is that ALSA's own headers are still used - unchanged, the real
 * ones, so every struct, every macro and every inline function is the genuine
 * article the ABI expects. What changes is where the *functions* come from.
 * A function-like macro is defined for each of the 76 snd_* symbols RtMidi
 * calls, so the source text `snd_seq_open(...)` becomes
 * `nvmidi_alsa_snd_seq_open(...)`, and each of those wrappers is a function
 * pointer that is filled in by dlsym.
 *
 * That is what makes this different from hand-writing a second set of
 * declarations: the ABI is checked against ALSA's own headers at build time.
 * Write a wrong argument list and the compiler says so instead of the plugin
 * crashing on a user's machine. And because the include is nested - this file
 * pulls in the real asoundlib.h from inside itself - the macros cannot leak
 * outward: they stop applying when this file ends, so nothing in the plugin
 * outside these two translation units is touched by them. Without that nesting
 * the macros would rewrite calls in ALSA's own inline functions, which is a
 * silent, spectacular breakage.
 *
 * The header stays reachable at build time (libasound2-dev), and is needed at
 * run time only for the version macro below, which the plugin reports.
 */

#pragma once

#ifndef __LINUX_ALSA__
#error "alsa_dlopen.h is only for the ALSA backend; define __LINUX_ALSA__ (MIDI_BACKEND=alsa-soft)."
#endif

#ifndef _WIN32
#include <dlfcn.h>
#endif

#include <string>

#include <alsa/asoundlib.h>

/* The library's soname, not the linker's -lasound. This is the name the
 * dynamic loader itself uses, so it is the name that has to be opened. */
#define NVGT_ALSA_LIBRARY "libasound.so.2"

/* 76 function pointers, one per snd_* symbol RtMidi's ALSA backend calls, and
 * a wrapper for each. All of it is generated from one X-macro list so that a
 * symbol cannot be present in one place and missing in another. */
#define NVGT_ALSA_FUNCTIONS(X) \
	X(int, snd_midi_event_decode, (snd_midi_event_t *dev, unsigned char *buf, long count, const snd_seq_event_t *ev)) \
	X(int, snd_midi_event_encode, (snd_midi_event_t *dev, const unsigned char *buf, long count, snd_seq_event_t *ev)) \
	X(void, snd_midi_event_free, (snd_midi_event_t *dev)) \
	X(int, snd_midi_event_init, (snd_midi_event_t *dev)) \
	X(int, snd_midi_event_new, (size_t bufsize, snd_midi_event_t **rdev)) \
	X(void, snd_midi_event_no_status, (snd_midi_event_t *dev, int on)) \
	X(void, snd_midi_event_reset_decode, (snd_midi_event_t *dev)) \
	X(int, snd_midi_event_resize_buffer, (snd_midi_event_t *dev, size_t bufsize)) \
	X(int, snd_seq_alloc_named_queue, (snd_seq_t *seq, const char *name)) \
	X(int, snd_seq_client_id, (snd_seq_t *seq)) \
	X(int, snd_seq_client_info_get_card, (const snd_seq_client_info_t *info)) \
	X(int, snd_seq_client_info_get_client, (const snd_seq_client_info_t *info)) \
	X(const char *, snd_seq_client_info_get_name, (snd_seq_client_info_t *info)) \
	X(void, snd_seq_client_info_set_client, (snd_seq_client_info_t *info, int client)) \
	X(int, snd_seq_close, (snd_seq_t *seq)) \
	X(int, snd_seq_connect_from, (snd_seq_t *seq, int my_port, int src_client, int src_port)) \
	X(int, snd_seq_create_simple_port, (snd_seq_t *seq, const char *name, unsigned int caps, unsigned int type)) \
	X(int, snd_seq_delete_port, (snd_seq_t *seq, int port)) \
	X(int, snd_seq_drain_output, (snd_seq_t *seq)) \
	X(void, snd_seq_ev_clear, (snd_seq_event_t *ev)) \
	X(void, snd_seq_ev_set_direct, (snd_seq_event_t *ev)) \
	X(void, snd_seq_ev_set_source, (snd_seq_event_t *ev, int port)) \
	X(void, snd_seq_ev_set_subs, (snd_seq_event_t *ev)) \
	X(int, snd_seq_event_input, (snd_seq_t *seq, snd_seq_event_t **ev)) \
	X(int, snd_seq_event_input_pending, (snd_seq_t *seq, int fetch_sequencer)) \
	X(int, snd_seq_event_output, (snd_seq_t *seq, snd_seq_event_t *ev)) \
	X(int, snd_seq_free_queue, (snd_seq_t *seq, int q)) \
	X(int, snd_seq_get_any_client_info, (snd_seq_t *seq, int client, snd_seq_client_info_t *info)) \
	X(int, snd_seq_get_any_port_info, (snd_seq_t *seq, int client, int port, snd_seq_port_info_t *info)) \
	X(int, snd_seq_get_port_info, (snd_seq_t *seq, int port, snd_seq_port_info_t *info)) \
	X(int, snd_seq_get_port_subscription, (snd_seq_t *seq, snd_seq_port_subscribe_t *sub)) \
	X(int, snd_seq_open, (snd_seq_t **seqp, const char *name, int streams, int mode)) \
	X(int, snd_seq_poll_descriptors, (snd_seq_t *seq, struct pollfd *pfds, unsigned int space, short events)) \
	X(int, snd_seq_poll_descriptors_count, (snd_seq_t *seq, short events)) \
	X(unsigned int, snd_seq_port_info_get_capability, (const snd_seq_port_info_t *info)) \
	X(int, snd_seq_port_info_get_client, (const snd_seq_port_info_t *info)) \
	X(const char *, snd_seq_port_info_get_name, (const snd_seq_port_info_t *info)) \
	X(int, snd_seq_port_info_get_port, (const snd_seq_port_info_t *info)) \
	X(unsigned int, snd_seq_port_info_get_type, (const snd_seq_port_info_t *info)) \
	X(void, snd_seq_port_info_set_capability, (snd_seq_port_info_t *info, unsigned int capability)) \
	X(void, snd_seq_port_info_set_client, (snd_seq_port_info_t *info, int client)) \
	X(void, snd_seq_port_info_set_midi_channels, (snd_seq_port_info_t *info, int channels)) \
	X(void, snd_seq_port_info_set_name, (snd_seq_port_info_t *info, const char *name)) \
	X(void, snd_seq_port_info_set_port, (snd_seq_port_info_t *info, int port)) \
	X(void, snd_seq_port_info_set_timestamp_queue, (snd_seq_port_info_t *info, int queue)) \
	X(void, snd_seq_port_info_set_timestamp_real, (snd_seq_port_info_t *info, int timestamping)) \
	X(void, snd_seq_port_info_set_timestamping, (snd_seq_port_info_t *info, int timestamping)) \
	X(void, snd_seq_port_info_set_type, (snd_seq_port_info_t *info, unsigned int type)) \
	X(void, snd_seq_port_subscribe_copy, (snd_seq_port_subscribe_t *dst, const snd_seq_port_subscribe_t *src)) \
	X(int, snd_seq_port_subscribe_free, (snd_seq_port_subscribe_t *sub)) \
	X(const snd_seq_addr_t *, snd_seq_port_subscribe_get_dest, (const snd_seq_port_subscribe_t *sub)) \
	X(const snd_seq_addr_t *, snd_seq_port_subscribe_get_sender, (const snd_seq_port_subscribe_t *sub)) \
	X(snd_seq_port_subscribe_t *, snd_seq_port_subscribe_malloc, (void)) \
	X(void, snd_seq_port_subscribe_set_dest, (snd_seq_port_subscribe_t *sub, const snd_seq_addr_t *addr)) \
	X(void, snd_seq_port_subscribe_set_sender, (snd_seq_port_subscribe_t *sub, const snd_seq_addr_t *addr)) \
	X(void, snd_seq_port_subscribe_set_time_real, (snd_seq_port_subscribe_t *sub, int val)) \
	X(void, snd_seq_port_subscribe_set_time_update, (snd_seq_port_subscribe_t *sub, int val)) \
	X(int, snd_seq_query_next_client, (snd_seq_t *seq, snd_seq_client_info_t *info)) \
	X(int, snd_seq_query_next_port, (snd_seq_t *seq, snd_seq_port_info_t *info)) \
	X(void, snd_seq_queue_tempo_set_ppq, (snd_seq_queue_tempo_t *info, int ppq)) \
	X(void, snd_seq_queue_tempo_set_tempo, (snd_seq_queue_tempo_t *info, unsigned int tempo)) \
	X(int, snd_seq_set_client_name, (snd_seq_t *seq, const char *name)) \
	X(int, snd_seq_set_port_info, (snd_seq_t *seq, int port, snd_seq_port_info_t *info)) \
	X(int, snd_seq_set_queue_tempo, (snd_seq_t *seq, int q, snd_seq_queue_tempo_t *tempo)) \
	X(int, snd_seq_start_queue, (snd_seq_t *seq, int q, snd_seq_real_time_t *tm)) \
	X(int, snd_seq_stop_queue, (snd_seq_t *seq, int q, snd_seq_real_time_t *tm)) \
	X(int, snd_seq_subscribe_port, (snd_seq_t *seq, snd_seq_port_subscribe_t *sub)) \
	X(const char *, snd_strerror, (int errnum)) \
	X(int, snd_seq_unsubscribe_port, (snd_seq_t *seq, snd_seq_port_subscribe_t *sub))

/* The pointers. One static per symbol: static so that the two translation
 * units that include this file each carry their own copy and the linker has
 * nothing to reconcile; the resolution below is idempotent, so two copies
 * filled twice is the same as one copy filled once. */
#define NVGT_ALSA_POINTER(ret, name, args) static ret (*nvmidi_##name) args = nullptr;
NVGT_ALSA_FUNCTIONS(NVGT_ALSA_POINTER)
#undef NVGT_ALSA_POINTER

/* And the wrappers the macros below redirect calls to. Calling through a null
 * pointer is a crash, but every call goes through nvgt_alsa_ready() first
 * (RtMidi's constructors and the plugin's entry points both check it), so a
 * null here is unreachable in practice - and the alternative, silently
 * returning an error code, would hide a plugin bug as a MIDI bug. */
#define NVGT_ALSA_WRAPPER(ret, name, args) \
	static ret nvmidi_##name args { return ::nvmidi_##name; }
NVGT_ALSA_FUNCTIONS(NVGT_ALSA_WRAPPER)
#undef NVGT_ALSA_WRAPPER

/* The redirection itself. Function-like macros, so only a call is rewritten;
 * taking the address of snd_strerror would still name the real symbol, which
 * is why nothing here does. */
#define NVGT_ALSA_REDIRECT(ret, name, args) \
	#define name NVGT_ALSA_REDIRECT_JOIN(nvmidi_, name)
#define NVGT_ALSA_REDIRECT_JOIN(a, b) NVGT_ALSA_REDIRECT_JOIN2(a, b)
#define NVGT_ALSA_REDIRECT_JOIN2(a, b) a##b
NVGT_ALSA_FUNCTIONS(NVGT_ALSA_REDIRECT)

namespace nvmidi_alsa {

/* Filled by dlopen/dlsym on the first call, and the reason keeps whatever the
 * loader said so the plugin can report it. */
inline const char** reason_slot() {
	static const char* reason = "";
	return &reason;
}

/* The one entry point to the library. Idempotent: the first call opens
 * libasound.so.2 and resolves every pointer, later calls are a boolean test.
 * Returns true when ALSA is usable. */
inline bool load() {
	static bool resolved = false;
	static bool tried = false;
	if (tried) return resolved;
	tried = true;
#ifndef _WIN32
	void* handle = dlopen(NVGT_ALSA_LIBRARY, RTLD_NOW | RTLD_GLOBAL);
	if (!handle) {
		const char* said = dlerror();
		*reason_slot() = said ? said : "dlopen failed without a reason";
		return false;
	}
	/* Every symbol is required: a partial resolution would leave callers
	 * jumping through null pointers later, far from here. The first miss is
	 * named, because "undefined symbol snd_seq_open" is a sentence a person
	 * can act on and a crash is not. */
	const char* missing = nullptr;
	size_t missing_index = 0;
	size_t index = 0;
#define NVGT_ALSA_RESOLVE(ret, name, args) \
	do { \
		::nvmidi_##name = reinterpret_cast<ret (*) args>(dlsym(handle, #name)); \
		if (!::nvmidi_##name && !missing) { missing = #name; missing_index = index; } \
	} while (0);
	NVGT_ALSA_FUNCTIONS(NVGT_ALSA_RESOLVE)
	(void)missing_index;
#undef NVGT_ALSA_RESOLVE
	if (missing) {
		static std::string text;
		text = std::string("libasound.so.2 is present but has no ") + missing +
			"; it is too old or not really the alsa library";
		*reason_slot() = text.c_str();
		return false;
	}
	resolved = true;
#else
	*reason_slot() = "the alsa backend is linux only";
	resolved = false;
#endif
	return resolved;
}

/* True when ALSA can be used, i.e. when the library was found and every
 * symbol resolved. */
inline bool ready() { return load(); }

/* What went wrong, in words, or an empty string when nothing did. */
inline std::string error() {
	if (ready()) return "";
	return std::string("alsa is unavailable: ") + *reason_slot();
}

} // namespace nvmidi_alsa
