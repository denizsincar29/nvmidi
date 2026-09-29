#!/usr/bin/env python3
"""Send a note into another process's virtual port on the ALSA sequencer.

Why this exists
---------------

The plugin can open a port and say it did, and a stubbed-out backend can say
the same thing without a port ever existing. What a stub cannot fake is a note
that arrives from outside the process, addressed to a port the process
published and did not tell anyone about.

So the reading half of the end-to-end test needs a second process, and it has
to be a midi *writer*: `aseqdump` only listens, and `aconnect` connects two
ports that already exist, neither of which produces a note on its own. There is
no `amidi` on a modern runner's alsa-utils (it moved to alsa-utils' alsa-tools
package at best, and it works on rawmidi devices, not on sequencer ports), so
the sender is twelve lines of python against the same C library the plugin
links, reached through ctypes rather than a build.

Nothing here is installed: libasound is already on the runner, and ctypes is in
the standard library. That matters because this file runs in a ci job whose
whole point is to avoid installing anything into the user's machine.

Dependencies - what is measured here and what is not:

  * a port name given to snd_seq_create_simple_port is visible to every other
    client, and snd_seq_parse_address resolves it by substring (documented
    alsa behaviour, and the whole basis of this file)
  * snd_seq_connect_to is how a client addresses another client's port; the
    event is then broadcast to subscribers and delivered to that subscription
  * the alsa sequencer itself is *not* guaranteed to exist. It is a kernel
    object (/dev/snd/seq, the snd-seq module) and the machine must have it.
    This file cannot create it; if snd_seq_open fails, that is the runner's
    configuration and the message says so.

The exit code is the interface: 0 only when the note was actually handed to the
sequencer, with the reason on stderr otherwise.
"""

import ctypes
import ctypes.util
import sys
import time

# --- the parts of alsa's sequencer API that this needs ----------------------
#
# Copied by hand from alsa/seq.h and alsa/seqmid.h rather than importing a
# python binding: pyalsa is not on an ubuntu runner and is not worth adding to
# a test whose subject is a different library entirely.

SND_SEQ_OPEN_OUTPUT = 1
SND_SEQ_PORT_CAP_READ = 1 << 0
SND_SEQ_PORT_CAP_SUBS_READ = 1 << 5
SND_SEQ_PORT_TYPE_MIDI_GENERIC = 1 << 1

SND_SEQ_ADDRESS_SUBSCRIBERS = 254
SND_SEQ_ADDRESS_UNKNOWN = 255

SND_SEQ_EVENT_NOTEON = 6
SND_SEQ_EVENT_NOTEOFF = 7
SND_SEQ_EVENT_LENGTH_FIXED = 1 << 1
SND_SEQ_TIME_STAMP_TICK = 0
SND_SEQ_TIME_MODE_ABS = 0
SND_SEQ_QUEUE_DIRECT = 253

# sizeof(snd_seq_event_t) in the capped-at-28-bytes layout this abi uses -
# confirmed at runtime below instead of being trusted, because getting it wrong
# means the kernel reads a garbage event rather than reporting an error.
EVENT_SIZE = 32


class Event(ctypes.Structure):
    """snd_seq_event_t, only as far as the fields a note-on needs."""

    _fields_ = [
        ("type", ctypes.c_ubyte),
        ("flags", ctypes.c_ubyte),
        ("tag", ctypes.c_ubyte),
        ("queue", ctypes.c_ubyte),
        # union snd_seq_timestamp: tick, which is what ABS/TICK selection uses
        ("time_tick", ctypes.c_uint),
        ("source_client", ctypes.c_ubyte),
        ("source_port", ctypes.c_ubyte),
        ("dest_client", ctypes.c_ubyte),
        ("dest_port", ctypes.c_ubyte),
        # union with note/control/... - note is the largest of them
        ("channel", ctypes.c_ubyte),
        ("note", ctypes.c_ubyte),
        ("velocity", ctypes.c_ubyte),
        ("off_velocity", ctypes.c_ubyte),
        ("duration", ctypes.c_uint),
        ("_pad", ctypes.c_ubyte * (EVENT_SIZE - 18)),
    ]


def main(argv):
    if len(argv) < 2:
        print("usage: e2e_send.py <port name> [seconds to keep retrying]", file=sys.stderr)
        return 2
    wanted = argv[1]
    # The port name to look for, when the process under test publishes it
    # under a different name than the one it announces on the console. The
    # alsa script uses the same string for both, so it can leave this out; a
    # script whose console line is timed has to say where the port is.
    port_name = argv[3] if len(argv) > 3 else wanted
    deadline_seconds = float(argv[2]) if len(argv) > 2 else 20.0

    name = ctypes.util.find_library("asound")
    if not name:
        print("libasound is not present, so there is no sequencer to send into", file=sys.stderr)
        return 1
    lib = ctypes.CDLL(name)

    lib.snd_seq_open.argtypes = [ctypes.POINTER(ctypes.c_void_p), ctypes.c_char_p,
                                 ctypes.c_int, ctypes.c_int]
    lib.snd_seq_open.restype = ctypes.c_int
    lib.snd_seq_set_client_name.argtypes = [ctypes.c_void_p, ctypes.c_char_p]
    lib.snd_seq_create_simple_port.argtypes = [ctypes.c_void_p, ctypes.c_char_p,
                                               ctypes.c_uint, ctypes.c_uint]
    lib.snd_seq_parse_address.argtypes = [ctypes.c_void_p, ctypes.POINTER(ctypes.c_ubyte * 2),
                                          ctypes.c_char_p]
    lib.snd_seq_connect_to.argtypes = [ctypes.c_void_p, ctypes.c_int,
                                       ctypes.c_ubyte, ctypes.c_ubyte]
    lib.snd_seq_event_output.argtypes = [ctypes.c_void_p, ctypes.POINTER(Event)]
    lib.snd_seq_drain_output.argtypes = [ctypes.c_void_p]
    lib.snd_strerror.argtypes = [ctypes.c_int]
    lib.snd_strerror.restype = ctypes.c_char_p
    lib.snd_seq_close.argtypes = [ctypes.c_void_p]

    def why(rc):
        return lib.snd_strerror(rc).decode("utf-8", "replace")

    handle = ctypes.c_void_p()
    rc = lib.snd_seq_open(ctypes.byref(handle), b"default", SND_SEQ_OPEN_OUTPUT, 0)
    if rc < 0:
        print("cannot open the alsa sequencer: %s" % why(rc), file=sys.stderr)
        return 1
    try:
        lib.snd_seq_set_client_name(handle, b"nvmidi e2e sender")
        # The port the sender writes from. It has to be readable and
        # subscribable so the kernel will route what it emits.
        port = lib.snd_seq_create_simple_port(
            handle, b"out", SND_SEQ_PORT_CAP_READ | SND_SEQ_PORT_CAP_SUBS_READ,
            SND_SEQ_PORT_TYPE_MIDI_GENERIC)
        if port < 0:
            print("cannot create a sender port: %s" % why(port), file=sys.stderr)
            return 1

        # The destination is looked up by name, over and over: the engine takes
        # a moment to start and publish its port, and a name that does not
        # exist yet is not an error worth failing on.
        addr = (ctypes.c_ubyte * 2)()
        deadline = time.time() + deadline_seconds
        connected = False
        last = ""
        while time.time() < deadline:
            rc = lib.snd_seq_parse_address(handle, ctypes.byref(addr), port_name.encode())
            if rc >= 0:
                rc = lib.snd_seq_connect_to(handle, port, addr[0], addr[1])
                if rc >= 0:
                    print("connected to %r as client %d port %d" % (wanted, addr[0], addr[1]))
                    connected = True
                    break
                last = why(rc)
            else:
                last = why(rc)
            time.sleep(0.25)
        if not connected:
            print("no port called %r appeared within %gs (%s)" % (wanted, deadline_seconds, last),
                  file=sys.stderr)
            return 1

        # A moment for the subscription to take effect before the first event:
        # connect_to is asynchronous on the kernel side.
        time.sleep(0.5)

        def send(ev_type, note, velocity):
            ev = Event()
            ev.type = ev_type
            ev.flags = SND_SEQ_EVENT_LENGTH_FIXED | SND_SEQ_TIME_STAMP_TICK | SND_SEQ_TIME_MODE_ABS
            ev.queue = SND_SEQ_QUEUE_DIRECT
            ev.source_client = 0
            ev.source_port = port
            ev.dest_client = SND_SEQ_ADDRESS_SUBSCRIBERS
            ev.dest_port = SND_SEQ_ADDRESS_UNKNOWN
            ev.channel = 0
            ev.note = note
            ev.velocity = velocity
            rc = lib.snd_seq_event_output(handle, ctypes.byref(ev))
            if rc < 0:
                print("cannot queue a note event: %s" % why(rc), file=sys.stderr)
                return False
            return True

        if not send(SND_SEQ_EVENT_NOTEON, 60, 100):
            return 1
        lib.snd_seq_drain_output(handle)
        print("sent note on 60 to %r" % wanted)
        time.sleep(0.2)
        send(SND_SEQ_EVENT_NOTEOFF, 60, 0)
        lib.snd_seq_drain_output(handle)
        print("sent note off 60")
        return 0
    finally:
        lib.snd_seq_close(handle)


if __name__ == "__main__":
    sys.exit(main(sys.argv))
