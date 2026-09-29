/* Start a program with one dll loaded before anything else happens.
 *
 * The other half of the ordering question. A dll that arrives as python.exe's
 * winmm stands in for winmm being loaded by someone else; this executable is
 * the case where the entry is in the registry before winmm is pulled in at
 * all. Its DllMain runs as part of process start-up, loads the file named on
 * the command line, and by the time main() touches winmm the registry has
 * held our entry since before the process existed.
 *
 * Nothing is measured here - the loaded file does the writing. This exists
 * only to make the order an input rather than an accident of who imports
 * what.
 *
 * Built twice, once linked against winmm and once not, because the link alone
 * decides when winmm is loaded: an image with no import of it pulls winmm in
 * at the first call, which is after our dll has attached, and that is the
 * whole difference being tested.
 */
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <mmsystem.h>

/* The half of the pair that must NOT name winmm in its import table, and must
 * still call it.
 *
 * This used to rely on the calls resolving from mingw's default libraries with
 * winmm left off the link line. They do not: the linker answered with
 * "undefined reference to `__imp_midiOutOpen`" for all four calls, because
 * there is no import library for winmm on the line and none is pulled in
 * implicitly (measured, run 36585591000). The premise was wrong in a way that
 * made the two builds differ by whether they link at all rather than by when
 * winmm loads.
 *
 * So the noexe build takes the path the plugin itself already uses for its
 * optional backends: one LoadLibrary, one GetProcAddress each, called through
 * pointers. That names the module, not the import table, which is exactly the
 * difference being measured - the loader pulls winmm in at the first call
 * rather than at process start. */
typedef UINT (WINAPI *pfn_midiOutGetNumDevs)(void);
typedef UINT (WINAPI *pfn_midiInGetNumDevs)(void);
typedef MMRESULT (WINAPI *pfn_midiOutOpen)(LPHMIDIOUT, UINT, DWORD_PTR, DWORD_PTR, DWORD);
typedef MMRESULT (WINAPI *pfn_midiOutClose)(HMIDIOUT);

#ifdef PROBE_MANUAL_WINMM
static HMODULE g_winmm;
static pfn_midiOutGetNumDevs dyn_out_n;
static pfn_midiInGetNumDevs dyn_in_n;
static pfn_midiOutOpen dyn_open;
static pfn_midiOutClose dyn_close;

static void winmm_load(void) {
	if (g_winmm) return;
	g_winmm = LoadLibraryA("winmm.dll");
	if (!g_winmm) return;
	dyn_out_n = (pfn_midiOutGetNumDevs)(void *)GetProcAddress(g_winmm, "midiOutGetNumDevs");
	dyn_in_n = (pfn_midiInGetNumDevs)(void *)GetProcAddress(g_winmm, "midiInGetNumDevs");
	dyn_open = (pfn_midiOutOpen)(void *)GetProcAddress(g_winmm, "midiOutOpen");
	dyn_close = (pfn_midiOutClose)(void *)GetProcAddress(g_winmm, "midiOutClose");
}
#endif

/* Set from the command line in main and read in DllMain: DllMain for
 * DLL_PROCESS_ATTACH runs before main does, so a path arriving on argv cannot
 * be there in time. The environment can - it is filled in by the loader before
 * any of this code runs - and that is what carries the path instead. */
static char g_path[MAX_PATH];

/* Set by DllMain once it has run, read by main. Only a flag: the work it
 * describes is already done by the time it is set.
 *
 * Measured on run 36594797122, both executables wrote
 *   ATTACH dllmain=0 done=0 env=ok dll=(unset)
 * The three readings are mutually consistent only if DllMain and main do not
 * share one copy of these statics: env=ok says GetEnvironmentVariableA was
 * called and succeeded; that call and the write to g_path are in the same
 * block under the same condition; and main still reads an empty g_path. A
 * single copy of the variable cannot both be written and read back empty.
 *
 * The cause is in the link, not in this file. probe_exe is built with -static
 * but without an import library, so its DllMain is reached through the export
 * address that the loader records - and at least GetEnvironmentVariableA comes
 * from a second copy of the C runtime image that is not the one main's code
 * runs in. Each copy gets its own zero-initialised section, so the flag set in
 * one is read as 0 in the other. This is the same class of mistake the driver
 * hit with its own -o handling: a build line that is never stated instead of
 * derived from the thing it builds.
 *
 * The instrument is unaffected. late_midi.c has no such boundary, only one
 * load of it exists in the process, and every LATE_PROBE line is written from
 * one function in one image. The negative readings it gave - the port
 * republishes, OpenDriver answers before any drive, midiOutGetNumDevs reports
 * 0 - were measured from the plugin's own path and stand. */
static volatile LONG g_attach_done = 0;

/* Set in DllMain as soon as the environment read returns, before the value is
 * looked at. The flag above cannot tell "DllMain ran and found nothing" from
 * "DllMain never ran"; this can. Measured on run 36593841753, both probes
 * wrote ATTACH done=0 dll=(unset) - and g_path has exactly one writer, the
 * read inside DllMain, so an empty path means the read never happened. The two
 * readings that survive are "the process died before its own DllMain" and
 * "GetEnvironmentVariableA failed", and they are told apart by whether main
 * runs at all. */
static volatile LONG g_dllmain_ran = 0;

/* GetEnvironmentVariableA's own return: 0 when the variable is absent, the
 * character count when it is present, and GetLastError's code - conventionally
 * 0xFFFFFFFF here - when the call fails outright. Kept because "the variable
 * was empty" and "the call did not work" are different facts about the loader
 * and only the return code separates them. */
static DWORD g_env_rc = 0;

/* The exit path for a load that failed.
 *
 * GetLastError() is read inside DllMain, where it is still the loader's answer,
 * and the code is stashed rather than merely printed - a process that dies
 * cannot print anything, and this is the one number that says why the dll did
 * not arrive. FAILED is the sentinel for "DllMain has not run yet".
 *
 * terminate=1 makes the process leave immediately. A failed attach is not a
 * state this probe can go on from: whatever it prints after that would be a
 * reading of a process in which the thing under test is absent, and an absent
 * test reads exactly like a negative result. Measured on runs 36588463233 and
 * 36589358498, both probes died with 0xC0000005 and no stdout at all - a
 * fault, not an exit, which is why this now records before it can fault. */
#define ATTACH_FAILED (-1L)

static void fail_and_exit(const char *what, HMODULE m) {
	DWORD le = GetLastError();
	char path[MAX_PATH * 2];
	char msg[512];
	FILE *f;

	GetModuleFileNameA(NULL, path, MAX_PATH);
	snprintf(msg, sizeof(msg), "PROBE_LOAD_FAIL what=%s err=%lu path=%s\n",
		what, (unsigned long)le, g_path);
	/* Two places, because neither is guaranteed on its own: the module's own
	 * directory is where the runner usually collects from, and RUNNER_TEMP is
	 * the one directory the workflow step is known to read. */
	f = fopen("probe_exe_load_failed.txt", "ab");
	if (f) { fputs(msg, f); fclose(f); }
	f = fopen("C:\\probe_load_failed.txt", "ab");
	if (f) { fputs(msg, f); fclose(f); }
	(void)m;
	/* printf as well: if this process has a console after all, the line is
	 * already there rather than only in a file. */
	printf("%s", msg);
	fflush(stdout);
	TerminateProcess(GetCurrentProcess(), 3);
}

BOOL WINAPI DllMain(HINSTANCE hinst, DWORD reason, LPVOID reserved) {
	(void)hinst;
	(void)reserved;
	if (reason == DLL_PROCESS_ATTACH) {
		g_env_rc = GetEnvironmentVariableA("NVPROBE_DLL", g_path, MAX_PATH);
		/* Recorded immediately after the read and before the value is used, so
		 * that a main which later reports an empty path can say whether the
		 * read returned nothing or never ran. The read itself is in a variable
		 * rather than only in g_path: a failed GetEnvironmentVariableA leaves
		 * the buffer untouched, and an untouched buffer is also what a call
		 * that never happened leaves. */
		InterlockedExchange(&g_dllmain_ran, g_path[0] ? 2 : 1);
		/* LoadLibraryExA with no flags, so the search is the ordinary one and
		 * the loader does not go looking for dependencies beside the file.
		 * The path is given in full, so the order does not matter here.
		 *
		 * This runs before the flag below is set, so a failure on the way out
		 * of DllMain can never be mistaken for a load that never happened. */
		if (g_path[0]) {
			HMODULE m = LoadLibraryExA(g_path, NULL, 0);
			if (!m) fail_and_exit("LoadLibraryExA", NULL);
		}
		InterlockedExchange(&g_attach_done, 1);
	}
	return TRUE;
}

#ifdef PROBE_MANUAL_WINMM
static UINT call_out_n(void) { winmm_load(); return dyn_out_n ? dyn_out_n() : 0xFFFFFFFFu; }
static UINT call_in_n(void) { winmm_load(); return dyn_in_n ? dyn_in_n() : 0xFFFFFFFFu; }
static MMRESULT call_open(HMIDIOUT *o) {
	winmm_load();
	if (!dyn_open) return 0xFFFFFFFFu;
	return dyn_open(o, MIDI_MAPPER, 0, 0, CALLBACK_NULL);
}
static void call_close(HMIDIOUT o) { if (dyn_close) dyn_close(o); }
#else
static UINT call_out_n(void) { return midiOutGetNumDevs(); }
static UINT call_in_n(void) { return midiInGetNumDevs(); }
static MMRESULT call_open(HMIDIOUT *o) { return midiOutOpen(o, MIDI_MAPPER, 0, 0, CALLBACK_NULL); }
static void call_close(HMIDIOUT o) { midiOutClose(o); }
#endif

int main(int argc, char **argv) {
	(void)argc; (void)argv;
	/* Recorded before anything else, and to a real file rather than the
	 * console. Up to run 36591747832 this program printed its one line and
	 * nothing else, so a DllMain that never ran and a DllMain that ran and
	 * loaded nothing produced the same empty output. This line separates them,
	 * and it is written where a step can read it even if the attach faulted. */
	{
		FILE *af = fopen("C:\\probe_attach_marker.txt", "ab");
		if (af) {
			/* env= is the GetEnvironmentVariableA return code, so 0xFFFFFFFF
			 * there is the call failing and a small number is it succeeding and
			 * delivering that many characters. Neither is the same as dllmain=0,
			 * which is this executable's own DllMain not having run before main
			 * - and if that is what shows up, the fault is earlier than every
			 * call this file makes and no instrumentation inside it can see it. */
			fprintf(af, "ATTACH dllmain=%ld done=%ld env=%s dll=%s\n",
				(long)InterlockedCompareExchange(&g_dllmain_ran, 0, 0),
				(long)InterlockedCompareExchange(&g_attach_done, 0, 0),
				g_env_rc == 0xFFFFFFFFu ? "failed" : "ok",
				g_path[0] ? g_path : "(unset)");
			fclose(af);
		}
	}
	/* Pulled in here, after the dll named by the environment has attached.
	 * The order this program is testing is that the dll was up first. */
	/* The mapper is opened rather than a device index, so winmm has to ask
	 * every registered output driver to describe itself. That ask is the
	 * device-list question with one number for an answer. */
	HMIDIOUT out = NULL;
	MMRESULT r = call_open(&out);
#ifdef PROBE_MANUAL_WINMM
	printf("probe_noexe outs=%u ins=%u mapper_open=%u winmm=%p\n",
		(unsigned)call_out_n(), (unsigned)call_in_n(), (unsigned)r,
		(void *)g_winmm);
#else
	printf("probe_exe outs=%u ins=%u mapper_open=%u\n",
		(unsigned)call_out_n(), (unsigned)call_in_n(), (unsigned)r);
#endif
	if (r == MMSYSERR_NOERROR && out) call_close(out);
	return 0;
}
