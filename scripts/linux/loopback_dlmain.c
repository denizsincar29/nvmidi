// loopback_dlmain.c - a loader shim for the loopback driver's DllMain.
//
// The driver is one translation unit of windows code: ModMessage, its
// driver-life dispatch, the publish thread and DllMain all live in
// nvmidi_loopback.c. To run DllMain, this file defines the windows names it
// uses low enough to compile and includes the source. What it does *not*
// provide is stubbed, not faked: OpenDriver answers zero and sets a real
// error, so a DllMain that depends on winmm having loaded it fails here
// loudly rather than succeeding for the wrong reason.
//
// Two modes, one binary each:
//
//   default     dlopen, let DllMain run, print its return, dlclose. Answers
//               "does it come back" with nothing else in the picture.
//   HOLD_OPEN   dlopen and do not dlclose: the image stays mapped and the
//               thread DllMain started keeps running, which is what a
//               process with a midi device looks like.

#include <windows.h>
#include <stdio.h>
#include <string.h>

// Windows-shaped bookkeeping. The driver stores GetModuleFileNameW's answer
// with the extension stripped; here that would strip the .linux from a
// built name, so the shim keeps the path in wide characters without
// pretending to be a filesystem.
static WCHAR g_module_path[MAX_PATH];
static unsigned int g_thread_count = 0;

// Driver-facing shims. Kept awkward on purpose - see the file comment.
HDRVR OpenDriver(LPCWSTR szDriverName, LPCWSTR szSectionName, LPARAM lParam) {
	(void)szDriverName; (void)szSectionName; (void)lParam;
	SetLastError(ERROR_NOT_SUPPORTED);
	return (HDRVR)0;
}
BOOL CloseDriver(HDRVR hDriver, LPARAM lParam1, LPARAM lParam2) {
	(void)hDriver; (void)lParam1; (void)lParam2;
	return TRUE;
}
DWORD GetModuleFileNameW(HMODULE hModule, LPWSTR lpFilename, DWORD nSize) {
	const char *name = "nvmidi_loopback.linux";
	(void)hModule;
	if (nSize == 0 || !lpFilename) return 0;
	size_t n = strlen(name);
	if (n >= nSize) n = nSize - 1;
	for (size_t i = 0; i < n; i++) lpFilename[i] = (WCHAR)(unsigned char)name[i];
	lpFilename[n] = 0;
	return (DWORD)n;
}
HMODULE GetModuleHandleW(LPCWSTR name) { (void)name; return (HMODULE)&g_module_path; }

// The driver's own writes go through fopen, so what it needs from a thread is
// only a real thread. Each start is counted so the output can say whether the
// publish thread existed at all, independently of whether it wrote anything.
typedef struct { HANDLE h; } ShimThread;

HANDLE CreateThread(LPSECURITY_ATTRIBUTES a, SIZE_T stack, LPTHREAD_START_ROUTINE fn,
                    LPVOID arg, DWORD flags, LPDWORD id) {
	(void)a; (void)stack; (void)flags; (void)id;
	ShimThread *t = (ShimThread *)malloc(sizeof(ShimThread));
	if (!t) return NULL;
	g_thread_count++;
	t->h = (HANDLE)(uintptr_t)fn | ((HANDLE)(uintptr_t)arg);
	// The thread body is run in place, synchronously, on purpose: the linux
	// job's question is whether DllMain's own statements are sound, and a
	// detached clone would move the very work being measured off the frame
	// that reports it. The publish loop never returns, so a thread that
	// would spin forever here is a hang the timeout catches - which is
	// itself a finding about this code.
	if (fn) fn(arg);
	return t->h;
}
BOOL CloseHandle(HANDLE h) { (void)h; return TRUE; }

// The driver installs an unhandled-exception filter from DllMain. On windows
// that is where its crash report goes; there is no equivalent here.
LPTOP_LEVEL_EXCEPTION_FILTER SetUnhandledExceptionFilter(LPTOP_LEVEL_EXCEPTION_FILTER f) {
	(void)f;
	return NULL;
}

// The registry. DllMain writes a Drivers32 slot itself, so on windows that
// runs before winmm has anything to do - which is why it is not what the
// windows fault is about, and why shimming it here is honest rather than
// cheating. What these return says so in the output: the attach runs, the
// slot is not written, and the run prints which of the two it got.
static int g_reg_attempts = 0;
LONG RegOpenKeyExW(HKEY root, LPCWSTR sub, DWORD opt, REGSAM access, PHKEY out) {
	(void)root; (void)sub; (void)opt; (void)access;
	g_reg_attempts++;
	if (out) *out = NULL;
	SetLastError(ERROR_FILE_NOT_FOUND);
	return ERROR_FILE_NOT_FOUND;
}
LONG RegSetValueExW(HKEY key, LPCWSTR name, DWORD reserved, DWORD type,
                    const BYTE *data, DWORD size) {
	(void)key; (void)name; (void)reserved; (void)type; (void)data; (void)size;
	return ERROR_FILE_NOT_FOUND;
}
LONG RegCloseKey(HKEY key) { (void)key; return ERROR_SUCCESS; }

#include "../tools/winmm_loopback/nvmidi_loopback.c"

// DllMain is the function under test, so this file names it once, qualifies
// it, and never re-declares it.
BOOL WINAPI shim_attach(HINSTANCE self, DWORD reason, LPVOID reserved);

int main(int argc, char **argv) {
	const char *path = argc > 1 ? argv[1] : NULL;
	printf("loader: %s\n", argv[0]);
	printf("target: %s\n", path ? path : "<none>");
	printf("threads started by DllMain so far: %u\n", g_thread_count);

	HINSTANCE self = (HINSTANCE)&g_module_path;
	BOOL ok = shim_attach(self, DLL_PROCESS_ATTACH, NULL);

	printf("DllMain returned %s\n", ok ? "TRUE" : "FALSE");
	printf("threads started by DllMain: %u\n", g_thread_count);
	printf("registry slot attempts: %d (the shim answers not-found on purpose;\n"
	       "  the registry is windows-only and a pass here proves nothing about it)\n",
	       g_reg_attempts);

#ifdef HOLD_OPEN
	printf("holding the image open; the thread DllMain started owns the rest\n");
	fflush(stdout);
	for (;;) Sleep(1000);
#else
	shim_attach(self, DLL_PROCESS_DETACH, NULL);
#endif
	return ok ? 0 : 1;
}
