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

BOOL WINAPI DllMain(HINSTANCE hinst, DWORD reason, LPVOID reserved) {
	(void)hinst;
	(void)reserved;
	if (reason == DLL_PROCESS_ATTACH) {
		GetEnvironmentVariableA("NVPROBE_DLL", g_path, MAX_PATH);
	}
	if (reason == DLL_PROCESS_ATTACH && g_path[0]) {
		/* LoadLibraryExA with no flags, so the search is the ordinary one and
		 * the loader does not go looking for dependencies beside the file.
		 * The path is given in full, so the order does not matter here. */
			HMODULE m = LoadLibraryExA(g_path, NULL, 0);
		if (!m) {
			/* The failure goes to a file because this process may have no
			 * console attached - it is started from a workflow step, and a
			 * printf here would be lost. */

			FILE *f = fopen("probe_exe_load_failed.txt", "ab");
			if (f) {
				fprintf(f, "LoadLibraryExA failed for %s error %lu\n",
					g_path, (unsigned long)GetLastError());
				fclose(f);
			}
		}
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
