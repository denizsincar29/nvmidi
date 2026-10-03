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

// The guard is deliberately the one windows.h uses, and it is a #define
// rather than an #ifndef for a reason that cost four CI runs. gcc on the
// ubuntu runner leaves this macro undefined, and an #ifndef here then
// defines it and suppresses the driver's own windows.h - but zig's glibc
// and musl targets predefine _WINDOWS_ themselves, so under that compiler
// the #ifndef never ran, every declaration in this file fell through to
// #endif, and the driver compiled against an empty header. A bare #define
// is right under both: defined already it is a no-op, undefined it is
// exactly what windows.h checks. Everything below this line has to stay
// below it.
#define _WINDOWS_
#define _MMSYSTEM_H_
#define _MMDDK_
#define _MMISCAPI_

// A guard of this file's own, so a header that must not be reached without it
// can say so by name. The windows sentinels above are deliberately the names
// windows.h uses and cannot be reused for this: they are defined on the
// windows build too, where this file is not included at all.
#define NVMIDI_WIN_SHIM_H 1

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
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
typedef unsigned long       MMVERSION;
typedef wchar_t             wchar2_unused;
typedef const char         *LPCSTR;
typedef char               *LPSTR;
typedef const WCHAR        *LPCWSTR;
typedef WCHAR              *LPWSTR;
typedef long                LPARAM;
typedef unsigned short      REGSAM;
typedef HDRVR               HKEY;
#define HKEY_LOCAL_MACHINE ((HKEY)0x80000002)

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
// (the filter typedef lives below, after EXCEPTION_POINTERS exists)

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

// The driver builds its status and trace paths from this, so what it returns
// decides where the harness looks for them. A relative name would land the
// files in whatever directory the binary was started from, which is the same
// thing on a windows runner and a trap here. It answers with the path of the
// running executable - the linux equivalent of what the API means - so the
// status file appears beside the binary, exactly as it does on windows, and
// the harness is told to look there too.
static inline DWORD GetModuleFileNameA(HMODULE h, LPSTR buf, DWORD n) {
	(void)h;
	if (!buf || n == 0) return 0;
	DWORD got = (DWORD)readlink("/proc/self/exe", buf, n - 1);
	if (got == 0 || got == (DWORD)-1) {
		static const char name[] = "nvmidi_loopback.linux";
		size_t len = sizeof(name) - 1;
		if (len >= n) len = n - 1;
		memcpy(buf, name, len);
		buf[len] = 0;
		return (DWORD)len;
	}
	buf[got] = 0;
	return got;
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

// -- the winmm protocol surface ----------------------------------------------
//
// The driver's whole vocabulary: the MMSYSERR returns, the two message
// namespaces (the handle-life one, DRV_*, and the device one, MODM_*/MIDM_*),
// the callback kinds, the caps structs and the two calls winmm itself would
// make into it. These were left out of the first version of this header on
// purpose - the plan was to skip the windows headers without inventing the
// protocol - and that plan did not survive contact: the driver's own logic is
// written in these names, so without them there is no compile at all, only a
// different error list.
//
// The trade this header makes is now explicit and worth saying out loud,
// because it is the price of the linux run. The numbers below are copied from
// winmm's headers, so the *dispatcher* is checked against the windows ones
// only on windows. What linux proves about it is the part that does not depend
// on the numbers: that the two tables do not share a case value, that every arm
// returns a value from the right family, and that DllMain and the thread run.
// A note that windows itself caught - MODM_DATA (7) read as DRV_FREE (0x0007) -
// is exactly the kind of mistake this file cannot catch and the windows build
// can. This header is a stand-in, not a second opinion.

typedef unsigned char BYTE;

// The return family. MMSYSERR_NOERROR is the success every driver message
// answers; INVALPARAM is the driver's refusal for a null pointer; NOTSUPPORTED
// is what makes the dispatcher fall through from the output half to the input
// half, so its exact value matters to the control flow and not just the error.
#define MMSYSERR_NOERROR       0
#define MMSYSERR_INVALPARAM    11
#define MMSYSERR_NOTSUPPORTED  8

// The two message namespaces, and the one thing about them that has to be
// understood before reading the dispatcher: they overlap. MODM_OPEN is 3 and
// DRV_OPEN is 3. A single table cannot hold both, which is the bug the driver's
// own comments describe at length.
#define DRV_LOAD         0x0001
#define DRV_ENABLE       0x0002
#define DRV_DISABLE      0x0003
#define DRV_FREE         0x0006
#define DRV_OPEN         0x0007
#define DRV_CLOSE        0x0008
#define DRV_QUERYDEVICEINTERFACESIZE 0x0030
#define DRV_QUERYDEVICEINTERFACE     0x0031
#define DRVCNF_OK        1
#define DRVCNF_CANCEL    2

#define MODM_OPEN        0x0003
#define MODM_CLOSE       0x0004
#define MODM_LONGDATA    0x0008
#define MODM_PREPARE     0x0009
#define MODM_UNPREPARE   0x000A
#define MODM_DATA        0x0007
#define MODM_GETDEVCAPS  0x0002
#define MODM_RESET       0x000B

#define MIDM_OPEN        0x0003
#define MIDM_CLOSE       0x0004
#define MIDM_ADDBUFFER   0x0007
#define MIDM_GETDEVCAPS  0x0002

#define MIM_DATA         0x03C1
#define MIM_LONGDATA     0x03C5

#define CALLBACK_TYPEMASK  0x00070000L
#define CALLBACK_NULL      0x00000000L
#define CALLBACK_FUNCTION  0x00030000L

// The caps structs winmm fills and the driver rewrites the name in. The field
// order and sizes are the windows ones - the driver writes szPname through a
// WCHAR * and indexes it by sizeof(WCHAR), so any other layout here would test
// a string copy that does not exist on windows.
typedef struct {
	WORD  wMid;
	WORD  wPid;
	MMVERSION vDriverVersion;   /* unsigned long */
	WCHAR szPname[32];
	WORD  wTechnology;
	WORD  wVoices;
	WORD  wNotes;
	WORD  wChannelMask;
	DWORD dwSupport;
} MIDIOUTCAPS;

typedef struct {
	WORD  wMid;
	WORD  wPid;
	MMVERSION vDriverVersion;
	WCHAR szPname[32];
	DWORD dwSupport;
} MIDIINCAPS;

// The buffer header winmm hands a driver on MIDM_ADDBUFFER. Only three fields
// are read: lpData, dwBufferLength and dwBytesRecorded.
typedef struct {
	LPSTR  lpData;
	DWORD  dwBufferLength;
	DWORD  dwBytesRecorded;
	DWORD_PTR dwUser;
	DWORD  dwFlags;
	DWORD_PTR dwLoops;
	void  *lpNext;
	DWORD_PTR reserved;
} MIDIHDR;

typedef struct { unsigned long Data1; unsigned short Data2, Data3; unsigned char Data4[8]; } GUID;
// The crash filter's parameter. Only two members are read, and only on a
// windows fault; on linux the filter is never called at all.
typedef struct {
	DWORD ExceptionCode;
	void *ExceptionAddress;
} SHIM_EXCEPTION_RECORD;
typedef struct {
	SHIM_EXCEPTION_RECORD *ExceptionRecord;
} SHIM_EXCEPTION_POINTERS;
typedef SHIM_EXCEPTION_POINTERS EXCEPTION_POINTERS;

// Moved below the struct it points at: written first, the struct tag was
// only ever visible inside that one prototype, so the driver's own
// crash_filter looked like an incompatible type rather than a forward order.
typedef LONG (*LPTOP_LEVEL_EXCEPTION_FILTER)(EXCEPTION_POINTERS *);

static inline LPTOP_LEVEL_EXCEPTION_FILTER
SetUnhandledExceptionFilter(LPTOP_LEVEL_EXCEPTION_FILTER f) {
	// No windows structured exceptions here; nothing can raise one. Returning
	// the previous filter is what the API promises, and NULL is the truth.
	(void)f;
	return NULL;
}

// The three reason codes DllMain switches on. The numbers are windows' and
// are only checked against windows by the windows build; here they simply
// have to exist and to be distinct, which is what makes the two branches of
// the driver's DllMain reachable in the order it expects.
#define DLL_PROCESS_DETACH 0
#define DLL_PROCESS_ATTACH 1

// RegQueryValueExW reads a value into a byte buffer and reports the type. The
// shim's registry has no values, so it answers not-found and the driver takes
// its own "slot is free" branch - which is the branch the linux run is meant
// to exercise anyway.
static inline LONG RegQueryValueExW(HKEY key, LPCWSTR name, DWORD *reserved,
	DWORD *type, unsigned char *data, DWORD *size) {
	(void)key; (void)name; (void)reserved; (void)type; (void)data; (void)size;
	return ERROR_FILE_NOT_FOUND;
}

typedef LONG (WINAPI *DRIVERPROC)(DWORD_PTR, HDRVR, UINT, LPARAM, LPARAM);
typedef DWORD (WINAPI *DRIVERMSGPROC)(DWORD, DWORD, DWORD_PTR, DWORD_PTR, DWORD_PTR);

#define ZeroMemory(p, n) memset((p), 0, (n))

// The caps name field is 32 WCHARs in both structs, and the driver loops to
// one less than the count. Named here so that the number appears once.
#define SHIM_CAPS_NAME_LEN 32

// A wide string whose element width is the windows one. The compiler's L"..."
// is an array of its own wchar_t - four bytes on linux - and every wide string
// in this driver is walked two bytes at a time, so an L"" reaching one of them
// is a type error at best and a string of interleaved zero bytes at worst.
// This macro builds the array by hand and the width is then a fact about the
// bytes, not about the compiler.
//
// It is called with a name, SHIM_L(midi), and not with a literal, SHIM_L"midi":
// that shape was tried first and silently did nothing, because a function-like
// macro's name must be followed by its own '(' and a quote is not one. The
// object it produced stayed the unexpanded token and the error was an
// undeclared identifier three hundred lines further down - the kind of
// diagnosis that reads as a missing declaration and is a missing parenthesis.
// six characters is the longest name used ("midi%d" is seven, and that one is
// not built here - it goes through shim_wprintf). This covers "midi" and every
// "midiN" the driver writes while it searches the ten slots.
#define SHIM_L(name) ((const WCHAR[]){ \
	(WCHAR)(#name)[0], (WCHAR)(#name)[1], (WCHAR)(#name)[2], (WCHAR)(#name)[3], \
	(WCHAR)(#name)[4], (WCHAR)(#name)[5], (WCHAR)(#name)[6], (WCHAR)0 })

// The same idea for the registry path, whose characters are not identifier
// safe. The widening is a byte-per-character copy because the path is ASCII;
// the buffer is a function-local static, so the returned pointer stays valid
// for the rest of the process - which is all this needs, since the key is
// opened once, from DllMain, and never freed. A local array here would have
// been the kind of bug that reads correct and hands back a dead pointer.
static inline const WCHAR *shim_str(const char *s) {
	static WCHAR buf[512];
	size_t i = 0;
	while (s[i] && i < 511) { buf[i] = (WCHAR)(unsigned char)s[i]; i++; }
	buf[i] = 0;
	return buf;
}
#define SHIM_STR(s) shim_str(s)

static inline void DisableThreadLibraryCalls(HMODULE m) { (void)m; }

#define ERROR_SUCCESS 0L
#define KEY_QUERY_VALUE 0x0001
#define KEY_SET_VALUE   0x0002

// GetLastError is read on the failure path of the publish thread only, and
// there is no error to report on linux: the shim's OpenDriver never fails, so
// this line is unreachable here. It exists so the driver compiles whole.
static inline DWORD GetLastError(void) { return 0; }

static inline DWORD midiOutGetNumDevs(void) { return 0; }
static inline DWORD midiInGetNumDevs(void)  { return 0; }
static inline HDRVR OpenDriver(LPCWSTR name, LPCWSTR path, DWORD_PTR p) {
	(void)name; (void)path; (void)p;
	return NULL;
}
static inline LONG DriverCallback(DWORD_PTR cb, DWORD flags, HDRVR h, UINT msg,
	DWORD_PTR inst, DWORD_PTR p1, DWORD_PTR p2) {
	(void)cb; (void)flags; (void)h; (void)msg; (void)inst; (void)p1; (void)p2;
	// winmm would call the listener's function here. There is no listener on
	// linux, and answering "no" is the honest value: a caller that checks it
	// gets the truth instead of a fabricated success.
	return 0;
}
// The driver's two conversions of a slot name to bytes. The wide strings it
// holds are ASCII by construction (the registry names midi..midi9), so the
// narrowing is a byte-per-character copy and not a code page question. It is
// named shim_w2mb rather than WideCharToMultiByte so that nobody later reads
// it as the real thing and expects it to fold case or substitute '?'.
static inline int shim_w2mb(const WCHAR *in, char *out, int outn) {
	if (!in || !out || outn <= 0) return 0;
	int i = 0;
	while (in[i] && i < outn) { out[i] = (char)(unsigned char)in[i]; i++; }
	out[i < outn ? i : outn - 1] = 0;
	return i;
}

// The four wide-string helpers below carry a shim_ prefix because musl's
// wchar.h already declares wcslen, wcscpy and wcsncpy, over four byte
// wchar_t. A same-named static here is a redeclaration error, not a
// shadow - the standard names are not ours to take, and _snwprintf is not a
// standard name at all but is prefixed anyway to keep the four together.
static inline size_t shim_wlen(const WCHAR *s) { size_t n = 0; while (s[n]) n++; return n; }
static inline WCHAR *shim_wcpy(WCHAR *d, const WCHAR *s) {
	size_t i = 0; while (s[i]) { d[i] = s[i]; i++; } d[i] = 0; return d;
}
static inline WCHAR *shim_wncpy(WCHAR *d, const WCHAR *s, size_t n) {
	size_t i = 0; for (; i < n && s[i]; i++) d[i] = s[i]; for (; i < n; i++) d[i] = 0; return d;
}
// Writes the driver's "midiN" slot name. The signature is (buffer, size, n)
// and not a format string: the only thing this has to produce is that one
// name, and a variadic printf without a working wide printf underneath it is
// a trap - the argument after the format would be read as a pointer.
static inline int shim_wprintf(WCHAR *d, size_t n, int num) {
	if (!d || n < 6) return -1;
	const char pre[] = "midi";
	size_t i = 0;
	while (pre[i] && i < n - 2) { d[i] = (WCHAR)(unsigned char)pre[i]; i++; }
	if (num >= 0 && num <= 9) d[i++] = (WCHAR)('0' + num);
	else if (num >= 10 && i < n - 2) { d[i++] = (WCHAR)('0' + num / 10); d[i++] = (WCHAR)('0' + num % 10); }
	d[i] = 0;
	return (int)i;
}

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

