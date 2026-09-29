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


class MIDIINCAPS(ctypes.Structure):
    _fields_ = [
        ("wMid", ctypes.c_ushort),
        ("wPid", ctypes.c_ushort),
        ("vDriverVersion", ctypes.c_uint),
        ("szPname", ctypes.c_wchar * MAXPNAMELEN),
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


def list_inputs():
    winmm = ctypes.WinDLL("winmm")
    n = winmm.midiInGetNumDevs()
    names = []
    for i in range(n):
        caps = MIDIINCAPS()
        if winmm.midiInGetDevCapsW(i, ctypes.byref(caps), ctypes.sizeof(caps)) == 0:
            names.append((i, caps.szPname))
    return names


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
    ctypes.WinDLL(path)
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
    return False, "the driver loaded but wrote no status within %.0fs" % timeout


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
              " [--driver <path to nvmidi-loopback.exe>]", file=sys.stderr)
        return 2
    # The driver flag is parsed out by hand rather than with argparse: this
    # file is read by people debugging a red build, and the two positional
    # arguments are easier to see than a usage block.
    driver = ""
    rest = []
    i = 1
    while i < len(argv):
        if argv[i] == "--driver":
            driver = argv[i + 1] if i + 1 < len(argv) else ""
            i += 2
        else:
            rest.append(argv[i])
            i += 1
    if not rest:
        print("usage: e2e_listen.py <port name substring> [deadline seconds]"
              " [--driver <path>]", file=sys.stderr)
        return 2
    wanted = rest[0].lower()
    deadline = float(rest[1]) if len(rest) > 1 else 60.0

    if driver:
        ok, why = load_driver(driver)
        if not ok:
            print("DRIVER_LOAD_FAILED %s" % why, file=sys.stderr)
            print("the listener has no input port of the plugin's to open, so it cannot"
                  " say anything about whether a message crossed a process boundary"
                  " in this run.", file=sys.stderr)
            return 1
        print("DRIVER_LOADED")

    inputs = list_inputs()
    print("winmm lists %d midi input device(s):" % len(inputs))
    for i, name in inputs:
        print("  %d %s" % (i, name))
    # With the driver loaded the name it publishes is the whole story, so the
    # job passes the substring it chose; without one, any match will do.
    matches = [(i, n) for i, n in inputs if wanted in n.lower()]
    if not matches:
        print("no midi input device matches %r" % rest[0], file=sys.stderr)
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
