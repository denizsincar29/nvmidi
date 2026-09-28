#!/usr/bin/env python3
"""midi_probe.py - name the MIDI failure out loud, where nvgt stays mute.

Why this exists
---------------

When nvgt cannot use the nvmidi plugin all it says is, at best,

    Compilation error: file: nvmidi
    line: 0 (0)
    ERROR: failed to load plugin

and at worst - when the script is launched in a way that makes the engine
build a product instead of running one - nothing at all. Both look the same
from the outside, and neither says which of the several very different things
went wrong:

  * the dll is not where the engine looks for it;
  * the dll is there but Windows refuses to load it (a dependency the machine
    does not have, a 32/64 bit mismatch, a file that is not really a PE);
  * the dll loaded but was built against a different plugin api version;
  * the plugin was entered and crashed inside its own registration code;
  * everything loaded and there is simply no MIDI port to play on.

ctypes answers the first three with the loader's own words, in one command,
with no compiler and no nvgt involved. This script prints all of it and then
talks to the plugin through the same shared structure the engine uses.

Usage
-----

    python midi_probe.py                       # look next to this script
    python midi_probe.py c:\\nvgt\\lib\\nvmidi.dll

Requires 64-bit Python: the plugin is a 64-bit dll and a 32-bit interpreter
cannot load it at all - which is itself one of the answers this prints.
"""

import ctypes
import os
import struct
import sys

# The plugin api version this script knows how to drive. It is the number in
# src/nvgt_plugin.h in the plugin's own source tree; if the dll reports a
# different one, the two are not talking about the same structure and the
# script says so instead of reading garbage.
KNOWN_API_VERSION = 5

# --- 1. reading the file without loading it ---------------------------------


def pe_facts(path):
    """Return what the file says about itself, or raise with a plain reason."""
    with open(path, "rb") as handle:
        head = handle.read(2)
    if head != b"MZ":
        raise ValueError(
            "the file does not start with MZ, so it is not a Windows dll at all "
            "(a Linux library starts with 0x7f E L F; this is the mixup that put "
            "a .so under the name nvmidi.dll)"
        )
    with open(path, "rb") as handle:
        data = handle.read()

    pe = struct.unpack_from("<I", data, 0x3C)[0]
    if data[pe : pe + 4] != b"PE\0\0":
        raise ValueError("the file starts with MZ but carries no PE header; it is truncated or not a dll")
    machine = struct.unpack_from("<H", data, pe + 4)[0]
    opt = pe + 24
    magic = struct.unpack_from("<H", data, opt)[0]
    nsec = struct.unpack_from("<H", data, pe + 6)[0]
    optsize = struct.unpack_from("<H", data, pe + 20)[0]
    data_dir = opt + (112 if magic == 0x20B else 96)
    section_table = opt + optsize

    sections = []
    for index in range(nsec):
        off = section_table + index * 40
        vsize, vaddr, rawsize, rawoff = struct.unpack_from("<IIII", data, off + 8)
        sections.append((vaddr, vsize, rawoff, rawsize))

    def rva_to_off(rva):
        for vaddr, vsize, rawoff, _ in sections:
            if vaddr <= rva < vaddr + max(vsize, 1):
                return rawoff + rva - vaddr
        return -1

    def cstring(off):
        end = data.index(b"\0", off)
        return data[off:end].decode("ascii", "replace")

    # Import table: the list of libraries Windows must find before it will
    # accept this dll. One missing name and the whole file is refused.
    imports = []
    import_rva = struct.unpack_from("<I", data, data_dir + 8)[0]
    if import_rva:
        off = rva_to_off(import_rva)
        while True:
            name_rva = struct.unpack_from("<I", data, off + 12)[0]
            if name_rva == 0:
                break
            imports.append(cstring(rva_to_off(name_rva)))
            off += 20

    # Export table: what the engine looks up by name.
    exports = []
    export_rva = struct.unpack_from("<I", data, data_dir)[0]
    if export_rva:
        eo = rva_to_off(export_rva)
        count = struct.unpack_from("<I", data, eo + 20)[0]
        names_rva = struct.unpack_from("<I", data, eo + 32)[0]
        names_off = rva_to_off(names_rva)
        for index in range(count):
            name_rva = struct.unpack_from("<I", data, names_off + 4 * index)[0]
            exports.append(cstring(rva_to_off(name_rva)))

    return {
        "size": len(data),
        "machine": machine,
        "imports": imports,
        "exports": exports,
    }


MACHINE_NAMES = {
    0x014C: "32-bit x86",
    0x8664: "64-bit x86 (x64)",
    0xAA64: "64-bit ARM",
}

# --- 2. the structure the engine hands to the plugin ------------------------


class Plugin_Shared(ctypes.Structure):
    """The first three fields of nvgt_plugin_shared.

    Only the version matters here, and it has to be read before anything else
    in the structure is trusted: the rest is a table of function pointers laid
    out according to that number. Reading them when it does not match would be
    reading someone else's memory.
    """

    _fields_ = [
        ("version", ctypes.c_int),
        ("f_datastream_create", ctypes.c_void_p),
        ("f_datastream_get_ios", ctypes.c_void_p),
    ]


def probe(path):
    print("checking %s" % path)

    if not os.path.exists(path):
        print("  NO SUCH FILE.")
        print("  This is the first answer to rule out: the engine looks for the dll in")
        print("  its own lib/ folder, so that is where it has to be. A copy left in")
        print("  Downloads or in the project folder is never opened.")
        return 1

    size = os.path.getsize(path)
    if size == 0:
        print("  THE FILE IS EMPTY (0 bytes).")
        print("  Windows refuses an empty dll and says nothing; every symptom then")
        print("  looks like the plugin does not exist.")
        return 1

    try:
        facts = pe_facts(path)
    except ValueError as reason:
        print("  NOT USABLE: %s" % reason)
        return 1

    print("  size          %d bytes" % facts["size"])
    print("  built for     %s" % MACHINE_NAMES.get(facts["machine"], hex(facts["machine"])))
    print("  needs at load %s" % ", ".join(facts["imports"]))
    print("  offers        %s" % ", ".join(facts["exports"]))

    expected = {"nvgt_plugin", "nvgt_plugin_version"}
    missing = expected - set(facts["exports"])
    if missing:
        print("  MISSING ENTRY POINT: %s" % ", ".join(sorted(missing)))
        print("  The engine looks exactly those names up; without them the file is not")
        print("  a plugin whatever else it says.")
        return 1

    if ctypes.sizeof(ctypes.c_void_p) == 4 and facts["machine"] == 0x8664:
        print("  MISMATCH: this is a 64-bit dll and this Python is 32-bit, so Windows")
        print("  will refuse it before a single symbol is looked up. Run it with a")
        print("  64-bit Python (the official python.org installer is 64-bit).")
        return 1

    # --- the actual load. Windows' own words come back through the exception.
    print("  loading it now, so that Windows itself has to answer...")
    try:
        library = ctypes.WinDLL(path)
    except OSError as error:
        print("  WINDOWS REFUSED IT: %s" % error)
        print("  This is the message nvgt never shows. It names the file that could")
        print("  not be found or the image that could not be mapped, and it is the")
        print("  real reason the plugin does not work.")
        return 1
    print("  loaded.")

    try:
        version_func = library.nvgt_plugin_version
    except AttributeError:
        print("  loaded, but nvgt_plugin_version is not exported, so it is not a plugin")
        return 1
    version_func.restype = ctypes.c_int
    reported = version_func()
    print("  its plugin api version is %d" % reported)
    if reported != KNOWN_API_VERSION:
        print("  the engine in this checkout uses api %d, so this dll was built against" % KNOWN_API_VERSION)
        print("  a different plugin api and would not be registered.")
        return 1

    try:
        entry = library.nvgt_plugin
    except AttributeError:
        print("  loaded and versioned, but has no nvgt_plugin entry point")
        return 1
    if os.environ.get("MIDI_PROBE_ENTER", "1") == "0":
        print("  nvgt_plugin() not called (MIDI_PROBE_ENTER=0); the file itself is fine")
        return 0

    entry.restype = ctypes.c_bool
    entry.argtypes = [ctypes.c_void_p]

    shared = Plugin_Shared()
    shared.version = KNOWN_API_VERSION
    print("  calling nvgt_plugin() with a stub engine...")
    try:
        accepted = entry(ctypes.byref(shared))
    except Exception as error:  # noqa: BLE001 - the point is to print anything
        print("  the entry point crashed: %r" % (error,))
        print("  A write to address 0 means the plugin got as far as running its own")
        print("  code and then dereferenced a null pointer - which is not something")
        print("  the loader can cause, and not something nvgt would ever have shown")
        print("  you. The file it loaded is genuinely this plugin; the fault is")
        print("  inside it (or inside the stub engine it was handed).")
        return 1

    if accepted:
        print("  the plugin accepted the stub engine and registered its types.")
        print("")
        print("  VERDICT: the dll and the plugin api are both fine. Whatever is wrong")
        print("  is on the nvgt side: the dll is in a folder nvgt does not search, or")
        print("  the engine reports an api version different from %d." % KNOWN_API_VERSION)
        return 0

    print("  the plugin REFUSED the stub engine, so it printed the reason on stderr")
    print("  just above this line (nvmidi names the api version it was built")
    print("  against there). If nothing appeared above, the refusal is the api")
    print("  version check.")
    return 1


def main(argv):
    if len(argv) > 1:
        candidates = [argv[1]]
    else:
        here = os.path.dirname(os.path.abspath(__file__))
        candidates = [
            os.path.join(here, "nvmidi.dll"),
            os.path.join(os.path.dirname(here), "lib", "nvmidi.dll"),
            r"c:\nvgt\lib\nvmidi.dll",
        ]

    if sys.maxsize <= 2**32:
        print("note: this is a 32-bit Python; the plugin is a 64-bit dll and cannot")
        print("      be loaded here whatever else is true. The file checks below")
        print("      still run, the load check does not.")
        print("")

    worst = 0
    for path in candidates:
        status = probe(path)
        worst = max(worst, status)
        print("")
    return worst


if __name__ == "__main__":
    sys.exit(main(sys.argv))
