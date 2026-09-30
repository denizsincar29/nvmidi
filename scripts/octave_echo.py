#!/usr/bin/env python3
"""octave_echo.py - press a key on the instrument, hear it back an octave up.

What this is
------------

The shortest thing that can be both tested and heard. It opens one midi input
and one midi output, and copies every note it receives to the output twelve
semitones higher. Press middle C on the keyboard; the same note comes back as
the C above it. Nothing else is transposed - a controller move is passed
through untouched, so a pedal still pedals and a wheel still bends.

Why an octave and not "middle C, once"
--------------------------------------

The note that comes back was not played by this program. It is a function of
what a person did with their hands, and it is audibly *different* from what
they did - so a single key press answers three questions at once: the input
port carried a real event, the output port carried a real event, and the
program is in the middle of the two. A test that plays a fixed note answers
only the second one, and a test that plays and then asks "did you hear it"
cannot separate a working input from a broken one.

That is why the person is in the loop at all: this process hands bytes to a
driver and knows nothing beyond that. It cannot tell a speaker from a muted
mixer. It says what it sent, and the one witness who can hear says what came
out.

The platforms differ, and this file does not hide it
----------------------------------------------------

Windows has no midi input a program may publish (the api is a list of devices
the system owns), so on a machine with no hardware input there is nothing to
open unless a loopback driver supplies one. The driver in
tools/winmm_loopback/ does: it is loaded into *this* process, which is the only
place winmm will load it, and then it appears in the machine's port list like
any other device. --driver takes its path.

On linux alsa publishes both directions, so a hardware port is openable
directly and --driver is not used.

Counters, not impressions
-------------------------

--seconds N runs unattended and prints a line per event plus a summary. The
summary is what a test asserts on: how many notes arrived, how many went out,
and how many were dropped because the output was not open. A run with
received=0 is a failure about the instrument or the cable, not a pass.

    unreleased: 0        every note that went out came back as a note off

Exit status
-----------

0 when at least one note was echoed (or the run was interrupted after echoing
one), 1 when nothing was ever received, 2 when the setup itself failed - no
port, no driver, no library. Nothing is ever reported as passing on the
strength of having started.

Ctrl+C closes both ports and sends all-notes-off before exiting, so a note
played on the way out is not left sounding.
"""

import argparse
import ctypes
import os
import platform
import sys
import time

# ---------------------------------------------------------------------------
# what one echoed note is
# ---------------------------------------------------------------------------

# The interval, in semitones. A named constant because the whole program is
# this number, and 0 would make it a copy rather than a transposition - which
# would still be a working loop and a useless test: a copy is the same note the
# instrument already made, so a person could not tell a live route from a dead
# one by ear.
OCTAVE = 12

NOTE_OFF = 0x80
NOTE_ON = 0x90
CONTROL_CHANGE = 0xB0

# A transposed note is clamped into 0..127 rather than allowed to wrap. Wrapping
# would turn the top octave of the keyboard into the bottom one, which sounds
# like a bug and is one: the instrument's highest C would come back as its
# lowest. Clamping drops it instead, and the count of what was dropped is
# printed, so the top of the range is a fact in the log rather than a surprise
# in the ear.
MIDI_MIN = 0
MIDI_MAX = 127


def describe(status, data1, data2):
    """One line for one message, in the words a person would use."""
    kind = status & 0xF0
    channel = (status & 0x0F) + 1
    if kind == NOTE_ON and data2 > 0:
        return "note on   key %3d  velocity %3d  channel %d" % (data1, data2, channel)
    if kind == NOTE_OFF or (kind == NOTE_ON and data2 == 0):
        return "note off  key %3d  channel %d" % (data1, channel)
    if kind == CONTROL_CHANGE:
        return "control   cc %d = %3d  channel %d" % (data1, data2, channel)
    return "message   status 0x%02X  data %d %d  channel %d" % (status, data1, data2, channel)


def transpose(status, data1, data2):
    """What to send back, or None when the message is passed through as it is.

    Only note numbers move. Everything else - a pedal, a wheel, a program
    change - is the performer's intent about the instrument rather than a note,
    and shifting those would be a different feature wearing this one's name.
    """
    kind = status & 0xF0
    if kind not in (NOTE_ON, NOTE_OFF):
        return None
    if kind == NOTE_ON and data2 == 0:
        # A note on with velocity 0 is a note off. It must be treated as one, or
        # a keyboard that releases its keys this way leaves every note sounding
        # an octave up for the rest of the session.
        kind = NOTE_OFF
    moved = data1 + OCTAVE
    if not (MIDI_MIN <= moved <= MIDI_MAX):
        return "out-of-range"
    return kind | (status & 0x0F), moved, data2


# ---------------------------------------------------------------------------
# the midi side: one class per platform, because the apis share no shape
# ---------------------------------------------------------------------------

class WinMm:
    """winmm through ctypes. Short messages only, which is what a keyboard sends."""

    def __init__(self):
        self.winmm = ctypes.WinDLL("winmm")
        self.kernel32 = ctypes.WinDLL("kernel32")
        self.loaded_dll = None

    def load_driver(self, path):
        """Load the loopback driver into this process and report what it says.

        LoadLibraryA, not ctypes.WinDLL: WinDLL calls LoadLibraryExW with
        LOAD_WITH_ALTERED_SEARCH_PATH, which makes the driver's own directory its
        search root and leaves it unable to find the libraries beside the python
        that started it. The driver has its own reason to be loaded this way
        (its DllMain publishes itself into Drivers32 and opens its own device
        from a thread), and the listener in scripts/windows/e2e_listen.py
        arrived at the same call for the same reason.
        """
        self.kernel32.LoadLibraryA.restype = ctypes.c_void_p
        self.kernel32.LoadLibraryA.argtypes = [ctypes.c_char_p]
        base = self.kernel32.LoadLibraryA(path.encode("mbcs"))
        if not base:
            err = ctypes.get_last_error()
            return False, "LoadLibraryA failed with error %d" % err
        self.loaded_dll = base
        return True, "loaded"

    def driver_status(self, path, timeout=20.0):
        """Read the status file the driver writes beside itself, or explain its absence."""
        status_path = os.path.splitext(path)[0] + ".status"
        deadline = time.time() + timeout
        while time.time() < deadline:
            if os.path.exists(status_path):
                try:
                    with open(status_path, "r", errors="replace") as handle:
                        return handle.read().strip()
                except OSError as error:
                    return "status file unreadable: %s" % error
            time.sleep(0.2)
        return "the driver loaded but wrote no status within %.0fs" % timeout

    def input_count(self):
        return self.winmm.midiInGetNumDevs()

    def output_count(self):
        return self.winmm.midiOutGetNumDevs()

    def input_name(self, index):
        caps = self._in_caps(index)
        return caps.szPname if caps else "?"

    def output_name(self, index):
        caps = self._out_caps(index)
        return caps.szPname if caps else "?"

    class _InCaps(ctypes.Structure):
        _fields_ = [
            ("wMid", ctypes.c_ushort),
            ("wPid", ctypes.c_ushort),
            ("vDriverVersion", ctypes.c_uint),
            ("szPname", ctypes.c_wchar * 32),
            ("dwFormats", ctypes.c_uint),
            ("wChannels", ctypes.c_ushort),
            ("wReserved1", ctypes.c_ushort),
        ]

    class _OutCaps(ctypes.Structure):
        _fields_ = [
            ("wMid", ctypes.c_ushort),
            ("wPid", ctypes.c_ushort),
            ("vDriverVersion", ctypes.c_uint),
            ("szPname", ctypes.c_wchar * 32),
            ("wChannels", ctypes.c_ushort),
            ("dwSupport", ctypes.c_uint),
        ]

    def _in_caps(self, index):
        caps = self._InCaps()
        midi_in_caps = getattr(self.winmm, "midiInGetDevCapsW", None)
        if midi_in_caps is None:
            return None
        midi_in_caps.argtypes = [ctypes.c_uint, ctypes.POINTER(self._InCaps), ctypes.c_uint]
        if midi_in_caps(index, ctypes.byref(caps), ctypes.sizeof(caps)) != 0:
            return None
        return caps

    def _out_caps(self, index):
        caps = self._OutCaps()
        midi_out_caps = getattr(self.winmm, "midiOutGetDevCapsW", None)
        if midi_out_caps is None:
            return None
        midi_out_caps.argtypes = [ctypes.c_uint, ctypes.POINTER(self._OutCaps), ctypes.c_uint]
        if midi_out_caps(index, ctypes.byref(caps), ctypes.sizeof(caps)) != 0:
            return None
        return caps


class Alsa:
    """alsa through ctypes, the raw sequencer interface.

    Not libasound's sequencer-timer client API - that is a dozen structures deep
    and would be most of this file. The kernel's own /dev/snd/seq takes a small
    fixed set of ioctls and the port list is the raw midi device files, which is
    enough for a program that only wants to move bytes between two of them.
    """

    SNDRV_SEQ_IOCTL_PVERSION = 0x80045100
    SNDRV_SEQ_IOCTL_CLIENT_ID = 0x80045101
    SNDRV_SEQ_IOCTL_CREATE_PORT = 0xC0A85120
    SNDRV_SEQ_IOCTL_DELETE_PORT = 0xC0A85124
    SNDRV_SEQ_IOCTL_SUBSCRIBE_PORT = 0xC0A85121
    SNDRV_SEQ_IOCTL_SET_CLIENT_POOL = 0xC0A85105

    def __init__(self, argv0="octave-echo"):
        self.argv0 = argv0
        self.fd = -1

    def port_names(self, direction):
        """The raw midi devices, which is what a person means by 'the port'."""
        names = []
        root = "/dev/snd"
        prefix = "midiC" if direction == "in" else "midiC"
        try:
            entries = sorted(os.listdir(root))
        except OSError:
            return names
        for entry in entries:
            if entry.startswith(prefix) and entry.endswith("D0"):
                # midiC<card>D0 is the device a keyboard appears as. The
                # direction is a property of the device, not the name: what is
                # openable for reading is an input.
                path = os.path.join(root, entry)
                if direction == "in" and os.access(path, os.R_OK):
                    names.append(path)
                elif direction == "out" and os.access(path, os.W_OK):
                    names.append(path)
        return names


def open_ports_alsa(args):
    """Open /dev/snd/seq and print what is there.

    The honest limit: the raw sequencer path needs a subscription between this
    client and the device port, and doing that properly is alsa-lib's job. This
    reports the devices it sees and exits rather than pretending to have opened
    one - a run that cannot receive must not report a pass.
    """
    root = "/dev/snd"
    if not os.path.isdir(root):
        return None, None, "no /dev/snd on this machine"
    midi = sorted(e for e in os.listdir(root) if e.startswith("midi"))
    if not midi:
        return None, None, "no raw midi devices in /dev/snd"
    print("raw midi devices visible: %s" % ", ".join(midi))
    print("")
    print("This build reaches alsa through /dev/snd/seq directly, and subscribing")
    print("this client to a device port needs alsa-lib. Run scripts/test_nvmidi.py")
    print("--interactive for the alsa path, which goes through the plugin itself.")
    return None, None, "alsa path not wired in this file"


# ---------------------------------------------------------------------------
# the loop
# ---------------------------------------------------------------------------

class Counters:
    def __init__(self):
        self.received = 0
        self.sent = 0
        self.passed = 0
        self.dropped = 0
        self.unreleased = 0
        self.open_notes = 0

    def summary(self):
        return (
            "received=%d sent=%d passed-through=%d dropped=%d unreleased=%d"
            % (self.received, self.sent, self.passed, self.dropped, self.unreleased)
        )


def run_windows(args):
    m = WinMm()
    if args.driver:
        driver = os.path.abspath(args.driver)
        if not os.path.exists(driver):
            print("no driver at %s" % driver)
            return 2
        ok, why = m.load_driver(driver)
        print("driver      %s (%s)" % (driver, why))
        if not ok:
            return 2
        print("driver says %s" % m.driver_status(driver))

    ins = m.input_count()
    outs = m.output_count()
    print("ports       %d input, %d output" % (ins, outs))
    for i in range(ins):
        print("  input  %d: %s" % (i, m.input_name(i)))
    for i in range(outs):
        print("  output %d: %s" % (i, m.output_name(i)))

    in_index = args.input if args.input is not None else 0
    out_index = args.output if args.output is not None else 0
    if ins == 0:
        print("")
        print("no midi input on this machine. On windows none can be created by a")
        print("program, so a loopback driver or a keyboard is required. See")
        print("tools/winmm_loopback/ for the one this project builds.")
        return 2
    if outs == 0:
        print("")
        print("no midi output on this machine, so nothing can be heard.")
        return 2
    if in_index >= ins or out_index >= outs:
        print("input %d or output %d is out of range" % (in_index, out_index))
        return 2

    return pump_winmm(m, in_index, out_index, args)


def pump_winmm(m, in_index, out_index, args):
    """The loop itself: open both, copy, close both. Returns the exit status."""
    winmm = m.winmm
    counters = Counters()
    playing = None  # the note currently sounding on the output, or None

    class MidiHdr(ctypes.Structure):
        pass

    MidiHdr._fields_ = [
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

    CALLBACK_FUNCTION = 0x00030000
    WIM_DATA = 0x3C0
    hdr = MidiHdr()
    buf = ctypes.create_string_buffer(1024)
    hdr.lpData = ctypes.cast(buf, ctypes.c_char_p)
    hdr.dwBufferLength = 1024

    # The callback runs on winmm's own thread, so the queue it appends to is
    # drained by the main loop rather than processed here. The data arrives
    # as dwParam1, packed status|data1<<8|data2<<16.
    queue = []

    def on_data(hmo, wMsg, instance, param1, param2):
        if wMsg == WIM_DATA:
            # The header has to go back before anything else, or winmm stops
            # delivering: the buffer is not a buffer until it is handed over.
            winmm.midiInAddBuffer(handle, ctypes.byref(hdr), ctypes.sizeof(hdr))
            recorded = hdr.dwBytesRecorded
            if recorded:
                raw = buf.raw[:recorded]
                for i in range(0, len(raw) - 2, 3):
                    queue.append((raw[i], raw[i + 1], raw[i + 2]))

    CALLBACK = ctypes.WINFUNCTYPE(None, ctypes.c_void_p, ctypes.c_uint,
                                  ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p)
    callback = CALLBACK(on_data)

    handle = ctypes.c_void_p()
    open_in = winmm.midiInOpen
    open_in.argtypes = [ctypes.POINTER(ctypes.c_void_p), ctypes.c_uint,
                        ctypes.c_void_p, ctypes.c_void_p, ctypes.c_uint]
    rc = open_in(ctypes.byref(handle), in_index, ctypes.cast(callback, ctypes.c_void_p),
                 None, CALLBACK_FUNCTION)
    if rc != 0:
        print("midiInOpen(%d) failed with %d" % (in_index, rc))
        return 2
    winmm.midiInPrepareHeader.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_uint]
    winmm.midiInAddBuffer.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_uint]
    winmm.midiInStart.argtypes = [ctypes.c_void_p]
    winmm.midiInPrepareHeader(handle, ctypes.byref(hdr), ctypes.sizeof(hdr))
    winmm.midiInAddBuffer(handle, ctypes.byref(hdr), ctypes.sizeof(hdr))
    winmm.midiInStart(handle)

    hout = ctypes.c_void_p()
    # Open the output with the same null-device-callback shape: no callback for
    # an output, the last argument is only meaningful for a stream handle.
    winmm.midiOutOpen.argtypes = [ctypes.POINTER(ctypes.c_void_p), ctypes.c_uint,
                                  ctypes.c_void_p, ctypes.c_void_p, ctypes.c_uint]
    rc = winmm.midiOutOpen(ctypes.byref(hout), out_index, None, None, 0)
    if rc != 0:
        print("midiOutOpen(%d) failed with %d" % (out_index, rc))
        winmm.midiInStop(handle)
        winmm.midiInClose(handle)
        return 2

    def send(status, data1, data2):
        winmm.midiOutShortMsg.argtypes = [ctypes.c_void_p, ctypes.c_uint]
        packed = (status & 0xFF) | ((data1 & 0x7F) << 8) | ((data2 & 0x7F) << 16)
        return winmm.midiOutShortMsg(hout, packed)

    print("")
    print("listening on input %d, echoing on output %d, +%d semitones"
          % (in_index, out_index, OCTAVE))
    print("press a key - the same key comes back an octave higher")
    print("a control change is passed through as it is")
    print("Ctrl+C exits and silences the output")
    if args.watch:
        # The flag exists because "silence" has two causes a person cannot
        # tell apart from where they sit: nothing arrived, or something
        # arrived and the sound is elsewhere. Counting the events that
        # arrived separates those two, and counting is all this does - the
        # printed lines below are the same ones the loop always printed, on
        # purpose, so a reading taken with --watch is a reading of the same
        # program.
        print("watch is on: every event is printed as it arrives")
    print("")

    deadline = time.time() + args.seconds if args.seconds > 0 else None
    exit_code = 1
    try:
        while True:
            while queue:
                status, data1, data2 = queue.pop(0)
                counters.received += 1
                print("in  %s" % describe(status, data1, data2))
                moved = transpose(status, data1, data2)
                if moved is None:
                    if send(status, data1, data2) == 0:
                        counters.passed += 1
                        print("out %s  (unchanged)" % describe(status, data1, data2))
                    continue
                if moved == "out-of-range":
                    counters.dropped += 1
                    print("    %d + %d is past 127, not sent" % (data1, OCTAVE))
                    continue
                kind, note, velocity = moved
                if send(kind, note, velocity) == 0:
                    counters.sent += 1
                    print("out %s  (+%d)" % (describe(kind, note, velocity), OCTAVE))
                    if kind == NOTE_ON:
                        playing = note
                        counters.open_notes += 1
                    else:
                        counters.open_notes = max(0, counters.open_notes - 1)
                    exit_code = 0
            if deadline is not None and time.time() >= deadline:
                print("--seconds elapsed, stopping")
                break
            time.sleep(0.005)
    except KeyboardInterrupt:
        print("")
        print("Ctrl+C - closing")
    finally:
        # Everything the output is holding, released before the port goes away.
        # A note left sounding outlives the process on a real synth, and the
        # person is left with a drone they cannot explain.
        for channel in range(16):
            send(0xB0 | channel, 123, 0)
        counters.unreleased = counters.open_notes
        winmm.midiInStop(handle)
        winmm.midiInReset(handle)
        winmm.midiInUnprepareHeader(handle, ctypes.byref(hdr), ctypes.sizeof(hdr))
        winmm.midiInClose(handle)
        winmm.midiOutClose(hout)
        print("")
        print(counters.summary())
        if counters.received == 0:
            print("VERDICT: nothing was received. The cable, the instrument, or the")
            print("port choice is the cause - this is a failure, not a quiet pass.")
        elif counters.sent == 0:
            print("VERDICT: notes arrived but none could be sent. The output port is")
            print("open and the driver refused the messages.")
        else:
            print("VERDICT: %d note(s) echoed an octave up. Whether they were audible")
            print("is yours to say; this process only handed bytes to a driver." % counters.sent)
    return exit_code


# ---------------------------------------------------------------------------
# what the person at the instrument is asked to do
# ---------------------------------------------------------------------------

def instructions():
    """The part of this program that is meant for a person, not a log.

    Written to be read out loud, and phrased for someone who cannot see the
    screen: every line says what to DO before it says what it means, and
    nothing here needs a mouse or a window. The exit is said in the same
    breath as the first instruction, because a test that cannot be stopped
    without knowing the secret is not a test a person will run twice.
    """
    print("---")
    print("What to do")
    print("")
    print("  This one runs in a terminal: type the command, press Enter,")
    print("  and it stays open until you stop it. Nothing opens a window.")
    print("")
    print("  Press any key on the Nord. Listen.")
    print("  The same note comes back one octave higher -")
    print("  press the C in the middle, hear the C above it.")
    print("  A pedal or a wheel is passed through unchanged.")
    print("")
    print("  If you hear nothing, press a key on ANY controller")
    print("  (the Nord, the piano, whatever is plugged in) and hold it.")
    print("  Every event that arrives is printed below as it arrives,")
    print("  so the question 'is my controller being heard at all'")
    print("  is answered on the screen even when nothing sounds.")
    print("")
    print("  If it says the ports are dummy, no hardware was found: stop")
    print("  here, that is the answer. --list shows what it can see.")
    print("")
    print("  To stop: press Ctrl+C. Nothing is saved and nothing is left")
    print("  running. If the ports look wrong, rerun with --list first,")
    print("  then --input N --output N to pick a different pair.")
    print("---")
    print("")


# ---------------------------------------------------------------------------
# the demonstration inside nvgt - the same feature, asked of the plugin
# ---------------------------------------------------------------------------

# Why this text is here and not in a .nvgt file beside the other scripts.
#
# The midi_in/midi_out pair the script holds are *globals* of the plugin
# (registered as "midi_input midi_in" / "midi_output midi_out"), and their port
# has to be chosen before any note can arrive. A .nvgt file cannot do that half:
# it has no command line and no environment to read a port number from, and the
# workflow would then have to bake an index into the file per run. So the file
# is written here, at the moment of the run, with the ports that were just
# resolved substituted in - and the script that nvgt executes is exactly the
# script a person would have typed by hand.
#
# The '<' and '>' placeholders are not formatting: a literal port number goes
# in their place, and the file that comes out has nothing left to substitute.
NVGT_DEMO = """#pragma plugin nvmidi

// press a key on the Nord: it is sent back out one octave higher.
// This script does not read the input port. The plugin does, on its own
// thread, and leaves each message in a queue - so the loop below only has to
// look at it. That is the whole of the contract: has_message() says whether
// anything is waiting, next_message(m) hands over one message and returns
// false when there is nothing left.
//
// Every method on these two globals is const, and that shapes the loop: a
// reader has no send, so a note leaves through midi_out and midi_in is only
// ever asked whether anything arrived. The names are the plugin's own, taken
// from the registration rather than from memory: open(uint port),
// has_message(), next_message(midi_message&out), send(uint, uint, uint),
// all_notes_off().
void main() {
	const int OCTAVE = 12;
	const int SECONDS = <SECONDS>;
	midi_in.open(<IN_PORT>);
	midi_out.open(<OUT_PORT>);

	print("NVMIDI_DEMO_READY input port <IN_PORT> output port <OUT_PORT>");
	print("press a key - the same key comes back an octave higher");
	print("a control change is passed through as it is");
	print("Ctrl+C exits and silences the output");
	print("");

	for (int i = 0; i < SECONDS * 200; i++) {
		while (midi_in.has_message()) {
			midi_message m;
			if (!midi_in.next_message(m)) break;
			print("in  status " + m.status + " data " + m.data1 + " " + m.data2);

			uint kind = m.status & 0xF0;
			bool is_note = (kind == 0x90 && m.data2 > 0) || kind == 0x80;

			if (is_note) {
				int moved = m.data1 + OCTAVE;
				if (moved <= 127) {
					uint back = (kind == 0x90) ? 0x90 : 0x80;
					midi_out.send(back, moved, m.data2);
					print("out key " + moved + "  (+" + OCTAVE + ")");
				}
			} else {
				midi_out.send(m.status, m.data1, m.data2);
				print("out unchanged");
			}
		}
		wait(5);
	}

	midi_out.all_notes_off();
	print("NVMIDI_DEMO_END");
}
"""


def write_nvgt_demo(in_port, out_port, seconds, path="nvmidi_octave_echo.nvgt"):
    """Write the .nvgt demonstration with the ports folded in. Returns the path."""
    text = NVGT_DEMO.replace("<IN_PORT>", str(in_port))
    text = text.replace("<OUT_PORT>", str(out_port))
    text = text.replace("<SECONDS>", str(seconds))
    with open(path, "w") as fh:
        fh.write(text)
    return path

def resolve_ports(args):
    """(in_port, out_port, sentence) - what --nvgt folds into the script.

    Why this is not run_windows(): that function exists to *demonstrate* the
    driver, so it loads tools/winmm_loopback/nvmidi.dll into this process when
    asked and then reads the port list it just created. Here the ports are only
    being named, and on a machine with the Nord and the GS Wavetable Synth
    plugged in, the loopback driver is not merely unnecessary - it is wrong.
    What this program would be sending on is a virtual port nothing is
    listening to, so the substitution would name ports the person cannot hear.

    The driver path stays accepted, because a machine with no controller at all
    is exactly the case the loopback was written for, and there the person has
    no other port to name.
    """
    if platform.system() == "Windows":
        winmm = WinMm()
        if args.driver:
            rc = winmm.load_driver(args.driver)
            why = winmm.driver_status(args.driver) if rc == 0 else \
                  "could not load %s" % args.driver
            print(why)
        ins, outs = winmm.input_count(), winmm.output_count()
        names_in = [winmm.input_name(i) for i in range(ins)]
        names_out = [winmm.output_name(i) for i in range(outs)]
    else:
        alsa = Alsa("octave-echo")
        names_in = alsa.port_names("read")
        names_out = alsa.port_names("write")

    print("")
    print("inputs:")
    for i, name in enumerate(names_in):
        print("  %d  %s" % (i, name))
    print("outputs:")
    for i, name in enumerate(names_out):
        print("  %d  %s" % (i, name))

    in_port = args.input if args.input is not None else 0
    out_port = args.output if args.output is not None else 0
    if out_port >= max(len(names_out), 1):
        out_port = 0
    sentence = ("using input port %d and output port %d - if that is not the "
                "instrument, rerun with --input N --output N" % (in_port, out_port))
    return in_port, out_port, sentence


# ---------------------------------------------------------------------------
# entry
# ---------------------------------------------------------------------------

def main(argv=None):
    parser = argparse.ArgumentParser(
        description="press a key, hear the same note an octave higher",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="windows: pass --driver tools/winmm_loopback/nvmidi.dll to load the\n"
               "loopback driver into this process before listing the ports.\n"
               "linux: alsa ports are listed; the alsa route lives in test_nvmidi.py\n"
               "--interactive, which goes through the plugin.",
    )
    parser.add_argument("--driver", help="windows only: winmm loopback driver to load first")
    parser.add_argument("--input", type=int, default=None, help="input port index (default 0)")
    parser.add_argument("--output", type=int, default=None, help="output port index (default 0)")
    parser.add_argument("--seconds", type=float, default=0.0,
                        help="stop on its own after this long; 0 waits for Ctrl+C")
    parser.add_argument("--list", action="store_true", help="print the ports and exit")
    parser.add_argument("--watch", action="store_true",
                        help="print every event as it arrives, so a port that is "
                             "open but silent can be told from one that is not")
    parser.add_argument("--nvgt", metavar="PATH", default=None,
                        help="write the .nvgt demonstration with the resolved "
                             "ports folded in, then exit. This is the one that "
                             "goes through the plugin; it needs nvgt.exe and the "
                             "plugin dll in the same folder.")
    args = parser.parse_args(argv)

    print("octave echo - midi in, the same note out %d semitones up" % OCTAVE)
    print("platform    %s %s" % (platform.system(), platform.machine()))
    print("")
    instructions()

    if args.nvgt:
        in_port, out_port, why = resolve_ports(args)
        print(why)
        path = write_nvgt_demo(in_port, out_port,
                               args.seconds if args.seconds > 0 else 120,
                               args.nvgt)
        print("")
        print("written: %s" % path)
        print("input port %d, output port %d" % (in_port, out_port))
        print("")
        print("Now, in the folder that holds nvgt.exe and nvmidi.dll:")
        print("    nvgt.exe %s" % path)
        print("")
        print("It runs for %s seconds, then stops by itself. Ctrl+C in the nvgt"
              % (args.seconds if args.seconds > 0 else 120))
        print("window stops it sooner. The same test - press a key, hear it an")
        print("octave up - but the note comes from the plugin this time, not")
        print("from this program.")
        return 0

    if platform.system() == "Windows":
        return run_windows(args)
    ins, outs, why = open_ports_alsa(args)
    print(why)
    return 2


if __name__ == "__main__":
    sys.exit(main())
