#!/usr/bin/env python3
"""List this process's midi ports, so the job can tell two processes apart.

Why this exists
---------------

The e2e has two processes and they do not agree about how many midi outputs the
machine has. The listener loads the loopback driver, so the driver's port is in
the listener's output list; the plugin runs in its own process and, being a
plain program opening a plain port, gets that process's list, which has no such
entry. Measured: the listener's process reported two outputs, the plugin's one.

That difference is the whole handshake problem. The script under test can only
hand the listener a *name*, and it can only name ports in its own list, so on
the runner the name it picks is `Microsoft GS Wavetable Synth` - a device with
no sound hardware behind it that answers with MMRESULT 1. The listener, told
that name, would then look for it among inputs and reasonably fail; the notice
would read as a plugin bug.

So the job reads the plugin's process list here - the same ctypes calls, the
same machine, the same shape of process, just before the script starts - and
passes the *index* in that list through to the listener. The listener turns the
index into a name off its own output list and opens the input the driver
published under that name. Nothing is guessed and the port that carries the
notes is the driver's, which is the only port on this runner that is real.

Output: one line per port, `index<TAB>name`, then a summary line. `--json` for
the form the env var holds. Exit 0 unless winmm itself refuses to answer.

Nothing here needs installing: ctypes ships with python and winmm with windows.
"""

import ctypes
import json
import sys

MAXPNAMELEN = 32

# The name fields are c_char and not c_wchar on purpose, and it is not a typo
# to fix: the entry point is midiInGetDevCapsW, the *wide* one, but the only
# declarations mingw's headers carry for it take an 8 bit name through a driver
# that fills bytes one per character. Decoding those bytes as UTF-16 does not
# truncate visibly - it swallows the NUL after the first letter and the name
# arrives one character long.
class MIDIOUTCAPS(ctypes.Structure):
    _fields_ = [
        ("wMid", ctypes.c_ushort),
        ("wPid", ctypes.c_ushort),
        ("vDriverVersion", ctypes.c_uint),
        ("szPname", ctypes.c_char * MAXPNAMELEN),
        ("wTechnology", ctypes.c_ushort),
        ("wVoices", ctypes.c_ushort),
        ("wNotes", ctypes.c_ushort),
        ("wChannelMask", ctypes.c_ushort),
        ("dwSupport", ctypes.c_uint),
    ]


def name_of(field):
    return field.split(b"\x00", 1)[0].decode("latin-1")


def ports():
    winmm = ctypes.WinDLL("winmm")
    n = winmm.midiOutGetNumDevs()
    out = []
    for i in range(n):
        caps = MIDIOUTCAPS()
        rc = winmm.midiOutGetDevCapsW(i, ctypes.byref(caps), ctypes.sizeof(caps))
        out.append({"index": i, "name": name_of(caps.szPname) if rc == 0 else "",
                    "caps_rc": rc})
    return out


def main(argv):
    found = ports()
    if "--json" in argv:
        print(json.dumps(found))
        return 0
    for p in found:
        print("%d\t%s" % (p["index"], p["name"]))
    print("%d midi output(s) in this process" % len(found))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
