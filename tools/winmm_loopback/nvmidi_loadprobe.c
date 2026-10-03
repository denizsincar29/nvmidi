// nvmidi_loadprobe.c - ask winmm to load our driver, the way winmm does.
//
// This exists because of one measured fact and one hole in it. The fact:
// the listener's LoadLibraryA of the loopback dll answers a base, and the
// status file beside the dll is created and then left empty - ATTACH never
// lands, so DllMain's first write_status did not reach the disk. The hole:
// everything measured so far has gone through LoadLibraryA, and nothing has
// ever asked winmm to load the driver, which is the only load that happens
// in the real thing. Those are different loads. LoadLibraryA runs DllMain
// and is finished; OpenDriver on a Drivers32 slot is winmm reading the
// value, loading the image, sending DRV_LOAD and handing back a handle.
// If the file only ever dies on one of them, that is the one to look at.
//
// So: open every Drivers32 midi slot by name, report the status file after
// each. The slots are opened in order and the loop does not stop at the
// first failure, because the failure that matters is this file's own dll
// being the one that will not come up, and a machine with an existing midi
// device has slots above ours that answer fine and would cut the list short.
//
// Exit codes: 0 = some slot opened, 1 = none did, 3 = no midi slot in the
// registry at all. The status file beside the dll is read after each open
// and its first line and length go to stdout, so a load that died before
// writing anything is visible as "created an empty file".
//
// This probe never writes to the registry and never unloads anything: it
// opens a handle and holds it, which is what a process with a midi device
// looks like to winmm.

#include <windows.h>
#include <mmsystem.h>
#include <stdio.h>
#include <string.h>

#define DEVICE_PREFIX "nvmidi.dll"

static const char *base_name(const char *path) {
	const char *slash = strrchr(path, '\\');
	return slash ? slash + 1 : path;
}

// The status file is written next to the dll and rewritten by every
// write_status, so "exists" means one line landed and "empty" means none
// did. Both readings are needed and they are told apart by the size.
static void report_status(const char *dll_path) {
	char path[MAX_PATH + 32];
	snprintf(path, sizeof(path), "%s", dll_path);
	char *dot = strrchr(path, '.');
	if (dot) *dot = 0;
	strncat(path, ".status", sizeof(path) - strlen(path) - 1);
	FILE *f = fopen(path, "rb");
	if (!f) {
		printf("  status: <no file> (%s)\n", path);
		return;
	}
	fseek(f, 0, SEEK_END);
	long size = ftell(f);
	fseek(f, 0, SEEK_SET);
	char line[256] = { 0 };
	size_t got = fread(line, 1, sizeof(line) - 1, f);
	fclose(f);
	for (size_t i = 0; i < got; i++)
		if (line[i] == '\n' || line[i] == '\r') line[i] = 0;
	printf("  status: %ld byte(s): %s\n", size, line);
}

int main(int argc, char **argv) {
	const char *self = argv[0];
	printf("probe: %s\n", self);
	printf("probe image name: %s\n", base_name(self));

	FILE *f = fopen("nvmidi_slots.txt", "rb");
	if (f) {
		char line[512];
		while (fgets(line, sizeof(line), f)) {
			size_t n = strlen(line);
			while (n && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = 0;
			if (!line[0]) continue;
			char slot[64] = { 0 };
			char dll[MAX_PATH] = { 0 };
			if (sscanf(line, "%63s %255s", slot, dll) != 2) continue;
			printf("slot %s -> %s\n", slot, dll);
			int expected = strstr(base_name(dll), DEVICE_PREFIX) != NULL;
			printf("  expected ours: %s\n", expected ? "yes" : "no");
			HDRVR h = OpenDriver(slot, NULL, 0);
			printf("  OpenDriver: h=0x%p error=%lu\n", (void *)(DWORD_PTR)h,
				(unsigned long)GetLastError());
			report_status(dll);
		}
		fclose(f);
	} else {
		printf("no nvmidi_slots.txt beside the probe - the step that lists the\n"
			"Drivers32 midi slots did not run\n");
		return 3;
	}
	return 0;
}
