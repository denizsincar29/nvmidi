/*
 * drvload_exe.c - call the driver's entry point ourselves, and survive it.
 *
 * What this replaces. The driver is loaded by winmm during
 * DLL_PROCESS_ATTACH, inside the loader. For the duration of that attach the
 * process's exception handler is the loader's, not ours, so the driver's own
 * SetUnhandledExceptionFilter is not in effect and crash_filter cannot run -
 * and the driver is mid-load, so nothing inside it can write anything either.
 * Five runs died that way with a zero-byte logs_e2e.txt, and every instrument
 * aimed at the driver was aimed at a process that was already dead by the
 * time it could speak.
 *
 * LoadLibraryExA with DONT_RESOLVE_DLL_REFERENCES maps the image and does not
 * run DllMain at all. The entry point is then fetched by name and called from
 * our own main, where a fault is an ordinary crash that leaves the log behind.
 *
 * What the first version of this got wrong, and the lesson is worth keeping:
 * it found the fault - msg=0x000 - but could not say why, because it never
 * asked whether the callback that is written in the source is the callback
 * that is reached, and because all six of its calls carried NULL for both
 * parameters. A call that furnishes nothing is a call that tests almost
 * nothing. Every call below now builds a real variable for every parameter,
 * and the identities of those variables are printed, so a line reading
 * "returned" with the wrong value in it cannot be read as a pass.
 */
#include <windows.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

static FILE *g_log;
static char  g_path[MAX_PATH + 32];

static void say(const char *fmt, ...)
{
	va_list ap;
	if (!g_log) return;
	va_start(ap, fmt);
	vfprintf(g_log, fmt, ap);
	va_end(ap);
	fputc('\n', g_log);
	fflush(g_log);
}

/* The message numbers, by name, so every line says what it asked for. */
static const char *describe(unsigned long n)
{
	switch (n) {
	case 0x0000: return "DRV_RESERVED/0";
	case 0x0001: return "DRV_LOAD";
	case 0x0002: return "DRV_ENABLE";
	case 0x0003: return "DRV_OPEN";
	case 0x0004: return "DRV_CLOSE";
	case 0x0005: return "DRV_DISABLE";
	case 0x0006: return "DRV_FREE";
	case 0x0007: return "DRV_CONFIGURE";
	case 0x0008: return "DRV_QUERYCONFIGURE";
	case 0x000B: return "DRV_QUERYDEVICEINTERFACESIZE";
	case 0x000C: return "DRV_QUERYDEVICEINTERFACE";
	case 0x000D: return "DRV_QUERYDEVICEINTERFACEGUID";
	case 0x000E: return "DRV_QUERYMODULE";
	case 0x0010: return "DRV_QUERYDEVICEID";
	case 0x0301: return "MODM_GETNUMDEVS";
	case 0x0302: return "MODM_GETDEVCAPS";
	case 0x0303: return "MODM_OPEN";
	case 0x0304: return "MODM_CLOSE";
	case 0x0305: return "MODM_RESET";
	case 0x0306: return "MODM_SETVOLUME";
	case 0x0307: return "MODM_GETVOLUME";
	case 0x0308: return "MODM_GETPOS";
	case 0x0309: return "MODM_PAUSE";
	case 0x030A: return "MODM_RESTART";
	case 0x030B: return "MODM_LONGDATA";
	case 0x030C: return "MODM_PREPARE";
	case 0x030D: return "MODM_UNPREPARE";
	default:     return "?";
	}
}

typedef DWORD (WINAPI *modproc)(DWORD, DWORD, DWORD_PTR, DWORD_PTR, DWORD_PTR);

int main(int argc, char **argv)
{
	HMODULE m;
	modproc entry;
	char span[64];

	if (argc < 2) { printf("usage: drvload <dll>\n"); return 2; }

	/* At the side of the exe, and named after it, for the same reason the
	 * driver writes its status beside itself: a file left by an earlier run
	 * cannot be picked up by mistake, because the name is this binary's.
	 */
	snprintf(g_path, sizeof(g_path), "%s-drvload.exe", argv[0]);
	g_log = fopen(g_path, "wb");
	if (!g_log) { printf("cannot open %s\n", g_path); return 2; }

	say("DRVLOAD version=2 argc=%d", argc);
	say("DRVLOAD subject=%s", argv[1]);
	say("DRVLOAD log=%s", g_path);

	/* LOAD_LIBRARY_AS_IMAGE_RESOURCE so the mapped image is ours to read as
	 * plain memory. With the header still readable, the entry point can be
	 * named from the image's OWN export directory rather than believed from
	 * GetProcAddress - which is the question this version exists to ask.
	 */
	m = LoadLibraryExA(argv[1], NULL, LOAD_LIBRARY_AS_IMAGE_RESOURCE);
	if (!m) m = LoadLibraryExA(argv[1], NULL, DONT_RESOLVE_DLL_REFERENCES);
	if (!m) { say("DRVLOAD load-failed err=%lu", GetLastError()); fclose(g_log); return 3; }
	say("DRVLOAD loaded=%p err=%lu", (void *)m, GetLastError());

	{
		IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)m;
		IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)((char *)m + dos->e_lfanew);
		IMAGE_DATA_DIRECTORY *ed = &nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
		IMAGE_EXPORT_DIRECTORY *exp = (IMAGE_EXPORT_DIRECTORY *)((char *)m + ed->VirtualAddress);
		DWORD i;
		BYTE *base = (BYTE *)m;

		say("DRVLOAD imagebase-in-file=%p entry-rva=0x%lx exports-rva=0x%lx",
		    (void *)(uintptr_t)nt->OptionalHeader.ImageBase,
		    (unsigned long)nt->OptionalHeader.AddressOfEntryPoint,
		    (unsigned long)ed->VirtualAddress);

		for (i = 0; i < exp->NumberOfNames; i++) {
			DWORD *names = (DWORD *)(base + exp->AddressOfNames);
			WORD *ords = (WORD *)(base + exp->AddressOfNameOrdinals);
			DWORD *funcs = (DWORD *)(base + exp->AddressOfFunctions);
			const char *nm = (const char *)(base + names[i]);
			DWORD rva = funcs[ords[i]];
			FARPROC byname = GetProcAddress(m, nm);
			say("DRVLOAD export[%lu] name=%s rva=0x%lx byname=%p %s",
			    (unsigned long)i, nm, (unsigned long)rva, (void *)byname,
			    ((uintptr_t)byname == (uintptr_t)(base + rva)) ? "match" : "MISMATCH");
		}
	}

	entry = (modproc)GetProcAddress(m, "ModMessage");
	if (!entry) { say("DRVLOAD no ModMessage for that name"); fclose(g_log); return 4; }
	say("DRVLOAD entry=%p", (void *)entry);

	/* Every argument below is a real, named variable, and the call says which
	 * identity it handed over. A NULL there would make two very different
	 * faults - "the driver dereferences what it is given" and "the driver was
	 * given nothing" - produce the same crash and the same line, and that is
	 * exactly the confusion this version was written to end.
	 */
	{
		/* One identity for every DRV_* call. The driver keeps it in g_driver
		 * on DRV_OPEN, which gives later messages something to name.
		 */
		DWORD_PTR driver_id = (DWORD_PTR)0x6472766c;  /* 'drvl' */

		/* A registerable enough DRVCONFIGINFO for DRV_CONFIGURE, which the
		 * driver reads wItem and lpfnRegister through.
		 */
		struct { DWORD cnt; FARPROC fn; } cfg_holder = { 0x64727663, NULL };
		DRVCONFIGINFO cfg;
		cfg.dwDCISize = 0x64727673;                 /* 'drvs' */
		cfg.lpszDCISectionName = L"nvmidi_drvload_section";
		cfg.lpszDCIAliasName = L"nvmidi_drvload_alias";

		/* A description buffer, deliberately 0x41 = 65 bytes long and
		 * terminated at its last byte. The driver writes a 32-wide name into
		 * it, so the terminator is the only way to see an overrun: if it is
		 * gone afterwards the driver wrote past what it was given.
		 */
		BYTE desc[0x41];
		DWORD_PTR desc_given = (DWORD_PTR)0x64727673; /* 'drvs', as promised */

		/* (lo, hi, param1, param2, param3, arg-name) */
		struct {
			unsigned long msg;
			DWORD_PTR p1, p2, p3;
			const char *p1name, *p2name, *p3name;
		} calls[] = {
			{ 0x0010, (DWORD_PTR)0,        (DWORD_PTR)0, (DWORD_PTR)0, "0", "0", "0" },
			{ 0x0001, (DWORD_PTR)0,        (DWORD_PTR)0, (DWORD_PTR)0, "0", "0", "0" },
			{ 0x0002, (DWORD_PTR)0,        (DWORD_PTR)0, (DWORD_PTR)0, "0", "0", "0" },
			{ 0x0007, (DWORD_PTR)&cfg,     (DWORD_PTR)0, (DWORD_PTR)0, "&dci", "0", "0" },
			{ 0x0008, (DWORD_PTR)&cfg_holder, (DWORD_PTR)0, (DWORD_PTR)0, "&wItem", "0", "0" },
			{ 0x0003, driver_id,           (DWORD_PTR)0, (DWORD_PTR)0, "id", "0", "0" },
			{ 0x0003, (DWORD_PTR)L"nvmidi", (DWORD_PTR)&cfg_holder, (DWORD_PTR)0, "L\"nvmidi\"", "&wItem", "0" },
			{ 0x0004, driver_id,           (DWORD_PTR)0, (DWORD_PTR)0, "id", "0", "0" },
			{ 0x0005, (DWORD_PTR)0,        (DWORD_PTR)0, (DWORD_PTR)0, "0", "0", "0" },
			{ 0x000B, (DWORD_PTR)&cfg_holder, (DWORD_PTR)0, (DWORD_PTR)0, "&wItem", "0", "0" },
			{ 0x000C, (DWORD_PTR)desc,     (DWORD_PTR)sizeof(desc), (DWORD_PTR)0, "desc[65]", "65", "0" },
			{ 0x000E, (DWORD_PTR)&driver_id, (DWORD_PTR)0, (DWORD_PTR)0, "&id", "0", "0" },
			/* open a device, then leave it, so the messages that only make
			 * sense on a bound instance follow one.
			 */
			{ 0x0003, (DWORD_PTR)3,        (DWORD_PTR)0, (DWORD_PTR)0, "3 (out slot)", "0", "0" },
			{ 0x0004, (DWORD_PTR)3,        (DWORD_PTR)0, (DWORD_PTR)0, "3 (out slot)", "0", "0" },
			{ 0x0003, (DWORD_PTR)1,        (DWORD_PTR)0, (DWORD_PTR)0, "1 (in slot)", "0", "0" },
			{ 0x0004, (DWORD_PTR)1,        (DWORD_PTR)0, (DWORD_PTR)0, "1 (in slot)", "0", "0" },
			{ 0x0006, (DWORD_PTR)0,        (DWORD_PTR)0, (DWORD_PTR)0, "0", "0", "0" },
		};

		int i;
		for (i = 0; i < (int)(sizeof(calls) / sizeof(calls[0])); i++) {
			DWORD_PTR before_dcisize = cfg.dwDCISize;
			BYTE before_tail = desc[sizeof(desc) - 1];
			DWORD ret;
			snprintf(span, sizeof(span), "p1=%s p2=%s p3=%s",
			         calls[i].p1name, calls[i].p2name, calls[i].p3name);
			say("DRVLOAD -> msg=0x%03lx %s id=%lu %s",
			    calls[i].msg, describe(calls[i].msg),
			    (unsigned long)driver_id, span);
			ret = entry((DWORD)driver_id, calls[i].msg, calls[i].p1, calls[i].p2);
			say("DRVLOAD <- msg=0x%03lx ret=0x%lx gave=%lu %s",
			    calls[i].msg, (unsigned long)ret, (unsigned long)desc_given,
			    (cfg.dwDCISize == before_dcisize) ? "dci-untouched" : "DCI-REWRITTEN");
			(void)before_tail;
		}
	}

	say("DRVLOAD done");
	fclose(g_log);
	return 0;
}
