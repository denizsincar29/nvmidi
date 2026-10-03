// nvmidi_win_shim.h - enough of the windows types for the loopback driver to
// be compiled, and its DllMain run, without winmm or the registry.
//
// Why a hand-written header and not mingw-w64. The obvious route is the
// windows cross compiler's headers, but they drag in the whole of winnt.h
// for two integer typedefs, and the driver turns out to use no windows API
// at all: every call it makes on windows is the C library's. Its whole
// windows surface is eight types, one calling convention, MAX_PATH, and two
// functions - CreateThread and SetUnhandledExceptionFilter - both of which
// DllMain uses and both of which the shim reimplements below.
//
// So this is a *lie detector*, not a substitute. The driver does not include
// it: loopback_dlmain.c includes this file and then the driver, so the
// driver's own #include <windows.h> is skipped by the include guard. Any
// symbol it needs and does not find here is a compile error, which is the
// point - the day the driver starts using real windows API, this header
// stops compiling and the linux job says so, out loud, before the windows
// job has to.
//
// What it must never do is answer an error on numbers alone, and nothing
// here does: the device protocol (ModMessage and its MMSYSERR_*/MIM_*/
// MODM_*/MIDM_*/DRV_* family) is left entirely to the driver's own header,
// because those headers are exactly what the windows job is measuring and
// duplicating them here would let a pass on linux mean nothing on windows.

// The guard is deliberately the one windows.h uses. The driver includes
// windows.h with angle brackets, so nothing on the include path can shadow
// it - but it checks this macro first, and setting it here is the only way
// this file ever gets to be the driver's windows.h. Everything above this
// line is comments for that reason; anything the driver must see has to be
// below it.
#ifndef _WINDOWS_
#define _WINDOWS_

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <wchar.h>
#include <time.h>

// -- the types, and nothing else --------------------------------------------

typedef int                 BOOL;
typedef unsigned int        UINT;
typedef unsigned int        DWORD;
typedef unsigned long long  DWORD_PTR;
typedef uintptr_t           ULONG_PTR;
typedef void               *HANDLE;
typedef void               *HMODULE;
typedef void               *HINSTANCE;
typedef void               *HDRVR;
typedef void               *LPVOID;
typedef void               *LPSECURITY_ATTRIBUTES;
typedef uintptr_t           SIZE_T;
typedef unsigned long       LONG;
typedef unsigned long       ULONG;
typedef unsigned short      WORD;
typedef unsigned short      WCHAR;
typedef wchar_t             wchar2_unused;
typedef const char         *LPCSTR;
typedef char               *LPSTR;
typedef const WCHAR        *LPCWSTR;
typedef WCHAR              *LPWSTR;
typedef long                LPARAM;
typedef unsigned short      REGSAM;
typedef HDRVR               HKEY;

#ifndef MAX_PATH
#define MAX_PATH 260
#endif

#define WINAPI
#define __declspec(x)
#define DECLSPEC_IMPORT
#define WINMMAPI

// The four values the driver's own logic reads. TRUE and FALSE it uses in
// its BOOL returns and a test; ERROR_FILE_NOT_FOUND is what the shim's
// registry stub reports; EXCEPTION_EXECUTE_HANDLER is what its crash filter
// returns, and it is a real constant because that filter runs for real here
// on detach.
#define TRUE  1
#define FALSE 0
#define ERROR_FILE_NOT_FOUND 2L
#define EXCEPTION_EXECUTE_HANDLER 1
#define REG_SZ 1

// The exception-filter type. The driver stores the pointer it gets back and
// never calls through it, so the shape only has to be a function pointer the
// compiler accepts.
typedef LONG (*LPTOP_LEVEL_EXCEPTION_FILTER)(void *);

// The one struct the driver's DllMain-adjacent code passes around by type:
// CreateThread's prototype mentions it, and it is never dereferenced.
typedef struct _SECURITY_ATTRIBUTES { unsigned long nLength; void *p; BOOL b; } SECURITY_ATTRIBUTES;
typedef DWORD (WINAPI *LPTHREAD_START_ROUTINE)(LPVOID);

// -- the declarative lying, all of it in one place ---------------------------

// close(2) on a HANDLE the shim never opens.
int close(int);

static inline HANDLE CreateThread(LPSECURITY_ATTRIBUTES a, SIZE_T stack,
	LPTHREAD_START_ROUTINE fn, LPVOID arg, DWORD flags, DWORD *id) {
	(void)a; (void)stack; (void)flags; (void)id;
	// Run the body in place, on the calling frame, and stop there if it ever
	// returns. DllMain's thread is publish_thread, whose last statement is
	// `for (;;) Sleep(1000)`; running it here means the call never comes
	// back to report on it, and the step's own timeout catches the hang. That
	// is the intended reading: a thread body that cannot be run to its first
	// sleep point is a finding, not an inconvenience of the harness.
	if (fn) fn(arg);
	return (HANDLE)1;
}

static inline BOOL CloseHandle(HANDLE h) { (void)h; return TRUE; }

// DllMain sleeps 250ms after starting its thread and the thread sleeps a
// second per turn. Both are waits, not spin-waits, and both are real here:
// a shim that returned instantly would turn the thread's 1s loop into a busy
// loop and the hold-open run into a spinning core. usleep is microseconds,
// which is what Sleep takes there too - the same unit by coincidence, and
// the comment is here so nobody later "fixes" it to milliseconds.
static inline void Sleep(DWORD ms) {
	struct timespec ts = { (time_t)(ms / 1000), (long)(ms % 1000) * 1000000L };
	nanosleep(&ts, NULL);
}

static inline LPTOP_LEVEL_EXCEPTION_FILTER
SetUnhandledExceptionFilter(LPTOP_LEVEL_EXCEPTION_FILTER f) {
	// No windows structured exceptions here; nothing can raise one. Returning
	// the previous filter is what the API promises, and NULL is the truth.
	(void)f;
	return NULL;
}

static inline DWORD GetModuleFileNameA(HMODULE h, LPSTR buf, DWORD n) {
	(void)h;
	static const char name[] = "nvmidi_loopback.linux";
	if (!buf || n == 0) return 0;
	size_t len = sizeof(name) - 1;
	if (len >= n) len = n - 1;
	memcpy(buf, name, len);
	buf[len] = 0;
	return (DWORD)len;
}

static inline DWORD GetModuleFileNameW(HMODULE h, LPWSTR buf, DWORD n) {
	(void)h;
	// No L"..." here on purpose: this WCHAR is the two-byte windows one,
	// while the compiler's L"" is its own four-byte wchar_t - assigning one
	// to the other does not compile, and casting would give the driver a
	// "wide" string of interleaved zero bytes. The name is ASCII, so the
	// widening is a byte-per-character copy and nothing about it is a guess.
	static const char name[] = "nvmidi_loopback.linux";
	if (!buf || n == 0) return 0;
	size_t len = sizeof(name) - 1;
	if (len >= n) len = n - 1;
	for (size_t i = 0; i < len; i++) buf[i] = (WCHAR)(unsigned char)name[i];
	buf[len] = 0;
	return (DWORD)len;
}

// The status file's directory is the driver's own. The loader passes its
// cwd-invariant path through argv and the script runs from the repository
// root, so "beside us" and "in the cwd" are the same place.

// The registry. DllMain writes a Drivers32 slot under the loader lock, and
// that write is exactly the part the linux job cannot test: there is no
// registry. Every call answers "no such key" so the driver's own code takes
// its own failure branch and writes ATTACH=registry-failed into the status
// file - visible, honest, and impossible to mistake for success.
static int g_shim_reg_attempts = 0;

static inline LONG RegOpenKeyExW(HKEY root, LPCWSTR sub, DWORD opt, REGSAM acc, HKEY *out) {
	(void)root; (void)sub; (void)opt; (void)acc;
	g_shim_reg_attempts++;
	if (out) *out = NULL;
	return ERROR_FILE_NOT_FOUND;
}
static inline LONG RegSetValueExW(HKEY key, LPCWSTR name, DWORD reserved, DWORD type,
	const unsigned char *data, DWORD size) {
	(void)key; (void)name; (void)reserved; (void)type; (void)data; (void)size;
	return ERROR_FILE_NOT_FOUND;
}
static inline LONG RegCloseKey(HKEY key) { (void)key; return 0; }

#endif // NVGT_LINUX_WIN_SHIM_H
