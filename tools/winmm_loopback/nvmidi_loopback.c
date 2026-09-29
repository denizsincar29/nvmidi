// nvmidi_loopback.c - a winmm MIDI device that sends what its output side is
// given to its own input side, published to winmm without an installer.
//
// Why this exists
// ---------------
// The windows half of the e2e has to observe a message leaving the *process*.
// The plugin can open a port and say so, and a stubbed backend can say the same
// thing without a port existing; neither can fake a message that arrives in a
// second process. winmm has no virtual ports, so the receiving half has to come
// from a driver, and the two vendor loopback drivers (LoopBe1, loopMIDI) are
// Inno setups that need an interactive desktop the hosted runner does not have.
//
// A driver that is never installed has none of those problems. Windows has
// always supported user mode winmm drivers: a dll whose entry point winmm calls
// directly, published with a Drivers32 entry. This is that - no kernel code, no
// signing, nothing put into the system directory, and no setup to hang.
//
// How it is started
// -----------------
// There is no installer, but there is also no magic: winmm learns about a
// driver only from HKEY_LOCAL_MACHINE\SOFTWARE\Microsoft\Windows NT\
// CurrentVersion\Drivers32, one value per midi slot (midi, midi1, ... midi9),
// each naming a dll and its entry point. LoadLibrary alone publishes nothing.
// So this file is both the driver and its own installer: DllMain starts a
// thread that writes the entry and calls OpenDriver on it. winmm then loads the
// image, asks it what it is, and exposes it as a real device.
//
// That thread is also what keeps the device alive: winmm's reference goes away
// when the last handle closes, so the loop at the end of it is the lifetime of
// the port, not an afterthought. The caller is told it worked by a status file,
// which the ci job reads back.
//
// What it does
// ------------
// One output device (the side the plugin opens) and one input device (the side
// the listener opens). Every short message the output receives is written into
// a ring and the input side is told; a listener drains it with midiInAddBuffer.
// Loopback, at the winmm level, in one process.
//
// The device name is "nvmidi loopback", so it collides with neither the synth
// windows ships with nor anything a user has installed. The slot picked is the
// first unused midiN, so a machine that already has a loopback keeps it.
//
// Build: build.sh in this directory. The result is nvmidi-loopback.exe - the
// extension winmm's own user mode drivers use, and the name the ci job wants.
//
// A note on the names and the numbers below
// ------------------------------------------
// Everything here comes out of one of the mingw headers (windows.h, mmsystem.h
// -> mmiscapi.h/mmeapi.h, mmddk.h). Nothing is written by hand, and that is the
// lesson this file was written three times to learn. A hand-invented DCB_MIDIIN,
// a hand-invented DRIVERENTRY, a hand-recalled DCB_TYPEMASK: the compiler
// rejected every one, because mingw's driver headers do not carry them. The
// entry point type is DRIVERMSGPROC (mmiscapi.h:77), the callback kind is
// CALLBACK_FUNCTION (mmsyscom.h:184), and DCB_* belongs to the wavedriver
// protocol and has no midi spelling at all.
//
// Numbers need the same care as names, and for a sharper reason: DRV_ and MODM_
// are separate namespaces that overlap. MODM_OPEN and DRV_OPEN are both 3,
// MODM_CLOSE and DRV_CLOSE are both 4, and MODM_DATA is 7 where DRV_FREE is
// 0x0007. A dispatcher that mixes the two does not merely fail to compile - it
// misreads the messages it does compile for.

#include <windows.h>
#include <mmsystem.h>
#include <mmddk.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

// The name is "nvmidi" on purpose and not something longer. MODM_GETDEVCAPS
// copies it into MIDIOUTCAPS.szPname, which is a fixed 32 character field, and
// the midiout GetDevCaps **W** entry point that the listener and the plugin
// both use puts it there as UTF-16 - not the UTF-8 a wide-name api usually
// means. Measured: a name decoded as UTF-16 by ctypes out of a buffer the
// driver filled with 8-bit bytes does not raise and does not truncate, it comes
// back as the first bytes with a NUL after each, "nvmidi loopback" reading as
// "n". A short ascii-only name is what survives that conversion intact, so the
// port a person sees in the log is the port that is really there.
#define DEVICE_NAME    "nvmidi"
#define RING_SIZE      4096

// The contract between this file and the ci step that starts it: the file next
// to the dll, rewritten on every start, so a later run cannot read an earlier
// run's verdict.
#define STATUS_SUFFIX  ".status"

typedef struct {
	UINT		id;
	DWORD		flags;
	DWORD_PTR	callback;   // the caller's DRVCALLBACK, already as a DWORD_PTR
	DWORD_PTR	instance;
	// The messages already handed to the output side, waiting for whoever reads
	// the input side to arm a buffer and take them. Four bytes per message.
	BYTE		ring[RING_SIZE];
	DWORD		head;
	DWORD		tail;
} DEVICE;

static DEVICE g_out;
static DEVICE g_in;
static HMODULE g_self;
// Written under the loader lock in DllMain, read once by publish_thread after
// the loader has moved on. The write happens-before the thread is created, so
// there is no window in which the thread can see it half done.
static WCHAR g_slot[32];
// How many midi inputs the machine reported before this file published
// anything. The difference after is this driver's own contribution, which is
// the only number that says whether the midi half of the driver is visible -
// midiInGetNumDevs alone cannot tell a device this file added from one a synth
// driver would have reported anyway.
static UINT g_inputs_before;

// Declared above its use because it is defined with the other output helpers,
// well below the dispatcher that calls it. The compiler said so on the runner
// before this line existed: an implicit declaration, then a conflicting type
// where the real one was reached.
static void trace_call(DWORD id, DWORD msg, DWORD_PTR p1);

// The callback kind, as midiOutOpen and midiInOpen name it: CALLBACK_FUNCTION
// means "call this address". It is the only kind that can work here, because
// the caller is winmm itself and not a window or a thread owned by us. The
// mask is what tells the kind apart from CALLBACK_NOSWITCH and the rest.
static DWORD call_kind(DWORD flags) {
	if ((flags & CALLBACK_TYPEMASK) == CALLBACK_FUNCTION) return CALLBACK_FUNCTION;
	return CALLBACK_NULL;
}

static void ring_init(DEVICE *d) {
	d->head = d->tail = 0;
}

// One short message, three bytes at most, packed the way winmm hands it to a
// driver: status byte, data bytes, then -1 for the ones that are not there. The
// order is kept as-is, because whoever reads the input side decodes exactly
// this layout.
static BOOL ring_put(DEVICE *d, DWORD packed) {
	DWORD used = (d->head - d->tail) & (RING_SIZE - 1);
	if (used + 4 >= RING_SIZE) return FALSE;      // full: the message is dropped
	__sync_synchronize();
	memcpy(&d->ring[d->head], &packed, 4);
	d->head = (d->head + 4) & (RING_SIZE - 1);
	return TRUE;
}

static BOOL ring_get(DEVICE *d, DWORD *packed) {
	if (d->head == d->tail) return FALSE;
	memcpy(packed, &d->ring[d->tail], 4);
	d->tail = (d->tail + 4) & (RING_SIZE - 1);
	return TRUE;
}

// The input side is told there is something to read. Without a callback there
// is nowhere to tell. DriverCallback takes the address as a DWORD_PTR - its
// declaration says so, and passing the function pointer unconverted is a
// warning in C and an error in anything stricter.
static void notify_input(void) {
	if (g_in.callback) {
		DriverCallback(g_in.callback, call_kind(g_in.flags),
			(HDRVR)g_self, MIM_DATA, g_in.instance, 0, 0);
	}
}

// ---- the output side: the plugin opens this one -----------------------------

// dwParam1 is what the plugin sent and the whole point of the device is to pass
// it on, so the packed bytes go into the ring untouched.
static DWORD on_output_message(DWORD msg, DWORD_PTR p1, DWORD_PTR p2) {
	(void)p2;
	switch (msg) {
	case MODM_DATA:
		ring_put(&g_out, (DWORD)p1);
		ring_put(&g_in, (DWORD)p1);
		notify_input();
		return MMSYSERR_NOERROR;
	case MODM_GETDEVCAPS: {
		// p1 is the MIDIOUTCAPS winmm built from the registry entry, and only
		// its szPname is ours to write: the field is fixed width, so a shorter
		// name is zero filled and terminated rather than strcpy'd, and
		// widening the bytes in place would run off the end of it.
		MIDIOUTCAPS *caps = (MIDIOUTCAPS *)p1;
		if (!caps) return MMSYSERR_INVALPARAM;
		ZeroMemory(caps->szPname, sizeof(caps->szPname));
		for (int i = 0; i < (int)(sizeof(caps->szPname) / sizeof(WCHAR)) - 1
				&& DEVICE_NAME[i]; i++) {
			caps->szPname[i] = (WCHAR)(unsigned char)DEVICE_NAME[i];
		}
		return MMSYSERR_NOERROR;
	}
	case MODM_LONGDATA:
	case MODM_PREPARE:
	case MODM_UNPREPARE:
	case MODM_RESET:
		// Sysex and buffer bookkeeping: accepted and discarded. A test that
		// needs them will say so by failing on what it expected to hear.
		return MMSYSERR_NOERROR;
	case MODM_OPEN:
		ring_init(&g_out);
		return MMSYSERR_NOERROR;
	case MODM_CLOSE:
		return MMSYSERR_NOERROR;
	default:
		return MMSYSERR_NOTSUPPORTED;
	}
}

// ---- the input side: the listener opens this one ----------------------------

// The listener arms buffers here; whatever the output side has put in the ring
// is copied into one now and the listener's own callback is raised. The MIDIHDR
// arrives in dwParam2 - that is where winmm puts it, and MIDM_ADDBUFFER is the
// only message here that carries one.
static DWORD on_input_message(DWORD msg, DWORD_PTR p1, DWORD_PTR p2) {
	(void)p1;
	switch (msg) {
	case MIDM_OPEN:
		ring_init(&g_in);
		return MMSYSERR_NOERROR;
	case MIDM_GETDEVCAPS: {
		MIDIINCAPS *caps = (MIDIINCAPS *)p1;
		if (!caps) return MMSYSERR_INVALPARAM;
		ZeroMemory(caps->szPname, sizeof(caps->szPname));
		for (int i = 0; i < (int)(sizeof(caps->szPname) / sizeof(WCHAR)) - 1
				&& DEVICE_NAME[i]; i++) {
			caps->szPname[i] = (WCHAR)(unsigned char)DEVICE_NAME[i];
		}
		return MMSYSERR_NOERROR;
	}
	case MIDM_CLOSE:
		return MMSYSERR_NOERROR;
	case MIDM_ADDBUFFER: {
		MIDIHDR *hdr = (MIDIHDR *)p2;
		if (!hdr) return MMSYSERR_INVALPARAM;
		DWORD packed;
		DWORD wrote = 0;
		BYTE *buf = (BYTE *)hdr->lpData;
		// A byte stream, not a list of dwords: the listener's own decoding
		// walks it a byte at a time, so the status byte and its data bytes are
		// written at the widths they arrived in.
		while (wrote + 4 <= hdr->dwBufferLength && ring_get(&g_in, &packed)) {
			buf[wrote++] = (BYTE)(packed & 0xFF);
			buf[wrote++] = (BYTE)((packed >> 8) & 0xFF);
			buf[wrote++] = (BYTE)((packed >> 16) & 0xFF);
			buf[wrote++] = (BYTE)((packed >> 24) & 0xFF);
		}
		hdr->dwBytesRecorded = wrote;
		if (g_in.callback) {
			DriverCallback(g_in.callback, call_kind(g_in.flags),
				(HDRVR)g_self, MIM_LONGDATA, g_in.instance, (DWORD_PTR)hdr, 0);
		}
		return MMSYSERR_NOERROR;
	}
	default:
		return MMSYSERR_NOTSUPPORTED;
	}
}

// ---- what winmm calls to find us --------------------------------------------

// The interface guids winmm asks for, one per half, so the output list and the
// input list each see exactly one port. The id in the call is which half is
// being asked about, so one driver entry becomes two devices without a table.
static const GUID g_out_guid = { 0x6d313532, 0x0000, 0x0000, { 0x00, 0x00, 0x6e, 0x76, 0x6d, 0x6f, 0x75, 0x74 } };
static const GUID g_in_guid  = { 0x6d313532, 0x0000, 0x0000, { 0x00, 0x00, 0x6e, 0x76, 0x6d, 0x69, 0x6e, 0x00 } };

// The handle winmm gave the dll when the registry entry was opened, kept for
// the life of the process because closing it is what would take the device back
// out of the machine's list.
static HDRVR g_driver = NULL;

// What winmm asks when it is enumerating a device, and where the two halves
// answer with their own guid. Only the two messages mingw declares are handled;
// the name query is not among them, and what names the device is the registry
// value that published it.
static DWORD get_dev_caps(DWORD id, UINT msg, DWORD_PTR p1, DWORD_PTR p2) {
	switch (msg) {
	case DRV_QUERYDEVICEINTERFACESIZE:
		*(DWORD_PTR *)p1 = (id >= 1 ? sizeof(g_in_guid) : sizeof(g_out_guid));
		return MMSYSERR_NOERROR;
	case DRV_QUERYDEVICEINTERFACE: {
		size_t want = (id >= 1) ? sizeof(g_in_guid) : sizeof(g_out_guid);
		size_t give = (want < (size_t)p2) ? want : (size_t)p2;
		memcpy((void *)p1, (id >= 1) ? (const void *)&g_in_guid : (const void *)&g_out_guid, give);
		return MMSYSERR_NOERROR;
	}
	default:
		return MMSYSERR_NOTSUPPORTED;
	}
}

// The life of the dll, and the enumeration winmm does before any device is
// open. Separate from the message halves because these numbers are a namespace
// of their own.
//
// This is the distinction that cost a compile: MODM_OPEN is 3 and DRV_OPEN is
// 3, MODM_CLOSE is 4 and DRV_CLOSE is 4, MODM_PREPARE is 5 and DRV_PREPARE is
// that same 5. One table cannot hold both, and the compiler said so with
// duplicate case values on lines 271-274. The blast radius was worse than the
// build: MODM_DATA is 7 and DRV_FREE is 0x0007, so a note arriving from the
// plugin would have been read as a teardown had this shipped as written.
static DWORD driver_life(DWORD msg, DWORD_PTR p1, DWORD_PTR p2) {
	switch (msg) {
	case DRV_LOAD:
		ring_init(&g_out);
		ring_init(&g_in);
		return DRVCNF_OK;
	case DRV_ENABLE:
	case DRV_DISABLE:
		return DRVCNF_OK;
	case DRV_FREE:
		// Only ever reached when the machine is tearing the driver down.
		return DRVCNF_OK;
	case DRV_OPEN:
		// Two shapes arrive here. winmm's own open of the Drivers32 entry comes
		// with the slot name, and that handle is the one to hold - without it
		// the device leaves the machine's list. An open of a device by index
		// comes with the index instead, and which half it is is that index.
		if (p1 && *(const char *)p1) {
			g_driver = (HDRVR)p2;
			return DRVCNF_OK;
		}
		ring_init((p1 == 1) ? &g_in : &g_out);
		return DRVCNF_OK;
	case DRV_CLOSE:
		return DRVCNF_OK;
	default:
		// Not this table's message. Answering DRVCNF_OK to everything it did
		// not recognise - which is what this arm used to do, DRV_OPEN included
		// - is what made OpenDriver fail with error=0.
		//
		// The two tables in this file do not share their numbers. MODM_OPEN is
		// 3, exactly as DRV_OPEN is 3, and driver_message checks this table
		// first. So the open of the device by index never reached
		// on_output_message: it was read here as a driver life message and
		// answered as one, so a device the senders believed they had opened for
		// output was never opened. winmm reported what it saw - a driver that
		// claimed to have handled a message and did not - and it has no error
		// number to offer for that, because nothing it can observe went wrong.
		//
		// DRVCNF_CANCEL is the refusal this protocol has, and a wrong number is
		// still better than a yes. The real repair is the dispatch above this,
		// which has to stop sending the two namespaces to this table in the
		// first place; this arm only makes sure a message that reaches here by
		// some other route is not answered with a success it did not earn.
		return DRVCNF_CANCEL;
	}
}

// Every call winmm makes, in the order winmm makes them: the dll's life first,
// then the enumeration, then whichever half the message belongs to. The halves
// are told apart by what winmm put in the call, and the output side is tried
// first because a driver is opened for output before it is asked about input.
//
// The hard part, and the thing this function got wrong, is that winmm does not
// keep its two namespaces apart for us. MODM_OPEN, MODM_CLOSE and MODM_PREPARE
// are 3, 4 and 5; DRV_OPEN, DRV_CLOSE and DRV_DISABLE are 3, 4 and 5 too. A
// driver is reached through *one* entry point, so the number is all there is
// to go on and no single table can hold both readings.
//
// The rule that resolves it is that the driver life messages carry a driver id
// where the midi ones carry a device handle. winmm passes the id it was given
// for this driver as dwDriverId on every call, and on a DRV_ message the same
// value arrives again in dwParam1 - which is exactly the shape CheckDriverMsg
// tests for in the reference driver, and it is a fact about the caller rather
// than a guess about the number. The two halves are then tried in turn and the
// one that does not own the message answers MMSYSERR_NOTSUPPORTED, which is a
// number winmm will not mistake for a success.
static DWORD WINAPI driver_message(DWORD dwDriverId, HDRVR hDriver,
	DWORD dwMessage, DWORD_PTR dwParam1, DWORD_PTR dwParam2)
{
	(void)hDriver;
	// Written before anything is decided, and that placement is the point.
	// Every failure this file has had arrived as one of two words - open-failed
	// or a port count of one - and both of them are downstream of a decision
	// made here. Whether winmm and midiOutOpen ever reach this function at all
	// is a separate question, and it is the first one that has to be answered:
	// a dispatcher that is never called cannot be debugged by rearranging its
	// arms, and three changes were made to those arms before anyone checked.
	// The trace is appended, so the file holds the whole call sequence in order
	// rather than the last thing that happened.
	trace_call(dwDriverId, dwMessage, dwParam1);
	switch (dwMessage) {
	case DRV_QUERYDEVICEINTERFACESIZE:
	case DRV_QUERYDEVICEINTERFACE:
		return get_dev_caps((DWORD)dwDriverId, dwMessage, dwParam1, dwParam2);
	default:
		break;
	}
	if (dwParam1 == (DWORD_PTR)dwDriverId) {
		switch (dwMessage) {
		case DRV_LOAD:  case DRV_ENABLE:  case DRV_DISABLE:
		case DRV_FREE:  case DRV_OPEN:    case DRV_CLOSE:
			return driver_life(dwMessage, dwParam1, dwParam2);
		default:
			break;
		}
	}
	DWORD r = on_output_message(dwMessage, dwParam1, dwParam2);
	if (r != MMSYSERR_NOTSUPPORTED) return r;
	return on_input_message(dwMessage, dwParam1, dwParam2);
}

// The entry point winmm resolves. It is named ModMessage because that is what
// the Drivers32 protocol names, and typed DRIVERMSGPROC because that is the
// type mmiscapi.h:77 declares for exactly this - a winmm driver entry point.
// The two HDRVR arguments the type wants are dropped, because the handle is
// held in g_driver and nothing here needs it passed in.
__declspec(dllexport) DWORD WINAPI ModMessage(DWORD dwDriverId, DWORD dwMessage,
	DWORD_PTR dwParam1, DWORD_PTR dwParam2, DWORD_PTR dwParam3)
{
	(void)dwParam3;
	return driver_message(dwDriverId, NULL, dwMessage, dwParam1, dwParam2);
}

// ---- becoming a device ------------------------------------------------------

// The status file the ci step polls for. Written next to the dll, which is
// where the step put it, and rewritten on each start.
static void write_status(const char *status) {
	char path[MAX_PATH + 32] = { 0 };
	GetModuleFileNameA(g_self, path, MAX_PATH);
	char *dot = strrchr(path, '.');
	if (dot) *dot = 0;
	strncat(path, STATUS_SUFFIX, sizeof(path) - strlen(path) - 1);
	FILE *f = fopen(path, "wb");
	if (!f) return;
	fputs(status, f);
	fclose(f);
}

// Every call that reaches the entry point, one line each, with the two numbers
// that say who is calling: the driver id winmm passes on every message, and
// dwParam1. GetModuleFileName is asked for the path on each call rather than
// cached, because this runs before anything else in the file and the one thing
// it must not do is depend on state another thread can still be filling in.
//
// The file is opened for append and closed again. That is slow and it is the
// right trade here: a trace that stays open would need a lock, a lock taken
// inside a driver message is a deadlock waiting for a re-entrant call, and this
// path is taken a handful of times per process, not per note.
static void trace_call(DWORD id, DWORD msg, DWORD_PTR p1) {
	char path[MAX_PATH + 32] = { 0 };
	GetModuleFileNameA(g_self, path, MAX_PATH);
	char *dot = strrchr(path, '.');
	if (dot) *dot = 0;
	strncat(path, ".trace.txt", sizeof(path) - strlen(path) - 1);
	FILE *f = fopen(path, "ab");
	if (!f) return;
	fprintf(f, "CALL msg=0x%04lx id=0x%08lx p1=0x%08lx\n",
		(unsigned long)msg, (unsigned long)id, (unsigned long)(DWORD_PTR)p1);
	fclose(f);
}

// The slot winmm will read this dll back out of: the first of midi..midi9 that
// nothing owns. Writing a slot another driver holds would take that driver away
// from the machine, so an occupied one is skipped rather than replaced.
static BOOL write_driver_entry(WCHAR *slot, size_t slot_len) {
	for (int i = 0; i < 10; i++) {
		WCHAR value[8];
		WCHAR self[MAX_PATH + 2] = { 0 };
		DWORD got = sizeof(self);
		HKEY key = NULL;
		if (i == 0) wcscpy(value, L"midi");
		else _snwprintf(value, 8, L"midi%d", i);
		if (RegOpenKeyExW(HKEY_LOCAL_MACHINE,
				L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Drivers32",
				0, KEY_QUERY_VALUE | KEY_SET_VALUE, &key) != ERROR_SUCCESS) {
			// Without this key there is nowhere to publish, and that is worth
			// saying rather than retrying.
			return FALSE;
		}
		DWORD type = 0;
		LONG r = RegQueryValueExW(key, value, NULL, &type, (BYTE *)self, &got);
		if (r == ERROR_SUCCESS && self[0]) {
			RegCloseKey(key);          // taken by something else; leave it alone
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

// Started from DllMain. The loader lock is held there, so everything that
// touches the registry or loads another module waits until this thread has it
// free.

// Says where the thread is when it stops. The last three runs printed a module
// base and then nothing - not registry-failed, not open-failed, not the report
// - which leaves two readings that need different fixes: the thread never runs,
// or it runs and the fault is later in it. A one line mark before anything that
// can fail separates them, and a fault handler turns a dead process into a line
// in a file.
//
// The handler is SetUnhandledExceptionFilter and not __try/__except: mingw-w64
// has no SEH keywords, and a compiler that does not know __try reports it as an
// undeclared identifier and goes on to mangle the rest of the function. This
// filter is plain C, it fires on the exception the process cannot handle, and
// it does not need the frame the fault happened in - which a mingw 64 bit image
// does not carry anyway, since it links unwind info instead of the frame
// pointer chain a walker would follow.
static LONG WINAPI crash_filter(EXCEPTION_POINTERS *ep) {
	char path[MAX_PATH];
	char line[MAX_PATH];
	FILE *f;
	if (!g_self) return EXCEPTION_EXECUTE_HANDLER;
	GetModuleFileNameA(g_self, path, MAX_PATH);
	// The address is the module base plus the offset of the faulting
	// instruction, and the base is already in the listener's log from this same
	// run - so the two subtract, and what is left indexes a disassembly without
	// further arithmetic. Measured on the runner: this fires before the process
	// goes away, so both lines reach the file.
	strcpy(strrchr(path, '.'), ".crash.txt");
	f = fopen(path, "wb");
	if (!f) return EXCEPTION_EXECUTE_HANDLER;
	if (ep && ep->ExceptionRecord) {
		snprintf(line, sizeof(line),
			"LOOPBACK_CRASH=0x%08lx at %p (base %p, code +0x%llx)\n",
			(unsigned long)ep->ExceptionRecord->ExceptionCode,
			ep->ExceptionRecord->ExceptionAddress,
			(void *)g_self,
			(unsigned long long)ep->ExceptionRecord->ExceptionAddress
				- (unsigned long long)g_self);
	} else {
		snprintf(line, sizeof(line), "LOOPBACK_CRASH=no exception record\n");
	}
	fputs(line, f);
	fclose(f);
	return EXCEPTION_EXECUTE_HANDLER;
}


// Started from DllMain. The loader lock is held there, so everything that
// touches the registry or loads another module waits until this thread has it
// free.
static DWORD WINAPI publish_thread(LPVOID unused) {
	(void)unused;
	WCHAR slot[32] = { 0 };
	char report[256];
	// First act of the thread, before anything that can fault. Three runs in a
	// row ended with a module base in the log and not one status line, which
	// leaves two readings - the thread never reached write_status, or it did
	// and something between there and the fopen failed - and they need
	// different fixes. This mark separates them on the very next run: if it
	// appears the thread is alive and the fault is later, if it does not then
	// the thread itself is not surviving the loader lock.
	write_status("LOOPBACK_STATUS=thread-started");
	// The registry entry is published by DllMain now, before winmm is allowed
	// to look, and the slot it took is handed over in g_slot. Re-publishing
	// here would be a second write to the same value for no reason, and the
	// read-back below is what says whether the first one survived.
	if (!g_slot[0]) {
		write_status("LOOPBACK_STATUS=registry-failed");
		return 0;
	}
	wcsncpy(slot, g_slot, 31);
	// winmm loads the dll fresh from the registry - a second image of this same
	// file, in this same process. OpenDriver is what starts it. The name goes
	// over as UTF-16, which is what the declaration asks for by its LPCWSTR.
	g_inputs_before = midiInGetNumDevs();
	HDRVR h = OpenDriver(slot, NULL, 0);
	// -1 is not a failure here. winmm answers a driver that is already open
	// with a handle of -1 rather than opening a second one, and the previous
	// image in this same process still holds the device: the earlier load is
	// the live one and the port it published is the one to use. Measured, from
	// a run where the publish thread ran again after the listener had already
	// loaded the file.
	// The compare is on an integer cast and not on HDRVR(-1): this compiler
	// will not cast a literal into the handle type, and both lines that tried
	// it were a build error rather than a runtime one.
	if (!h && (long long)h != -1) {
		// The error number, not just the verdict. open-failed alone says winmm
		// would not start the driver and nothing about why: a missing registry
		// value, a refused load, and a dll the loader cannot resolve all arrive
		// here as the same word. GetLastError is the only thing that tells them
		// apart, and it has to be read immediately - every call in between is
		// allowed to overwrite it.
		char detail[256];
		snprintf(detail, sizeof(detail),
			"LOOPBACK_STATUS=open-failed slot=%ls error=%lu",
			slot, (unsigned long)GetLastError());
		write_status(detail);
		return 0;
	}
	UINT outs = midiOutGetNumDevs();
	UINT ins = midiInGetNumDevs();
	char slot8[64] = { 0 };
	WideCharToMultiByte(CP_ACP, 0, slot, -1, slot8, sizeof(slot8) - 1, NULL, NULL);
	// A slot whose name a previous run already left behind is read back, not
	// taken: a driver that is still held answers its own name. That is a
	// finding, not a failure - the device is there and usable, which is all
	// the listener needs - and calling it open-failed would send whoever
	// debugging the build after a bug that is not in this file.
	snprintf(report, sizeof(report),
		"LOOPBACK_STATUS=%s slot=%s outputs=%u inputs=%u inputs_before=%u",
		((long long)h == -1) ? "stale" : "ok", slot8, outs, ins, g_inputs_before);
	write_status(report);
	// The handle is held, not closed: this thread is the device's lifetime.
	for (;;) Sleep(1000);
	return 0;
}

BOOL WINAPI DllMain(HINSTANCE hinst, DWORD reason, LPVOID reserved) {
	(void)reserved;
	if (reason == DLL_PROCESS_ATTACH) {
		g_self = hinst;
		DisableThreadLibraryCalls(hinst);
		memset(&g_out, 0, sizeof(g_out));
		memset(&g_in, 0, sizeof(g_in));
		ring_init(&g_out);
		ring_init(&g_in);
		// The filter goes on before the thread starts, because the thread is
		// where a fault is now expected to show up and a handler installed
		// afterwards would miss it.
		SetUnhandledExceptionFilter(crash_filter);
		// Published here, on the loading thread, and not from the thread
		// below. Every failure this file has had was read as "winmm cannot
		// start the driver", and the registry dump says the write itself was
		// never the problem: HKLM\...\Drivers32 had midi1 pointing straight at
		// this dll, because that write is done with the key alone and needs
		// nothing winmm could still be initialising. What needs winmm is the
		// other half - winmm builds its device list once, and a slot that
		// appears after that build is a device that never received DRV_LOAD.
		// Doing the write while the loader is still holding this dll puts it
		// before that build for any module that loads us early, which is the
		// only arrangement in which winmm can find us at all.
		write_driver_entry(g_slot, 32);
		HANDLE t = CreateThread(NULL, 0, publish_thread, NULL, 0, NULL);
		if (t) CloseHandle(t);
	}
	return TRUE;
}
