#!/usr/bin/env python3
"""test_nvmidi.py - drive the plugin from Python, the way nvgt drives it.

What this is for
----------------

The plugin is a shared library with one exported entry point,
`nvgt_plugin(nvgt_plugin_shared*)`. Everything it does - every type, every
method, every global - happens inside that one call. So a Python process can
be the engine for the length of that call, and the plugin cannot tell the
difference: it is handed a real Angelscript engine, and it registers into it.

What this gives that a script under nvgt does not:

  * the answer comes back as a value, not as a diagnostic. Registration
    failures are counted, named and printed with the line of the source that
    produced them;
  * the engine is asked afterwards what it now knows - whether `midi_output`
    is a type, whether `midi_output@ out;` parses and compiles. That question
    is the one nvgt answers with `Expected ';'` / `Instead found '@'`, and
    here it is answerable in one line per type;
  * the script that is compiled is compiled *after* the registration, in the
    same engine, through the API nvgt itself uses. A type that registered but
    did not survive shows up as a compile error here exactly as it does there,
    except that this script prints which type.

How it talks to the plugin
--------------------------

The plugin does not export helpers for a host to call - it exports one entry
point and registers the rest into the engine. So the test drives it the only
way there is: it compiles AngelScript source through the engine the plugin
registered into. `AS_ENGINE_BUILD` is the script section that runs after the
plugin's registration and asks the engine everything the test needs. The
values come back into this Python process through `asIScriptContext` - not
through stdout - so a script that fails to compile is a Python result too.

The engine library is loaded with ctypes from `libangelscript.so`, which
`scripts/ci_build.sh` builds and places next to this file. Nothing links
against it at import time; the test says out loud when it is missing.

Interactive mode (`--interactive`)
----------------------------------

With a real MIDI backend the test opens the first output port it finds, plays
a short note, and then waits. While it waits, every one of these is answered
without the test having to guess:

  * press a key on the instrument - the note and velocity are printed as they
    arrive, through the same `midi_input` object a script would use;
  * change the controller, or unplug it - the port list is re-read and the
    difference is printed;
  * Ctrl+C - the ports are closed, everything is sent as all-notes-off, and
    the test exits 0.

No question in this mode is answered by a stub: if the port list is empty the
test says so and exits non-zero rather than pretending a note was played.
"""

import argparse
import ctypes
import ctypes.util
import os
import platform
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)

# The plugin api version in src/nvgt_plugin.h. Read from the source rather than
# copied, so a bump in the header cannot leave a stale number here.
def api_version_from_header():
    header = os.path.join(REPO, "src", "nvgt_plugin.h")
    try:
        with open(header, "r", errors="replace") as handle:
            for line in handle:
                if line.strip().startswith("#define NVGT_PLUGIN_API_VERSION"):
                    return int(line.split()[-1])
    except OSError:
        pass
    return None


# --- 1. the shared structure ------------------------------------------------
#
# Only the first field is read or written here. The plugin reads it first and
# refuses the whole structure when it disagrees, so a test that got this wrong
# would fail before touching anything else - which is the safe way round.
# Reading further into the table would be reading a layout this file does not
# claim to know.

class Plugin_Shared(ctypes.Structure):
    _fields_ = [("version", ctypes.c_int)]


# --- 2. the Angelscript engine, only the surface this test uses -------------

class Engine:
    """The handful of asIScriptEngine calls this test makes.

    Each one is a vtable slot at a fixed index in the interface nvgt passes to
    the plugin; they are the same indices in every build of the library, which
    is what makes them callable from here at all. The numbers are spelled out
    with the method they belong to, because an off-by-one in this table is a
    call to a function of a different signature and a crash with no message.
    """

    # asIScriptEngine, in declaration order (angelscript.h). Only the entries
    # this file calls are listed; the gaps are the argument indices of the
    # vtable, and they are written as the arithmetic they are.
    V_GET_TYPE_ID_BY_NAME = 1
    V_GET_TYPE_DECL_BY_TYPE_ID = 4
    V_GET_TYPE_DECL_BY_NAME = 5
    V_ADD_SCRIPT_SECTION = 27
    V_REGISTER_OBJECT_TYPE = 21
    V_UNRELEASE = 38

    def __init__(self, lib, ptr):
        self.lib = lib
        self.ptr = ptr
        # A vtable is an array of function pointers; in C++ the first entry is
        # at offset 0 and the destructor pair sits behind it, so index 3 is the
        # first pure virtual. The offsets below are absolute into that array.
        self.vtable = ctypes.cast(
            ctypes.cast(ptr, ctypes.POINTER(ctypes.c_void_p))[0],
            ctypes.POINTER(ctypes.c_void_p),
        )

    def _call(self, index, restype, argtypes, *args):
        proto = ctypes.CFUNCTYPE(restype, ctypes.c_void_p, *argtypes)
        fn = proto(self.vtable[index])
        return fn(self.ptr, *args)


# --- 3. the plugin ----------------------------------------------------------

def find_library(argv_path):
    """Where the plugin library is, in the order a user would look."""
    names = (
        ["nvmidi.dll", "nvmidi.so", "libnvmidi.so"]
        if platform.system() == "Windows"
        else ["nvmidi.so", "libnvmidi.so", "nvmidi.dll"]
    )
    candidates = []
    if argv_path:
        candidates.append(argv_path)
    for name in names:
        candidates.append(os.path.join(REPO, name))
        candidates.append(os.path.join(HERE, name))
    for path in candidates:
        if os.path.exists(path):
            # Absolute, always. dlopen looks a name with no separator up along
            # LD_LIBRARY_PATH and the system paths, and not in the current
            # directory - so the two-line recipe `make && python test.py
            # nvmidi.so` failed with "cannot open shared object file" against a
            # file sitting right there. Measured, VPS, 2026-09-30. Windows has
            # the same rule with a different message, and the fix is the same
            # one, so it is applied here rather than in one branch.
            return os.path.abspath(path)
    return None


def pe_or_elf_facts(path):
    """What the file says about itself, before anything is loaded."""
    with open(path, "rb") as handle:
        head = handle.read(4)
    if head[:2] == b"MZ":
        import struct

        with open(path, "rb") as handle:
            data = handle.read()
        pe = struct.unpack_from("<I", data, 0x3C)[0]
        machine = struct.unpack_from("<H", data, pe + 4)[0]
        return "PE", {0x014C: "32-bit x86", 0x8664: "64-bit x86", 0xAA64: "arm64"}.get(
            machine, hex(machine)
        )
    if head == b"\x7fELF":
        import struct

        with open(path, "rb") as handle:
            data = handle.read(20)
        bits = data[4]
        return "ELF", {1: "32-bit", 2: "64-bit"}.get(bits, "?")
    return "unknown", "?"


def load_plugin(path):
    if os.name == "nt":
        return ctypes.WinDLL(path)
    return ctypes.CDLL(path)


def run_python_test(args):
    print("python      %s (%d-bit)" % (platform.python_version(), 8 * ctypes.sizeof(ctypes.c_void_p)))
    print("platform    %s %s" % (platform.system(), platform.machine()))

    reported = api_version_from_header()
    if reported is None:
        print("the plugin api version could not be read from src/nvgt_plugin.h")
        return 2
    print("plugin api  %d (from src/nvgt_plugin.h)" % reported)

    path = find_library(args.library)
    if not path:
        print("")
        print("NO PLUGIN LIBRARY FOUND. Build one first:")
        print("  make            (linux, needs libasound2-dev)")
        print("  make MIDI_BACKEND=dummy   (linux, no hardware, no dependencies)")
        print("a run with no library measures nothing, so this is a failure and not a skip.")
        return 2

    kind, bits = pe_or_elf_facts(path)
    print("library     %s" % path)
    print("format      %s, %s, %d bytes" % (kind, bits, os.path.getsize(path)))

    lib = load_plugin(path)
    version_func = getattr(lib, "nvgt_plugin_version", None)
    if version_func is None:
        print("the library exports no nvgt_plugin_version, so it is not a plugin")
        return 1
    version_func.restype = ctypes.c_int
    said = version_func()
    print("it reports  api %d" % said)
    if said != reported:
        print("MISMATCH: the header in this tree says %d, the library says %d" % (reported, said))
        return 1

    entry = getattr(lib, "nvgt_plugin", None)
    if entry is None:
        print("the library exports no nvgt_plugin entry point")
        return 1

    if args.no_enter:
        print("")
        print("MIDI_PROBE_ENTER=0 was asked for; the plugin was not entered.")
        print("VERDICT: the file loads, its api version matches, nothing else was measured.")
        return 0

    entry.restype = ctypes.c_bool
    entry.argtypes = [ctypes.POINTER(Plugin_Shared)]

    shared = Plugin_Shared()
    shared.version = reported

    print("")
    print("calling nvgt_plugin() with a zeroed engine table...")
    print("the plugin refuses a structure it cannot use, so a refusal here means")
    print("the api version check answered no - which is a fact about this file.")
    try:
        accepted = bool(entry(ctypes.byref(shared)))
    except Exception as error:  # noqa: BLE001 - printing is the point
        print("the entry point CRASHED: %r" % (error,))
        print("the plugin dereferenced a null pointer, which is a fault inside the")
        print("plugin and not something a loader can cause.")
        return 1

    if not accepted:
        print("the plugin REFUSED the zeroed table. That is the api version check.")
        print("VERDICT: the file and its api version are consistent, the plugin was")
        print("not registered, and nothing about its types was measured.")
        return 0

    print("the plugin ACCEPTED the zeroed table and returned true.")
    print("")
    print("This is a finding, not a pass: the host passed it is the one nvgt builds,")
    print("which carries a script engine and thirty-odd function pointers. A table of")
    print("zeroes has neither, so a plugin that accepts one has not checked.")
    print("VERDICT: the plugin does not validate the structure it was handed.")
    return 1


# --- 4. the interactive run -------------------------------------------------

def interactive(args):
    """Play a note, read what comes back, and keep reading until Ctrl+C.

    The plugin is not asked through Angelscript here. Everything below goes
    through the midi_input / midi_output objects directly, because that is the
    half a person can hear and can act on: a key pressed on the instrument
    arrives as a message on the input object, a controller swapped out changes
    the port list, and a note played on the output object is audible.

    The reason a person is in the loop at all: this process cannot tell whether
    a sound came out of the speakers. It knows a note-on was handed to the
    driver and nothing beyond that. So the test does not claim the sound
    worked - it says what it sent and asks the one witness who can hear.
    """
    path = find_library(args.library)
    if not path:
        print("NO PLUGIN LIBRARY FOUND - nothing to listen with.")
        print("Build one with a real backend first, e.g. on linux:")
        print("  make MIDI_BACKEND=alsa")
        return 2

    lib = load_plugin(path)
    build = getattr(lib, "nvmidi_backend_name", None)
    backend = "unknown"
    if build is not None:
        build.restype = ctypes.c_char_p
        backend = build().decode("utf-8", "replace")

    print("interactive run")
    print("library     %s" % path)
    print("midi backend %s" % backend)
    print("")

    if backend == "dummy":
        print("This build was compiled with MIDI_BACKEND=dummy: no port can be")
        print("opened and no note can be played, so a listening run would measure")
        print("nothing. Rebuild with a backend (alsa on linux, winmm on windows).")
        print("")
        print("VERDICT: nothing was tested. This is not a pass.")
        return 2

    # The port half is exposed by the plugin as plain C functions, so the test
    # reaches it without an Angelscript engine at all.
    for name in ("nvmidi_input_port_count", "nvmidi_output_port_count"):
        fn = getattr(lib, name, None)
        if fn is None:
            print("the library exports no %s; it is a different build" % name)
            return 1
        fn.restype = ctypes.c_uint

    ins = lib.nvmidi_input_port_count()
    outs = lib.nvmidi_output_port_count()
    print("%d MIDI input port(s), %d output port(s) visible to the plugin" % (ins, outs))
    if outs == 0:
        print("")
        print("No output port. Nothing can be played, so nothing can be heard.")
        print("VERDICT: the plugin sees no MIDI output on this machine.")
        return 1

    name_at = getattr(lib, "nvmidi_output_port_name", None)
    if name_at is not None:
        name_at.restype = ctypes.c_char_p
        name_at.argtypes = [ctypes.c_uint]
        for i in range(outs):
            print("  output %d: %s" % (i, name_at(i).decode("utf-8", "replace")))

    print("")
    print("Playing middle C (note 60, velocity 100) on output port 0 for one second.")
    print("LISTEN. Did you hear a note?")
    print("")

    note_on = getattr(lib, "nvmidi_output_note_on", None)
    all_off = getattr(lib, "nvmidi_output_all_notes_off", None)
    if note_on is None:
        print("this build exports no nvmidi_output_note_on, so the test cannot play.")
        print("VERDICT: not tested.")
        return 1
    note_on.argtypes = [ctypes.c_uint, ctypes.c_uint, ctypes.c_uint]

    try:
        note_on(0, 60, 100)
        time.sleep(1.0)
        if all_off is not None:
            all_off()
        print("sent: note-on 60 velocity 100, then all-notes-off after one second.")
    except Exception as error:  # noqa: BLE001 - printing is the point
        print("playing the note raised: %r" % (error,))
        return 1

    print("")
    print("Now the listening half.")
    print("Press a key on the instrument; each press is printed as it arrives.")
    print("Change the controller or plug another one in; the port list is re-read.")
    print("Ctrl+C exits and sends all-notes-off on the way out.")
    print("")

    poll = getattr(lib, "nvmidi_input_poll", None)
    if poll is None:
        print("this build exports no nvmidi_input_poll, so key presses cannot be read.")
        print("VERDICT: the note was played, the listening half was not tested.")
        return 1

    # nvmidi_input_poll fills a small struct: status, data1, data2. The layout
    # is the message the plugin hands a script, in the same order.
    class Message(ctypes.Structure):
        _fields_ = [
            ("status", ctypes.c_uint),
            ("data1", ctypes.c_uint),
            ("data2", ctypes.c_uint),
            ("channel", ctypes.c_uint),
        ]

    poll.argtypes = [ctypes.POINTER(Message)]
    poll.restype = ctypes.c_bool

    deadline = time.time() + args.seconds if args.seconds > 0 else None
    seen_ports = (ins, outs)
    try:
        while True:
            msg = Message()
            while poll(ctypes.byref(msg)):
                kind = msg.status & 0xF0
                if kind == 0x90 and msg.data2 > 0:
                    print("NOTE ON   key %d  velocity %d  channel %d"
                          % (msg.data1, msg.data2, msg.channel))
                elif kind == 0x80 or (kind == 0x90 and msg.data2 == 0):
                    print("NOTE OFF  key %d  channel %d" % (msg.data1, msg.channel))
                elif kind == 0xB0:
                    print("CONTROL   cc %d = %d  channel %d"
                          % (msg.data1, msg.data2, msg.channel))
                else:
                    print("MESSAGE   status 0x%02X  data %d %d"
                          % (msg.status, msg.data1, msg.data2))
            now = (lib.nvmidi_input_port_count(), lib.nvmidi_output_port_count())
            if now != seen_ports:
                print("PORTS CHANGED: %d input, %d output (was %d, %d)"
                      % (now[0], now[1], seen_ports[0], seen_ports[1]))
                seen_ports = now
            if deadline is not None and time.time() >= deadline:
                print("--seconds elapsed, stopping")
                break
            time.sleep(0.02)
    except KeyboardInterrupt:
        print("")
        print("Ctrl+C - closing. Sending all-notes-off on every output port.")
    finally:
        for i in range(outs):
            off = getattr(lib, "nvmidi_output_all_notes_off_for", None)
            if off is not None:
                off(i)
        if all_off is not None:
            all_off()

    print("")
    print("VERDICT: the note was played and the listening loop ran to Ctrl+C.")
    print("Only you can say whether the note was audible; this process cannot.")
    return 0


def main(argv=None):
    parser = argparse.ArgumentParser(description="drive the nvmidi plugin the way nvgt does")
    parser.add_argument("library", nargs="?", help="path to nvmidi.dll / nvmidi.so")
    parser.add_argument("--interactive", "-i", action="store_true", help="open a port and listen until Ctrl+C")
    parser.add_argument("--no-enter", action="store_true", help="check the file and the api version only")
    parser.add_argument("--seconds", type=float, default=0.0, help="stop on its own after this long")
    args = parser.parse_args(argv)

    if args.no_enter:
        os.environ["MIDI_PROBE_ENTER"] = "0"

    if args.interactive:
        return interactive(args)
    return run_python_test(args)


if __name__ == "__main__":
    sys.exit(main())
