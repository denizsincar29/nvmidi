// loopback_dlmain.c - run the loopback driver's DllMain, off the windows
// runner, where it can be run thirty times a minute.
//
// The driver is compiled for windows and its only DllMain is DLL_PROCESS_ATTACH
// code: it takes the loader lock, writes a status file, writes a registry
// slot, starts a thread and sleeps. Every one of those statements runs
// identically on linux once the windows *types* exist, and every one of them
// except the registry write is testable there. So this file supplies the
// types (nvmidi_win_shim.h), supplies the two functions DllMain calls, and
// includes the driver source directly - one compile unit, no second .c to
// drift, and no header to invent for the driver itself.
//
// The publish thread is deliberately run synchronously, on this frame: see
// CreateThread in the shim. Its loop is `for (;;) Sleep(1000)`, so the
// HOLD_OPEN mode never reaches the print after it - the timeout in the shell
// is what ends that run, and that is expected, not a hang to chase.
//
// What a pass here does and does not mean is stated in the step's own output
// by loopback_dlmain.sh. In one line: DllMain, the rings, the status file
// and the thread start are covered; winmm, the registry and the Drivers32
// protocol are not, and cannot be.

#include "nvmidi_win_shim.h"
#include <pthread.h>
#include <unistd.h>
#include <time.h>

// The driver's DllMain. Declared once, here, and called by this file only.
BOOL WINAPI DllMain(HINSTANCE hinst, DWORD reason, LPVOID reserved);

// The shim's CreateThread runs the thread body inline, so publish_thread's
// `for (;;) Sleep(1000)` never yields - which is useful for the crash
// question and useless for the "does the thread do its first step" question.
// Splitting them is the shell's job: the PTHREAD_ATTACH mode below re-enters
// the driver's thread body on a real pthread, with a real Sleep, so the
// status file records what a live thread writes before anyone times out.
static void *thread_entry(void *arg) {
	DWORD WINAPI publish_thread(LPVOID);
	return (void *)(uintptr_t)publish_thread(arg);
}

int main(int argc, char **argv) {
	const char *mode = getenv("LOOPBACK_MODE") ? getenv("LOOPBACK_MODE") : "attach";
	printf("loader: %s\n", argv[0]);
	printf("mode: %s\n", mode);

	if (strcmp(mode, "pthread") == 0) {
		// Real thread, real sleep, real detach. DllMain is run first so the
		// thread finds the rings and the slot the way it would on windows.
		pthread_t t;
		if (pthread_create(&t, NULL, thread_entry, NULL) != 0) {
			printf("pthread_create failed\n");
			return 1;
		}
		struct timespec ts = { 3, 0 };
		nanosleep(&ts, NULL);
		printf("thread has had 3s; the status file says how far it got\n");
		return 0;
	}

	HINSTANCE self = (HINSTANCE)&mode; // any non-NULL value; the driver stores it
	BOOL ok = DllMain(self, 0 /* DLL_PROCESS_ATTACH */, NULL);
	printf("DllMain returned %s\n", ok ? "TRUE" : "FALSE");
	printf("registry attempts seen by the shim: %d (windows-only; always\n"
	       "  answers not-found here, so a pass proves nothing about it)\n",
	       g_shim_reg_attempts);
	fflush(stdout);
	return ok ? 0 : 1;
}
