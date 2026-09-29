/* Is winmm's device list built when the loaded module attaches, or when
 * winmm loads?
 *
 * The loopback driver publishes its Drivers32 slot from its own DllMain and
 * then asks OpenDriver to start it, and OpenDriver has refused with the slot
 * present and error 0 on every run. Two readings survive that: winmm cannot
 * use the entry at all, or winmm read the entry list before the entry existed.
 * A DllMain cannot tell them apart, because by the time any module of ours is
 * attaching, the entry is either already there or already missed - whoever
 * loads us has decided the order.
 *
 * So make the order an input. This file is a second copy of the driver with
 * one difference: it publishes the same slot at attach time, and then reports
 * what winmm answers - the device count, and OpenDriver's result - into the
 * file the workflow reads. Built twice, once named for each module that loads
 * it, the same code answers the question from two different places:
 *
 *   - as python.exe's DLLs\winmm.dll: this code runs when some *other* module
 *     in the process first pulls winmm in. If the list is built at winmm load
 *     time - whenever that is - the entry written here is too late by
 *     definition, and OpenDriver still fails.
 *   - as a dll loaded by our own executable's DllMain: the entry exists on
 *     disk before winmm is ever pulled in. If OpenDriver then succeeds, the
 *     list is built on demand and everything so far was a timing problem.
 *
 * Nothing here is the plugin. It is a probe that borrows the driver's
 * publication code so the answer comes from winmm itself rather than from a
 * reading of its documentation.
 */
#include <windows.h>
#include <stdio.h>
#include <string.h>

#define DEVICE_NAME "nvmidi"
#define STATUS_SUFFIX ".status"

static HMODULE g_self;

/* The same publication the driver does, trimmed to what the question needs:
 * find an unused midi slot, point it at this file, and remember its name.
 * Deliberately not shared as a header with the driver - if this probe grows a
 * bug the driver must not inherit it, and the two are read side by side when
 * the result comes back. */
static BOOL publish(WCHAR *slot, size_t slot_len) {
	for (int i = 0; i < 10; i++) {
		WCHAR value[8];
		WCHAR self[MAX_PATH + 2] = { 0 };
		DWORD got = sizeof(self);
		HKEY key = NULL;
		if (i == 0) wcscpy(value, L"midi");
		else _snwprintf(value, 8, L"midi%d", i);
		if (RegOpenKeyExW(HKEY_LOCAL_MACHINE,
				L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Drivers32",
				0, KEY_QUERY_VALUE | KEY_SET_VALUE, &key) != ERROR_SUCCESS)
			return FALSE;
		LONG r = RegQueryValueExW(key, value, NULL, NULL, (BYTE *)self, &got);
		if (r == ERROR_SUCCESS && self[0]) {
			RegCloseKey(key);
			continue;
		}
		GetModuleFileNameW(g_self, self, MAX_PATH);
		LONG w = RegSetValueExW(key, value, 0, REG_SZ, (const BYTE *)self,
			(DWORD)((wcslen(self) + 1) * sizeof(WCHAR)));
		RegCloseKey(key);
		if (w != ERROR_SUCCESS) return FALSE;
		wcsncpy(slot, value, slot_len - 1);
		return TRUE;
	}
	return FALSE;
}

/* Opens MIDIMAPPER rather than a name or an index. The mapper asks every
 * registered output driver to describe itself, so whether it finds this file
 * is the device-list question stated in one number, and it needs no
 * cooperation from any entry this file owns. */
static void report(const char *where) {
	char path[MAX_PATH + 32] = { 0 };
	char host[MAX_PATH] = { 0 };
	char line[512];
	WCHAR slot[32] = { 0 };
	HDRVR h;
	DWORD err;
	FILE *f;

	/* Fixed path, not the running module's own directory. The log file used to
	 * be <this module>.status, and on run 36588463233 both cases reported the
	 * same directory, D:\a\_temp\play - which is where the runner copy of this
	 * file lives, not where python lives. python's own directory is the one
	 * whose winmm is being replaced, and the whole question is whether that
	 * replacement loads at all, so the answer has to land somewhere neither
	 * interpreter nor loader can decide. Measured: run 36589358498, no file at
	 * C:\late_midi_host.txt after the loaded-as-winmm run, which reads the same
	 * as the load never happening. */
	f = fopen("C:\\late_midi_host.txt", "ab");
	if (!f) return;

	/* Who is asking. On the attach run the process is python.exe, so the
	 * directory is ...\Python313. On the OpenDriver run the process is this
	 * file's own module and the directory is wherever the runner put it. Those
	 * are two different directories on the runner, and only the log kept them
	 * apart - the probe filename alone said the same thing twice. */
	GetModuleFileNameA(g_self, path, MAX_PATH);
	GetModuleFileNameA(NULL, host, MAX_PATH);
	fprintf(f, "HOST where=%s host=%s self=%s\n", where, host, path);

	BOOL pub = publish(slot, 32);
	UINT outs = midiOutGetNumDevs();
	UINT ins = midiInGetNumDevs();
	h = OpenDriver(slot, NULL, 0);
	err = GetLastError();
	snprintf(line, sizeof(line),
		"LATE_PROBE where=%s published=%d slot=%ls outs=%u ins=%u open=%lld err=%lu\n",
		where, (int)pub, slot, outs, ins, (long long)h, (unsigned long)err);
	fputs(line, f);
	fclose(f);
}

BOOL WINAPI DllMain(HINSTANCE hinst, DWORD reason, LPVOID reserved) {
	(void)reserved;
	if (reason == DLL_PROCESS_ATTACH) {
		g_self = hinst;
		DisableThreadLibraryCalls(hinst);
		/* Nothing else here may load another module: a DllMain that calls
		 * LoadLibrary can deadlock against the loader lock, and the whole
		 * point of this file is that it runs where that lock is held. The
		 * winmm calls below are already loaded by this point in both cases -
		 * one is a forwarder to anything that loads it, the other is linked
		 * into an executable that has it. */
		report("attach");
	}
	return TRUE;
}
