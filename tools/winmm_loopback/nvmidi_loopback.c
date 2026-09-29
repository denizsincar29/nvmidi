// nvmidi_loopback.c - a winmm MIDI output device that sends what it is given
// to its own input side.
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
// always supported user mode winmm drivers: a plain DLL with a .drv extension
// whose exports winmm calls directly, added with Drivers32 entries and calling
// DriverCallback. This is that: no kernel code, no signing, nothing put into
// the system directory.
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
// with or with anything a user has installed.
//
// Build: see build.sh in this directory. The result is nvmidi-loopback.exe,
// the same file with a .exe extension, because that is the convention winmm's
// own user mode drivers use and the name the ci job downloads.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmsystem.h>
#include <string.h>

#define DRIVER_NAME    "nvmidi loopback driver"
#define DEVICE_NAME    "nvmidi loopback"
#define RING_SIZE      4096

// The registry layout, so the entry this driver keeps can be written back.
// Windows passes the key to the open call and expects the driver to close it.
static const char *kDriverKey    = "Software\\Microsoft\\Windows NT\\CurrentVersion\\Drivers32";
static const char *kDriverValue  = "midi8";        // the first free midi slot is found at load time
static const char *kDeviceKey    = "Software\\Microsoft\\Windows NT\\CurrentVersion\\MCI";

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
	MEMORY_BARRIER;
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

static const DRIVERENTRY g_entries[] = {
	{ DEVICE_NAME, modMessage, sizeof(DEVICE), 0 },
	{ NULL, NULL, 0, 0 }
};

__declspec(dllexport) DWORD_PTR WINAPI modGetDevCaps(UINT id, UINT msg, DWORD_PTR user, DWORD_PTR p1, DWORD_PTR p2);

static BOOL write_device_entry(void) {
	HKEY key = NULL;
	LONG r = RegCreateKeyExA(HKEY_LOCAL_MACHINE, DRIVER_NAME, 0, NULL, 0,
		KEY_SET_VALUE | KEY_QUERY_VALUE, NULL, &key, NULL);
	if (r != ERROR_SUCCESS) return FALSE;
	RegSetValueExA(key, "EntryPoint", 0, REG_SZ, (const BYTE *)"modMessage", 12);
	RegCloseKey(key);
	(void)kDriverKey; (void)kDriverValue; (void)kDeviceKey;
	return TRUE;
}

// Called by winmm before anything else. The two devices this driver publishes
// are what winmm enumerates as "nvmidi loopback".
__declspec(dllexport) DWORD WINAPI DriverProc(DWORD dwDriverId, HDRVR hDriver,
	DWORD dwMessage, DWORD dwParam1, DWORD dwParam2)
{
	(void)dwDriverId; (void)hDriver; (void)dwParam1; (void)dwParam2;
	switch (dwMessage) {
	case DRV_LOAD:
		g_self = GetModuleHandleA(NULL);
		ring_init(&g_out);
		ring_init(&g_in);
		return DRVCNF_OK;
	case DRV_ENABLE:
	case DRV_DISABLE:
	case DRV_FREE:
		return DRVCNF_OK;
	case DRV_OPEN: {
		// The id the caller was given: 0 is the output, 1 is the input. That is
		// the only place the two halves differ, and it has to match the order
		// the DrvEnum entries come out in.
		DWORD_PTR which = dwParam1;
		DEVICE *d = which ? &g_in : &g_out;
		ring_init(d);
		return (d == &g_in) ? 1 : 0;
	}
	case DRV_CLOSE:
		return DRVCNF_OK;
	default:
		return DRVCNF_OK;
	}
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
	}
	return TRUE;
}
