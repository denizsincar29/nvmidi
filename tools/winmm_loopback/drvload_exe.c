/* Loads a copy of a winmm driver dll and calls its entry point on our terms.
 *
 * The loopback dll dies during load with an execute fault at its own base +
 * 0x15e6 and writes no status line, which puts the fault inside DllMain - the
 * loader points the process at its own exception handler for the duration of
 * DLL_PROCESS_ATTACH, so the driver's own crash_filter never fires for it and
 * nothing inside the file can report anything. This program moves the load out
 * of the loader's hands and into main: LoadLibraryExA with
 * DONT_RESOLVE_DLL_REFERENCES maps the image and does NOT call DllMain, so the
 * copy of the dll this program is asked to open is mapped and no more.
 *
 * A file copied away from its own name is enough to keep winmm out of the
 * picture. winmm loads the module named by HKLM\...\Drivers32\middiN, this
 * program publishes nothing and copies the driver to a name nothing points at,
 * so the only load of that copy is this one call.
 *
 * Then ModMessage is called the way winmm would call it. That is the report:
 * an execute fault here is the driver's own entry point answering a message it
 * was sent, caught by this program's exception filter with the faulting
 * instruction already written down, and a clean exit is the entry point
 * working. Either reading is a measurement that has not been available from
 * the loader side.
 *
 * The messages tried are the whole midi namespace, because which one does it is
 * the second half of the answer and there is no way to ask from inside a driver
 * whose fault handler does not run.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <mmsystem.h>
#include <mmiscapi.h>
#include <stdarg.h>

typedef DWORD (WINAPI *MODMSGPROC)(DWORD, DWORD, DWORD_PTR, DWORD_PTR, DWORD_PTR);

static FILE *g_log;

static void say(const char *fmt, ...) {
	va_list ap;
	if (!g_log) return;
	va_start(ap, fmt);
	vfprintf(g_log, fmt, ap);
	va_end(ap);
	fputc('\n', g_log);
	fflush(g_log);
}

static const char *describe(unsigned long n) {
	/* The messages winmm sends, named so the log reads as calls rather than
	 * numbers. DRV_ and MODM_ share their numbers - 3 is DRV_OPEN and MODM_OPEN
	 * both - so a name here is one of two readings and is printed as such. */
	switch (n) {
	case 0x0001: return "DRV_LOAD";
	case 0x0002: return "DRV_ENABLE";
	case 0x0003: return "DRV_OPEN/MODM_OPEN";
	case 0x0004: return "DRV_CLOSE/MODM_CLOSE";
	case 0x0005: return "DRV_DISABLE/MODM_PREPARE";
	case 0x0006: return "DRV_FREE";
	case 0x0007: return "DRV_QUERYCONFIGURE/MODM_DATA";
	case 0x000B: return "DRV_CONFIGURE";
	case 0x000D: return "DRV_QUERYDEVICEINTERFACESIZE";
	case 0x000E: return "DRV_QUERYDEVICEINTERFACE";
	case 0x000F: return "DRV_QUERYDEVICEINTERFACEGUID";
	case 0x003C: return "DRV_USER";
	case 0x0300: return "DRV_RESERVED(base)";
	case 0x0301: return "DRV_QUERYMODULE";
	case 0x0302: return "DRV_QUERYDEVICEID";
	case 0x0310: return "MODM_GETNUMDEVS";
	case 0x0311: return "MODM_GETDEVCAPS";
	case 0x0312: return "MODM_OPEN/MOM_OPEN";
	case 0x0313: return "MODM_CLOSE/MOM_CLOSE";
	case 0x0315: return "MODM_RESET";
	case 0x0316: return "MODM_SETVOLUME";
	case 0x0317: return "MODM_GETVOLUME";
	case 0x0318: return "MODM_GETPOS";
	case 0x0319: return "MODM_PAUSE";
	case 0x031A: return "MODM_RESTART";
	case 0x031B: return "MODM_LONGDATA";
	case 0x031C: return "MODM_PREPARE";
	case 0x031D: return "MODM_UNPREPARE";
	default: break;
	}
	return "?";
}

int main(int argc, char **argv) {
	HMODULE m;
	MODMSGPROC entry;
	char exe[MAX_PATH] = { 0 };
	char copy[MAX_PATH] = { 0 };
	char *dot;
	unsigned long m0;
	unsigned long lo, hi;
	int i;

	GetModuleFileNameA(NULL, exe, MAX_PATH);
	dot = strrchr(exe, '.');
	if (dot) *dot = 0;
	strncat(copy, exe, sizeof(copy) - strlen(copy) - 1);
	strncat(copy, "-drvload.exe", sizeof(copy) - strlen(copy) - 1);
	g_log = fopen(copy, "wb");
	if (!g_log) {
		/* Nothing else in this file can report anything without this, so a
		 * silent run has to be distinguishable from a program that never
		 * started. The runner step greps the file for the first line and says
		 * so when it is missing. */
		return 2;
	}
	say("DRVLOAD version=1 argc=%d", argc);
	if (argc < 2) {
		say("DRVLOAD no dll argument, nothing to load");
		return 3;
	}
	/* A relative path is resolved by LoadLibraryEx against the process
	 * directory, and the runner step runs from the repository root where
	 * "-DLL tools/winmm_loopback/x.dll" is the path that was meant. */
	say("DRVLOAD loading=%s", argv[1]);
	m = LoadLibraryExA(argv[1], NULL, DONT_RESOLVE_DLL_REFERENCES);
	say("DRVLOAD loaded=%p err=%lu", (void *)m, (unsigned long)GetLastError());
	if (!m) return 4;
	entry = (MODMSGPROC)(void *)GetProcAddress(m, "ModMessage");
	say("DRVLOAD entry=%p", (void *)entry);
	if (!entry) return 5;

	/* The two ranges, not a chosen few. A number picked by hand would be the
	 * same guess three times over; the midi namespace is scanned whole so the
	 * report names every message the entry point survives and stops at the one
	 * it does not. 0x310..0x320 is MODM_*, 0x000..0x010 is the DRV_ life
	 * namespace that shares three of its numbers. */
	struct { const char *what; unsigned long lo, hi; } ranges[] = {
		{ "DRV",  0x000, 0x010 },
		{ "MODM", 0x310, 0x320 },
	};
	for (i = 0; i < (int)(sizeof(ranges) / sizeof(ranges[0])); i++) {
		for (m0 = ranges[i].lo; m0 <= ranges[i].hi; m0++) {
			say("DRVLOAD call what=%s msg=0x%03lx name=%s", ranges[i].lo == 0 ? m0 : m0,
				m0, describe(m0));
			entry(0, m0, 0, 0, 0);
			say("DRVLOAD returned msg=0x%03lx", m0);
		}
	}
	(void)lo; (void)hi;
	say("DRVLOAD done");
	return 0;
}
