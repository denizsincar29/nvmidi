#!/usr/bin/env python3
"""Listen on a windows midi input and log, with timestamps, what arrives.

Why this exists
---------------

The plugin can open a port and say it did. A stubbed backend says the same
thing without a port ever existing, and a plugin that writes into a port it
opened wrongly loses the bytes somewhere inside winmm with nothing to show for
it. Neither of those can fake a message that arrives at *another process*.

So the sending half of the end-to-end test needs a second process, and on
windows it is this one: it opens a midi input port, waits for the plugin to
play into it, and prints every message it receives with the moment it arrived.
The assertion is then a comparison between two logs - the notes the script
says it played, and the notes this file saw - and neither log can be produced
alone.

Why the loopback route, and why it is loaded here
-------------------------------------------------

Windows has no midi *input* port for a program to open and no way to publish
one; the midi api is a list of devices the system owns, and a program can only
attach to those. (`midiInOpen` takes a device id and nothing else - there is
no `midiInCreateVirtual`, which is the same absence the plugin documents.)
Two processes therefore meet through a loopback driver: one of them writes to
a port the driver owns, the driver copies every byte into the matching input
port, and this file opens that.

The driver is `tools/winmm_loopback/nvmidi_loopback.c`, built by the ci job and
passed in with `--driver`. It is loaded *here*, in this process, because that
is the only place it can be loaded at all: a winmm driver is a dll that winmm
loads into whatever process opens the device, so a driver loaded in one
process owns a port only for that process's lifetime and publishes nothing to
anyone else. An earlier version of this test tried to install the driver in a
separate step and have this file open it across processes; it could not have
worked, and the reason is the model, not a bug in the attempt.

Loading it here is what keeps the test honest. This process owns the device, so
the device is real to it; the plugin is started as its own process by the job
before this file runs, sees the port as an ordinary port of the machine, opens
it and plays. What arrives here has therefore crossed a process boundary, and
that is the one thing a stubbed backend cannot fake.

If `--driver` is omitted, the file falls back to opening any input port whose
name matches, which is how it runs on a machine that already has a loopback
driver installed.

Writing takes the same route in the other direction, for the self test: a
message this process emits to the loopback driver's output comes back to it.
That proves the route is live before the plugin is involved at all, so a
failure later is the plugin's and not the runner's.

Two devices, one of them ours
-----------------------------

The driver publishes a midi output and a midi input, and loading it and then
listing the machine gives both - measured on a runner: `outputs_after=2
inputs_after=1`, the second output being this file's. The input is opened here
without a question; the output is where the plugin will write.

The plugin is a second process, and a midi device belongs to the process that
owns the driver: the plugin's own `midiOutGetNumDevs` therefore cannot see the
port this file made. Its port list is one entry shorter than this file's, and
the plugin can only name what it can see. So the job is given a name to hand
the script and the script is told which entry in *its* list that name means -
`--index` - rather than left to match on a string that does not exist on its
side. Without that, the script picks the first port in its own shorter list,
which on a runner with no sound device is the unopenable GS Wavetable entry,
and the failure reads as a plugin bug when it is a fact about two processes.

The same ownership is why `midiInOpen` and `midiOutOpen` both behave here: the
driver installs itself into the midi slots and answers the input side too.

What the exit code means
------------------------

  0  the port opened and at least one message arrived
  1  the port could not be opened, or nothing arrived before the deadline
  2  the self test failed: the loopback route itself is not carrying midi

The messages themselves are on stdout, one per line, in a format the ci job
greps. Nothing here needs installing: ctypes is in the standard library and
winmm is part of windows.
"""

import ctypes
import os
import sys
import threading
import time

# --- the parts of the winmm midi api that this needs -----------------------
#
# Hand copied from mmsystem.h rather than taken from a python binding. pywin32
# is a large dependency for a test whose subject is a different library, and
# the four calls used here are stable api that has not changed since windows 3.

MIDIIN_MMSYSERR_NOERROR = 0
CALLBACK_FUNCTION = 0x00030000
CALLBACK_NULL = 0x00000000
MIDI_IO_STATUS = 0x00000020

# A short name keeps the window tiny: winmm copies the name into a fixed
# field, and the spec (MAXPNAMELEN) is 32 characters including the terminator.
MAXPNAMELEN = 32

# What the loopback driver calls itself, used only in input-only mode when the
# sender never published a name to match on. It is the driver's own name and
# not the removed name the script was looking for: that one is the string this
# process could not decode (measured: `midiOutGetDevCapsW` hands back one byte
# per character on this runner, so a six character name reads as "M").
LOOPBACK_INPUT_HINT = "nvmidi"


class MIDIINCAPS(ctypes.Structure):
    _fields_ = [
        ("wMid", ctypes.c_ushort),
        ("wPid", ctypes.c_ushort),
        ("vDriverVersion", ctypes.c_uint),
        # c_char and not c_wchar, and that is the whole reason this structure
        # is written out by hand. The narrow midiInGetDevCapsW entry point is
        # the one that exists - there is no narrow sibling - and it fills the
        # field from the driver's 8 bit string, one byte per character. ctypes
        # reading those bytes as UTF-16 does not fail and does not truncate:
        # it keeps the first byte and takes the following NUL as the end, so a
        # driver named "nvmidi loopback" arrives here as "n". Measured, and the
        # reason the driver's name is now short and ascii-only.
        ("szPname", ctypes.c_char * MAXPNAMELEN),
        ("dwSupport", ctypes.c_uint),
    ]


class MIDIOUTCAPS(ctypes.Structure):
    _fields_ = [
        ("wMid", ctypes.c_ushort),
        ("wPid", ctypes.c_ushort),
        ("vDriverVersion", ctypes.c_uint),
        # c_char for the same reason as MIDIINCAPS above.
        ("szPname", ctypes.c_char * MAXPNAMELEN),
        ("wTechnology", ctypes.c_ushort),
        ("wVoices", ctypes.c_ushort),
        ("wNotes", ctypes.c_ushort),
        ("wChannelMask", ctypes.c_ushort),
        ("dwSupport", ctypes.c_uint),
    ]


class MIDIHDR(ctypes.Structure):
    _fields_ = [
        ("lpData", ctypes.c_char_p),
        ("dwBufferLength", ctypes.c_uint),
        ("dwBytesRecorded", ctypes.c_uint),
        ("dwUser", ctypes.c_void_p),
        ("dwFlags", ctypes.c_uint),
        ("lpNext", ctypes.c_void_p),
        ("reserved", ctypes.c_void_p),
        ("dwOffset", ctypes.c_uint),
        ("dwReserved", ctypes.c_void_p * 8),
    ]


# --- what the plugin sent, in terms of the bytes ---------------------------
#
# A single midi status byte is enough to name every message class the plugin
# writes, and naming them here rather than printing raw hex is what makes the
# listener's log readable by a person looking at a failed build.
#
# The comparison against the script's own log then does not depend on this
# table: both sides are reduced to (channel, kind, first data byte, second
# data byte) and matched on that.

def describe(status, d1, d2):
    kind = status & 0xF0
    channel = status & 0x0F
    if kind == 0x90:
        return ("note_on" if d2 > 0 else "note_off", channel, d1, d2)
    if kind == 0x80:
        return ("note_off", channel, d1, d2)
    if kind == 0xB0:
        return ("control_change", channel, d1, d2)
    if kind == 0xC0:
        return ("program_change", channel, d1, d2)
    if kind == 0xE0:
        return ("pitch_bend", channel, d1, d2)
    if status == 0xF0:
        return ("sysex", channel, d1, d2)
    return ("other", channel, d1, d2)


class Listener:
    def __init__(self, device_id, want):
        self.device_id = device_id
        self.want = want
        self.handle = ctypes.c_void_p()
        self.messages = []
        self.lock = threading.Lock()
        # The callback is stored on the instance so that the c function pointer
        # it decays into stays alive for as long as winmm holds it. A local
        # would be collected and the driver would call freed memory.
        self.proc = ctypes.WINFUNCTYPE(
            None, ctypes.c_void_p, ctypes.c_uint, ctypes.c_void_p, ctypes.c_void_p,
            ctypes.c_uint, ctypes.c_void_p)(self._on_message)
        self.stream = []

    def _on_message(self, hmo, wMsg, dwInstance, dwParam1, dwParam2):
        # winmm calls this from the thread that opened the port; it is a plain
        # python call in that thread. Nothing here may block, or messages stop
        # being delivered.
        status = dwParam1 & 0xFF
        d1 = (dwParam1 >> 8) & 0xFF
        d2 = (dwParam1 >> 16) & 0xFF
        kind, channel, a, b = describe(status, d1, d2)
        with self.lock:
            self.messages.append((time.time(), kind, channel, a, b))

    def open(self):
        winmm = ctypes.WinDLL("winmm")
        caps = MIDIINCAPS()
        if winmm.midiInGetDevCapsW(self.device_id, ctypes.byref(caps), ctypes.sizeof(caps)) != 0:
            return False, "midiInGetDevCapsW failed for device %d" % self.device_id
        rc = winmm.midiInOpen(ctypes.byref(self.handle), self.device_id, self.proc,
                              0, CALLBACK_FUNCTION)
        if rc != MIDIIN_MMSYSERR_NOERROR:
            return False, "midiInOpen refused device %d (%s): error %d" % (
                self.device_id, caps.szPname, rc)
        rc = winmm.midiInStart(self.handle)
        if rc != MIDIIN_MMSYSERR_NOERROR:
            winmm.midiInClose(self.handle)
            return False, "midiInStart refused device %d: error %d" % (self.device_id, rc)
        return True, caps.szPname

    def close(self):
        winmm = ctypes.WinDLL("winmm")
        if self.handle:
            winmm.midiInStop(self.handle)
            winmm.midiInReset(self.handle)
            winmm.midiInClose(self.handle)
            self.handle = ctypes.c_void_p()

    def drain(self):
        with self.lock:
            out = list(self.messages)
            self.messages.clear()
        return out


def cap_name(raw):
    """The device name as the driver wrote it: bytes up to the first NUL."""
    return raw.split(b"\x00", 1)[0].decode("latin-1")


def list_inputs():
    winmm = ctypes.WinDLL("winmm")
    n = winmm.midiInGetNumDevs()
    names = []
    for i in range(n):
        caps = MIDIINCAPS()
        if winmm.midiInGetDevCapsW(i, ctypes.byref(caps), ctypes.sizeof(caps)) == 0:
            names.append((i, cap_name(caps.szPname)))
    return names


def list_outputs():
    """The midi outputs, the same way and for the same reason as list_inputs."""
    winmm = ctypes.WinDLL("winmm")
    n = winmm.midiOutGetNumDevs()
    names = []
    for i in range(n):
        caps = MIDIOUTCAPS()
        if winmm.midiOutGetDevCapsW(i, ctypes.byref(caps), ctypes.sizeof(caps)) == 0:
            names.append((i, cap_name(caps.szPname)))
    return names


def driver_field(text, key):
    """One field out of the driver's status file: slot=, outputs=, inputs=."""
    for word in text.replace("\n", " ").split():
        if word.startswith(key + "="):
            return word.split("=", 1)[1]
    return None


def load_driver(path, timeout=20.0):
    """Load the loopback driver into this process and wait for it to publish.

    Returns (ok, reason). The driver registers itself under a Drivers32 slot
    and opens its own device from a thread it starts at load time, then writes
    a status file next to the dll. Waiting for that file rather than sleeping a
    fixed time is what makes the numbers below the driver's own account of what
    it did, not this script's guess: a device that failed to open says so in
    that file instead of looking like a port that simply never appeared.
    """
    status_path = path + ".status"
    # A stale file from an earlier step would be read as this load's result.
    if os.path.exists(status_path):
        os.remove(status_path)
    print("DRIVER_LOADING %s" % path)
    # LoadLibraryA and not ctypes.WinDLL. WinDLL calls LoadLibraryExW with
    # LOAD_WITH_ALTERED_SEARCH_PATH, which makes windows look for the dll's own
    # dependencies beside it - and on this runner that is the directory the
    # plugin's build left 50590 bytes of driver named nvmidi.dll in, next to a
    # libwinpthread-1.dll built against a different shared runtime than the one
    # this python was started with. LoadLibraryA takes the ordinary search order
    # instead, called through ctypes with the result restype set to c_void_p:
    # the default restype is c_int, which truncates the handle to 32 bits on a
    # 64 bit runner and hands everything downstream a base address that was
    # never a real one.
    kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel32.LoadLibraryA.restype = ctypes.c_void_p
    kernel32.LoadLibraryA.argtypes = [ctypes.c_char_p]
    module_base = kernel32.LoadLibraryA(path.encode("mbcs"))
    if not module_base:
        err = ctypes.get_last_error()
        # The dispatch in DllMain is not obeyed - the loader calls DllMain for
        # DLL_PROCESS_ATTACH regardless - so a resolve failure elsewhere in the
        # file would still start the publish thread before LoadLibraryA returns
        # and the wait below would still succeed. A return of 0 is therefore a
        # loader fault and not the driver declining to publish.
        print("DRIVER_LOAD_RC 0 (LoadLibraryA failed, error %d)" % err)
    else:
        print("DRIVER_LOAD_RC base=0x%x" % module_base)
    deadline = time.time() + timeout
    while time.time() < deadline:
        if os.path.exists(status_path):
            with open(status_path, "r", errors="replace") as f:
                text = f.read()
            print("DRIVER_STATUS %s" % text.strip().replace("\n", " | "))
            if "LOOPBACK_STATUS=ok" in text:
                return True, text
            return False, "the driver loaded but did not open its device: %s" % text.strip()
        time.sleep(0.25)
    return False, ("the driver loaded but wrote no status within %.0fs"
                   " (status file %s, module base %r)"
                   % (timeout, "present" if os.path.exists(status_path) else "absent",
                      module_base))


def self_test(device_id):
    """Prove the loopback route carries midi, using only winmm."""
    winmm = ctypes.WinDLL("winmm")
    handle = ctypes.c_void_p()
    rc = winmm.midiOutOpen(ctypes.byref(handle), device_id, 0, 0, CALLBACK_NULL)
    if rc != 0:
        return False, "midiOutOpen refused device %d while self testing: error %d" % (device_id, rc)
    try:
        # 0x90 = note on, channel 0, note 0, velocity 0 - the quietest note
        # there is, sent only to prove the wire, and released immediately.
        for _ in range(3):
            winmm.midiOutShortMsg(handle, 0x00000090)
            time.sleep(0.05)
        winmm.midiOutShortMsg(handle, 0x00000080)
    finally:
        winmm.midiOutClose(handle)
    return True, ""


def main(argv):
    # The port to listen on, by name, read out of the script's log by the
    # caller. Substring match, case insensitive, because winmm's names carry
    # vendor spelling and a runner's device order is not guaranteed.
    if len(argv) < 2:
        print("usage: e2e_listen.py <port name substring> [deadline seconds]"
              " [--driver <path to nvmidi-loopback.dll>]", file=sys.stderr)
        return 2
    # The driver flag is parsed out by hand rather than with argparse: this
    # file is read by people debugging a red build, and the two positional
    # arguments are easier to see than a usage block.
    driver = ""
    index_arg = ""
    inputs_only = False
    rest = []
    i = 1
    while i < len(argv):
        if argv[i] == "--driver":
            driver = argv[i + 1] if i + 1 < len(argv) else ""
            i += 2
        elif argv[i] == "--index":
            index_arg = argv[i + 1] if i + 1 < len(argv) else ""
            i += 2
        elif argv[i] == "--inputs-only":
            # Listen without a port name. Used when the sender never got far
            # enough to name one - its open failed, so there is no name and no
            # E2E_PORT line - but the driver is still worth loading and the two
            # lists are still worth printing. The alternative is to skip the
            # listener step entirely, which is what the earlier runs did, and
            # then nothing is known about the listener half at all.
            inputs_only = True
            i += 1
        else:
            rest.append(argv[i])
            i += 1
    if not rest and not inputs_only:
        print("usage: e2e_listen.py [<port name substring>] [deadline seconds]"
              " [--driver <path>] [--index <n>] [--inputs-only]", file=sys.stderr)
        return 2
    wanted = rest[0].lower() if rest else ""
    deadline = float(rest[1]) if len(rest) > 1 else 60.0
    # The index the plugin's own process enumerated its port under, when the
    # job could read it out of the script's log. A flag and not only an
    # environment variable, because the environment is inherited by every
    # process the step starts and a stale NVGT_PORT_INDEX from an earlier step
    # would be read here as if this run had produced it. -1 means "match on
    # the name instead".
    index = -1
    if index_arg:
        try:
            index = int(index_arg)
        except ValueError:
            print("--index %r is not a number" % index_arg, file=sys.stderr)
            return 2
    elif os.environ.get("NVGT_PORT_INDEX"):
        index = int(os.environ["NVGT_PORT_INDEX"])

    driver_text = ""
    if driver:
        ok, why = load_driver(driver)
        if not ok:
            print("DRIVER_LOAD_FAILED %s" % why, file=sys.stderr)
            print("the listener has no input port of the plugin's to open, so it cannot"
                  " say anything about whether a message crossed a process boundary"
                  " in this run.", file=sys.stderr)
            return 1
        driver_text = why
        print("DRIVER_LOADED")
        print("DRIVER_ADDED outputs_before=%s outputs_after=%d inputs_before=%s inputs_after=%d"
              % (driver_field(driver_text, "outputs_before"), 0,
                 driver_field(driver_text, "inputs_before"), 0))

    # The output list as well as the input one, because the index the script
    # was told is an index into *outputs* and this file has to translate it
    # here, where both lists are visible. The two lists do not run in step on
    # a driver that publishes both halves, so the index cannot be carried
    # across as-is: it is used to read a name off the output list and the name
    # is then looked up among the inputs.
    outputs = list_outputs()
    print("winmm lists %d midi output device(s):" % len(outputs))
    for i, name in outputs:
        print("  %d %s" % (i, name))
    inputs = list_inputs()
    print("winmm lists %d midi input device(s):" % len(inputs))
    for i, name in inputs:
        print("  %d %s" % (i, name))

    matches = []
    if 0 <= index < len(outputs):
        # The script's index names an output; the same name on the input side
        # is what there is to listen on. Exact match first, substring second:
        # two drivers shortened to the same 31 characters would collide, and a
        # substring that happens to match the wrong one would be worse than
        # saying so.
        named = outputs[index][1]
        print("NVGT_PORT_INDEX %d is %r on the output side" % (index, named))
        matches = [(i, n) for i, n in inputs if n == named]
        if not matches:
            matches = [(i, n) for i, n in inputs if named.lower() in n.lower()]
        if not matches:
            print("the output at index %d (%r) has no input side of the same name,"
                  " so there is nothing here to listen on" % (index, named), file=sys.stderr)
            return 1
    elif wanted != "":
        matches = [(i, n) for i, n in inputs if wanted in n.lower()]
    else:
        # Input-only mode: no name was ever published, so the only entry worth
        # opening is the one the driver we just loaded added. When the driver
        # did not load there is nothing to open either, and the message below
        # says so rather than reporting a plugin fault for a missing port.
        matches = [(i, n) for i, n in inputs if LOOPBACK_INPUT_HINT in n.lower()]
    if not matches:
        print("no midi input device matches %r" % (wanted if wanted else LOOPBACK_INPUT_HINT),
              file=sys.stderr)
        print("a loopback driver is what puts a port on the input side;", file=sys.stderr)
        print("without one there is nothing here to listen on.", file=sys.stderr)
        return 1
    device_id, device_name = matches[0]
    print("DRIVER_LOADED_PORTS %d matches, using %d %s" % (len(matches), device_id, device_name))
    print("listening on %d %s" % (device_id, device_name))

    listener = Listener(device_id, wanted)
    ok, why = listener.open()
    if not ok:
        print(why, file=sys.stderr)
        return 1
    print("OPENED %s" % why)

    # The route check, before any claim about the plugin: this process sends
    # to the output side of the same loopback and reads it back here. If that
    # does not work, no absence of messages afterwards means anything.
    print("SELFTEST sending three bytes to the same driver's output and waiting for them back")
    ok, why = self_test(device_id)
    if ok:
        time.sleep(1.0)
        back = listener.drain()
        if not back:
            print("SELFTEST_FAILED the loopback driver did not return the bytes it was given,"
                  " so a message that never arrives later cannot be read as the plugin's fault",
                  file=sys.stderr)
            listener.close()
            return 2
        print("SELFTEST_OK heard %d message(s) back" % len(back))
    else:
        print("SELFTEST_FAILED %s" % why, file=sys.stderr)
        listener.close()
        return 2

    # Input-only mode ends here, and that is the whole point of the mode: there
    # is no name, so nothing is coming, and the value of the run is the load
    # above and the two lists. Waiting the deadline out would only hold the
    # step open. Measured on run 36573157386: this mode was given 5 seconds and
    # the step still took 25, because the driver status never appeared and the
    # twenty second wait for it ran first - and the listener's failure then
    # skipped every step after it, so the sender's own log was never read at
    # all. A diagnostic that hides the thing it was added to explain is worse
    # than no diagnostic.
    if inputs_only:
        listener.close()
        print("INPUTS_ONLY done: the route is proven by SELFTEST_OK and no port"
              " name was published, so there is nothing to wait for")
        return 0

    # The wait. The script under test is already playing by the time the ci job
    # starts this process - it launches the script first and finds the port
    # name in the script's log - so the deadline only has to cover a runner
    # that has been busy for a while, not the whole run.
    start = time.time()
    # The messages are dropped as they are counted rather than held: the
    # scroller is unbounded and a stuck sender would otherwise grow it without
    # limit while this process sleeps.
    while time.time() - start < deadline:
        for t, kind, channel, a, b in listener.drain():
            print("HEARD %.3f %s ch%d %d %d" % (t - start, kind, channel, a, b))
            sys.stdout.flush()

    for t, kind, channel, a, b in listener.drain():
        print("HEARD %.3f %s ch%d %d %d" % (t - start, kind, channel, a, b))

    listener.close()
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
