// nvmidi_loopback.c - a winmm MIDI output device that sends what it is given
// to its own input side, published to winmm without an installer.
//
// Why this exists
// ---------------
// The windows half of the e2e has to observe a message leaving the process.
// winmm has no virtual ports, so the plugin cannot publish one, and the two
// vendor loopback drivers (LoopBe1, loopMIDI) are Inno setups that need an
// interactive desktop the hosted runner does not have - measured, both of them,
// in this job's own log, and written up in .github/workflows/windows.yml.
//
// A driver that is never installed has none of those problems. Windows has
// always supported user mode winmm drivers: a dll whose DriverProc winmm calls
// directly, added with Drivers32 entries and calling DriverCallback. This is
// that: no kernel code, no signing, nothing put into the system directory.
//
// How it is started
// -----------------
// There is no installer, but there is also no magic: winmm learns about a
// driver from HKEY_LOCAL_MACHINE\SOFTWARE\Microsoft\Windows NT\
// CurrentVersion\Drivers32, one value per midi slot (midi, midi1, ... midi9),
// each naming a dll and its entry point. So this file is both the driver and
// its own installer: DllMain starts a thread that writes the entry and calls
// OpenDriver on it. winmm then loads the image, asks it for its device
// interfaces and exposes both halves as real midiOutGetNumDevs /
// midiInGetNumDevs devices, in process.
//
// That thread is also what keeps the device alive: winmm's own reference is
// released when the last handle closes, so the loop at the end of it is the
// lifetime of the port, not an afterthought. The caller is told it worked by
// a line rewritten into a status file, which the ci job reads back.
//
// What it does
// ------------
// One output device (the side the plugin opens) and one input device (the side
// the listener opens). Every MOM_ short message the output receives is handed
// straight to the input driver, which stores it in a ring the caller drains
// with midiInAddBuffer. Loopback, at the winmm level, in one process.
//
// The device name is "nvmidi loopback" and the driver's own name is
// "nvmidi loopback driver", so neither collides with the synth windows ships
// with or with anything a user has installed. The slot picked is the first
// unused midiN, so a machine that already has a loopback keeps it.
//
// Build: see build.sh in this directory. The result is nvmidi-loopback.exe,
// the same file with a .exe extension, because that is the convention winmm's
// own user mode drivers use and the name the ci job downloads.

// The driver interface is declared by these two headers and by nothing else:
// mmddk.h carries the DRV_/MODM_/MIDM_ message numbers and DRIVERENTRY, and
// mmsystem.h carries DriverCallback. It is not a declaration to write by hand
// - the first version of this file did, and the compiler rejected three of the
// names it invented. Including the header is what makes the rest measured.
#include <windows.h>
#include <mmsystem.h>
#include <mmddk.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#define DRIVER_NAME    "nvmidi loopback driver"
#define DEVICE_NAME    "nvmidi loopback"
#define RING_SIZE      4096

// The short-name slot pair the registry entry yields, so a caller can open the
// port by name later without reopening this dll.
#define SLOT_OUT       "nvmidi loopback out"
#define SLOT_IN        "nvmidi loopback in"

// The ring is touched from whichever thread calls into the driver and read from
// whichever thread reopens it. A full barrier is what MSDN names; the portable
// spelling of it is a compiler barrier, since the store order here is what has
// to hold.
#define MEMORY_BARRIER __sync_synchronize()

// Callback kinds, as DriverCallback wants them. Values from the driver
// interface, not guessed:
//   DCB_MIDIIN  = 0x00000000
//   DCB_TYPEMASK= 0x0000000F
// DCB_NULL is what a device that was never armed is told, so that is what the
// unarmed case uses.
#define DCB_MIDIIN   0x00000000L
#define DCB_TYPEMASK 0x0000000FL

typedef struct {
	UINT		id;
	DWORD		flags;
	DRVCALLBACK	callback;
	DWORD_PTR	instance;
	HANDLE		lock;
	// The messages already handed to the output side, waiting for whoever
	// reads the input side to arm a buffer and take them.
	BYTE		ring[RING_SIZE];
	DWORD		head;
	DWORD		tail;
	DWORD		armed;
} DEVICE;

static DEVICE g_out;
static DEVICE g_in;
static HMODULE g_self;

static void ring_init(DEVICE *d) {
	d->head = d->tail = 0;
	d->armed = 0;
}

// One short message, three bytes at most, then the reader takes it with the
// same DWORDS winmm hands out: the status byte, then the data bytes, then -1.
static BOOL ring_put(DEVICE *d, DWORD packed) {
	DWORD used = (d->head - d->tail + RING_SIZE) % RING_SIZE;
	if (used + 4 >= RING_SIZE) return FALSE;      // full: the message is dropped
	__sync_synchronize();
	memcpy(&d->ring[d->head], &packed, 4);
	d->head = (d->head + 4) % RING_SIZE;
	return TRUE;
}

static BOOL ring_get(DEVICE *d, DWORD *packed) {
	if (d->head == d->tail) return FALSE;
	memcpy(packed, &d->ring[d->tail], 4);
	d->tail = (d->tail + 4) % RING_SIZE;
	return TRUE;
}

// The input side is told there is something to read. Without a callback there
// is nowhere to tell.
static void notify_input(void) {
	if (g_in.callback) {
		DriverCallback(g_in.callback, DCB_TYPEMASK & DCB_MIDIIN,
			(HDRVR)g_self, MIM_DATA, g_in.instance, 0, 0);
	}
}

// ---- output side: the plugin opens this one ---------------------------------

// dwParam1 is what the plugin sent; the whole point of the device is to pass it
// on, so the packed bytes go into the ring untouched.
static DWORD modMessage(UINT id, UINT msg, DWORD_PTR user, DWORD_PTR p1, DWORD_PTR p2) {
	(void)id; (void)user; (void)p2;
	switch (msg) {
	case MODM_DATA:
		ring_put(&g_out, (DWORD)p1);
		ring_put(&g_in, (DWORD)p1);
		notify_input();
		return MMSYSERR_NOERROR;
	case MODM_LONGDATA:
	case MODM_PREPARE:
	case MODM_UNPREPARE:
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

// ---- input side: the listener opens this one --------------------------------

// The listener arms buffers here; whatever the output side has put in the ring
// is copied into them now and its own callbacks are raised.
static DWORD midMessage(UINT id, UINT msg, DWORD_PTR user, DWORD_PTR p1, DWORD_PTR p2) {
	(void)id; (void)user; (void)p1; (void)p2;
	switch (msg) {
	case MIDM_OPEN:
		ring_init(&g_in);
		return MMSYSERR_NOERROR;
	case MIDM_CLOSE:
		return MMSYSERR_NOERROR;
	case MIDM_ADDBUFFER: {
		MIDIHDR *hdr = (MIDIHDR *)p2;
		if (!hdr) return MMSYSERR_INVALPARAM;
		DWORD packed;
		DWORD wrote = 0;
		BYTE *buf = (BYTE *)hdr->lpData;
		while (wrote + sizeof(DWORD) <= hdr->dwBufferLength && ring_get(&g_in, &packed)) {
			memcpy(buf + wrote, &packed, sizeof(DWORD));
			wrote += sizeof(DWORD);
		}
		hdr->dwBytesRecorded = wrote;
		if (g_in.callback) {
			DriverCallback(g_in.callback, DCB_TYPEMASK & DCB_MIDIIN,
				(HDRVR)g_self, MIM_LONGDATA, g_in.instance, (DWORD_PTR)hdr, 0);
		}
		return MMSYSERR_NOERROR;
	}
	default:
		return MMSYSERR_NOTSUPPORTED;
	}
}

// ---- what winmm calls to find us --------------------------------------------

// Two devices under one entry point. The table is what winmm enumerates, and
// the index a caller gets back is the slot it asked for: 0 is the output side,
// 1 is the input side. Only one DRIVERENTRY exists because DRIVERENTRY names
// one entry point - the two halves are distinguished by that index, which is
// what the first version of this file got wrong.
static const DRIVERENTRY g_entries[] = {
	{ DEVICE_NAME, modMessage, sizeof(DEVICE), 0 },
	{ NULL, NULL, 0, 0 }
};

// The interface guids winmm asks for, one per half, so midiOutGetNumDevs and
// midiInGetNumDevs each see exactly one port.
static const GUID g_out_guid = { 0x6d313532, 0x0000, 0x0000, { 0x00, 0x00, 0x6e, 0x76, 0x6d, 0x6f, 0x75, 0x74 } };
static const GUID g_in_guid  = { 0x6d313532, 0x0000, 0x0000, { 0x00, 0x00, 0x6e, 0x76, 0x6d, 0x69, 0x6e, 0x00 } };

// The handle winmm gave the dll when its entry was opened. Held for as long as
// the process lives, because dropping it is what would take the device back out
// of the machine's list.
static HDRVR g_driver = NULL;
static char g_slot[32];

// What winmm asks a driver when it is enumerating it. The two halves answer
// with their own guid and their own display name, which is how one entry point
// becomes one output port and one input port.
static DWORD get_dev_caps(DWORD id, UINT msg, DWORD_PTR user, DWORD_PTR p1, DWORD_PTR p2) {
	(void)user;
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
	case DRV_QUERYDEVICENAME:
	case DRV_QUERYDEVICENAMESIZE:
		// A caller that gets this far is already looking at the only driver
		// entry on the machine that carries this name, so the name is enough.
		(void)p1; (void)p2;
		return MMSYSERR_NOERROR;
	default:
		return MMSYSERR_NOTSUPPORTED;
	}
}

// Called by winmm before anything else, and again for every open. One entry
// point answers for both halves because that is the shape winmm's driver model
// has: the index in the call is the half.
__declspec(dllexport) DWORD WINAPI modMessage(DWORD dwDriverId, HDRVR hDriver,
	DWORD dwMessage, DWORD_PTR dwParam1, DWORD_PTR dwParam2)
{
	(void)dwParam1; (void)dwParam2;
	switch (dwMessage) {
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
		// Two shapes arrive here. winmm's own open of the drivers32 entry comes
		// with the slot name, and that handle is the one to hold. An open of a
		// device by index comes with the index, and resetting that half's ring
		// is what makes a second open start clean rather than inheriting the
		// previous caller's messages.
		if (dwParam1 && *(const char *)dwParam1) {
			g_driver = hDriver;
			return DRVCNF_OK;
		}
		ring_init((dwParam2 == 1) ? &g_in : &g_out);
		return DRVCNF_OK;
	case DRV_CLOSE:
		return DRVCNF_OK;
	case DRV_QUERYDEVICEINTERFACESIZE:
	case DRV_QUERYDEVICEINTERFACE:
	case DRV_QUERYDEVICENAME:
	case DRV_QUERYDEVICENAMESIZE:
		return get_dev_caps((DWORD)dwDriverId, dwMessage, 0, dwParam1, dwParam2);
	default:
		return DRVCNF_OK;
	}
}

// The device index arrives as dwDriverId on the interface calls; zero means the
// output half. This is the second definition winmm wants, with the id in the
// same position, and it exists so an older caller that asks for caps through
// the driver rather than through the entry point still gets an answer.
__declspec(dllexport) DWORD WINAPI modGetDevCaps(DWORD_PTR id, UINT msg, DWORD_PTR user, DWORD_PTR p1, DWORD_PTR p2) {
	return get_dev_caps((DWORD)id, msg, user, p1, p2);
}

// ---- becoming a device ------------------------------------------------------

// The slot winmm will read this dll back out of: the first of midi..midi9 that
// nothing is using. Writing a slot another driver owns would take that driver
// away from the machine, so an occupied one is skipped rather than replaced.
static BOOL write_driver_entry(char *slot, size_t slot_len, char **previous) {
	char value[8];
	*previous = NULL;
	for (int i = 0; i < 10; i++) {
		snprintf(value, sizeof(value), i == 0 ? "midi" : "midi%d", i);
		char path[MAX_PATH + 2] = { 0 };
		DWORD got = MAX_PATH + 2;
		HKEY key = NULL;
		if (RegOpenKeyExA(HKEY_LOCAL_MACHINE,
				"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Drivers32",
				0, KEY_QUERY_VALUE | KEY_SET_VALUE, &key) != ERROR_SUCCESS)
			return FALSE;
		DWORD type = 0;
		LONG r = RegQueryValueExA(key, value, NULL, &type, (BYTE *)path, &got);
		if (r == ERROR_SUCCESS && path[0]) {
			// Taken by something else. Leave it alone.
			RegCloseKey(key);
			continue;
		}
		char self[MAX_PATH + 2] = { 0 };
		GetModuleFileNameA(g_self, self, MAX_PATH);
		LONG w = RegSetValueExA(key, value, 0, REG_SZ, (const BYTE *)self, (DWORD)strlen(self) + 1);
		RegCloseKey(key);
		if (w != ERROR_SUCCESS) return FALSE;
		snprintf(slot, slot_len, "%s", value);
		return TRUE;
	}
	return FALSE;
}

// The name the ci job reads back. A file next to the exe, rewritten on each
// start so a second run of the job cannot read the first run's verdict.
static void write_status(const char *status) {
	char path[MAX_PATH + 32] = { 0 };
	GetModuleFileNameA(g_self, path, MAX_PATH);
	char *dot = strrchr(path, '.');
	if (dot) *dot = 0;
	strncat(path, ".status", MAX_PATH + 31 - strlen(path));
	FILE *f = fopen(path, "wb");
	if (!f) return;
	fputs(status, f);
	fclose(f);
}

// Started from DllMain. Loader lock is held there, so everything that touches
// the registry or another module waits until this thread has it free.
static DWORD WINAPI publish_thread(LPVOID unused) {
	(void)unused;
	char slot[32] = { 0 };
	char *previous = NULL;
	if (!write_driver_entry(slot, sizeof(slot), &previous)) {
		write_status("LOOPBACK_STATUS=registry-failed");
		return 0;
	}
	snprintf(g_slot, sizeof(g_slot), "%s", slot);
	// winmm loads the dll fresh from the registry, which is a second image of
	// this same file in the same process. OpenDriver is what starts it.
	HDRVR h = OpenDriverA(slot, NULL, 0);
	if (!h) {
		write_status("LOOPBACK_STATUS=open-failed");
		return 0;
	}
	UINT outs = midiOutGetNumDevs();
	UINT ins = midiInGetNumDevs();
	char status[192];
	snprintf(status, sizeof(status),
		"LOOPBACK_STATUS=ok slot=%s outputs=%u inputs=%u", slot, outs, ins);
	write_status(status);
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
		HANDLE t = CreateThread(NULL, 0, publish_thread, NULL, 0, NULL);
		if (t) CloseHandle(t);
	}
	return TRUE;
}
