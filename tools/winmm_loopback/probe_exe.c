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

int main(int argc, char **argv) {
	(void)argc; (void)argv;
	/* Pulled in here, after the dll named by the environment has attached.
	 * The order this program is testing is that the dll was up first. */
	/* The mapper is opened rather than a device index, so winmm has to ask
	 * every registered output driver to describe itself. That ask is the
	 * device-list question with one number for an answer. */
	HMIDIOUT out = NULL;
	MMRESULT r = midiOutOpen(&out, MIDI_MAPPER, 0, 0, CALLBACK_NULL);
	printf("probe_exe outs=%u ins=%u mapper_open=%u\n",
		(unsigned)midiOutGetNumDevs(), (unsigned)midiInGetNumDevs(),
		(unsigned)r);
	if (r == MMSYSERR_NOERROR && out) midiOutClose(out);
	return 0;
}
