#!/usr/bin/env python3
"""Launch the nvmidi hearing test and show what came back.

    python3 scripts/hear_test.py                 # pick a port, run, report
    python3 scripts/hear_test.py --list          # just show the ports
    python3 scripts/hear_test.py --in Nord --out synth
    python3 scripts/hear_test.py --nvgt /path/to/nvgt

Why a launcher exists at all: nvgt opens a console window of its own on
Windows and hides it behind the game, so a test that talks to the console is
invisible exactly when it is most needed. This reads that console back, prints
every HEAR_ line as it arrives, and turns the transcript into a verdict.

A verdict is not the same thing as a pass. The two halves of this test are
measured differently, and the difference is the whole point:

  * The input half is real evidence. Every HEAR_MSG line is a byte that came
    out of the keyboard, through rtmidi, through the plugin's queue, into the
    engine's string - the machine that produced it is not the machine being
    judged.
  * The echo half is real evidence too, but only to the person listening.
    HEAR_ECHO note on 60 -> 72 is the plugin saying it *sent* that byte, and
    sending is exactly the half no build machine has ever been able to check.
    Whether the synth made a sound is not in this file and cannot be.

Ctrl+C stops it, and so does escape in the nvgt window.
"""

import argparse
import os
import re
import shutil
import subprocess
import sys
import time

# Where the script and the config live. The config is written next to the
# working directory of the process that runs nvgt, and nvgt resolves the
# script's relative paths against its own working directory too, so both are
# run from the root that holds the script.
#
# The root is found by looking for the script, not by assuming a depth.
# Measured on a real Windows machine on 2026-10-01: run from the flat kit
# (nvgt.exe, nvmidi.dll and hear_test.nvgt in one folder, as the README tells
# people to lay it out), the count-the-parents version built
# ...\pythons\scripts\hear_test.nvgt - the launcher's own folder with
# "scripts" glued on - and nvgt answered "error: Path not found" followed by
# "Nothing chosen, nothing run." The file was right there beside it.
def _find_root():
    here = os.path.dirname(os.path.abspath(__file__))
    for candidate in (here, os.path.dirname(here), os.path.join(here, "scripts")):
        if os.path.isfile(os.path.join(candidate, "hear_test.nvgt")):
            return candidate
    return here


ROOT = _find_root()
SCRIPT = os.path.join(ROOT, "hear_test.nvgt")
if not os.path.isfile(SCRIPT):
    SCRIPT = os.path.join(ROOT, "scripts", "hear_test.nvgt")
CONFIG = os.path.join(ROOT, "midi_config.json")

CANDIDATES = ["nvgt", "nvgt.exe", "nvgt_console", "nvgt_console.exe"]


def find_nvgt(explicit=None):
    if explicit:
        return explicit if os.path.exists(explicit) else None
    for name in CANDIDATES:
        found = shutil.which(name)
        if found:
            return found
    # The official Windows layout, for a copy unzipped next to the test.
    for folder in ("nvgt", os.path.join("..", "nvgt")):
        for name in CANDIDATES:
            p = os.path.join(ROOT, folder, name)
            if os.path.exists(p):
                return os.path.abspath(p)
    return None


def parse_ports(text):
    """Pull the port list out of the script's own console output."""
    ins, outs = [], []
    for line in text.splitlines():
        m = re.match(r"\s*HEAR_INPUT_PORT\s+(\d+)\s+(.*)$", line)
        if m:
            ins.append((int(m.group(1)), m.group(2).strip()))
        m = re.match(r"\s*HEAR_OUTPUT_PORT\s+(\d+)\s+(.*)$", line)
        if m:
            outs.append((int(m.group(1)), m.group(2).strip()))
    return ins, outs


def ask(ins, outs, want_out):
    """Choose a port by number. Input is read with a guard, never by posix."""
    if not ins:
        return None, None
    print("\nMIDI input ports. Pick the one your instrument is:")
    for index, name in ins:
        print("  %d  %s" % (index, name))
    default = 0
    for index, name in ins:
        if "nord" in name.lower():
            default = index
    while True:
        try:
            answer = input("input port [%d]: " % default).strip()
        except EOFError:
            answer = ""
        if answer == "":
            chosen_in = default
            break
        if answer.isdigit() and any(index == int(answer) for index, _ in ins):
            chosen_in = int(answer)
            break
        if answer.lower() in ("q", "quit"):
            return None, None
        print("  %r is not one of the numbers above." % answer)

    if not want_out:
        return chosen_in, chosen_in
    if not outs:
        print("No output ports, so there is nothing to play back into.")
        return chosen_in, None
    print("\nMIDI output ports. Pick the synth you want the echo out of:")
    for index, name in outs:
        print("  %d  %s" % (index, name))
    while True:
        try:
            answer = input("output port [%d]: " % outs[0][0]).strip()
        except EOFError:
            answer = ""
        if answer == "":
            return chosen_in, outs[0][0]
        if answer.isdigit() and any(index == int(answer) for index, _ in outs):
            return chosen_in, int(answer)
        if answer.lower() in ("q", "quit"):
            return None, None
        print("  %r is not one of the numbers above." % answer)


def name_of(ports, index):
    for i, name in ports:
        if i == index:
            return name
    return ""


def write_config(in_name, out_name):
    """Hand the chosen ports over as a config file, not as arguments.

    The script cannot read a command line: it has no accessor for one. Two runs
    measured that - "No matching symbol 'get_argc'" and then "No matching symbol
    'application'" - so the port names travel the way the plugin's own parser
    wants them anyway, as a `match` line per direction. Written fresh each run,
    because the echo half wants the input and the output to be different ports
    and a stale output line from an earlier run would silently decide this one.
    """
    names = [in_name]
    if out_name and out_name != in_name:
        names.append(out_name)
    # JSON, and two "match" keys when the two directions differ: the plugin's
    # reader applies keys in order, so the later one decides - the same
    # last-wins rule the earlier `match=` line used, in the shape every reader
    # in this repository now reads.
    lines = ['{ "match": "%s" }' % in_name]
    if len(names) == 2:
        lines = ['{ "match": "%s", "match": "%s" }' % (names[0], names[1])]
    with open(CONFIG, "w") as handle:
        handle.write("\n".join(lines) + "\n")


def run(nvgt, script, in_name, out_name, echo):
    """Run nvgt, echo its console, and collect the transcript."""
    argv = [nvgt, script]
    if in_name:
        write_config(in_name, out_name if echo else "")
    print("\n$ " + " ".join(argv))
    transcript = []
    try:
        process = subprocess.Popen(
            argv, cwd=ROOT, stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT, text=True, bufsize=1)
    except OSError as error:
        print("Cannot start nvgt: %s" % error)
        return None
    try:
        for line in process.stdout:
            line = line.rstrip("\n")
            transcript.append(line)
            print("  " + line)
    except KeyboardInterrupt:
        print("\nStopping nvgt.")
    finally:
        process.terminate()
        try:
            process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            process.kill()
    return "\n".join(transcript)


def verdict(text, echo):
    """Say what was measured, and only what was measured."""
    print("\n" + "=" * 60)
    if "HEAR_API" not in text:
        print("The plugin never reported itself. Either nvgt did not start,")
        print("or the library is not next to it, or the pragma did not load.")
        return
    api = re.search(r"HEAR_API\s+(\S+)", text)
    if api:
        print("backend: %s" % api.group(1))
        if "Dummy" in api.group(1):
            print("This build is the Dummy backend: it can never reach a port.")

    total = re.search(r"HEAR_TOTAL\s+(\d+)", text)
    messages = re.findall(r"HEAR_MSG\s+(.*)", text)
    if total:
        count = int(total.group(1))
        print("\ninbound: %s messages reached the plugin." % count)
        if count == 0:
            print("That is a measurement, not a failure by itself: either the")
            print("instrument sent nothing, or it is on another port - the")
            print("script printed the ports it could see above.")
        else:
            for line in messages[:5]:
                print("  %s" % line)
            if len(messages) > 5:
                print("  ... and %d more" % (len(messages) - 5))
            print("These bytes came from outside the plugin, so this half is")
            print("measured, not reported.")

    if not echo:
        print("\nThe echo half was not run on request.")
        return
    echoes = re.search(r"HEAR_ECHOES\s+(\d+)", text)
    shifted = re.findall(r"HEAR_ECHO note on (\d+) -> (\d+)", text)
    if not echoes:
        print("\necho: not reached - no message came in during the first phase.")
        return
    print("\necho: %s notes were sent back an octave up." % echoes.group(1))
    if shifted:
        print("The transposition is in the log: %s" % ", ".join(
            "%s to %s" % pair for pair in shifted[:5]))
    print("Whether the synth sounded is the one thing this file cannot say.")
    print("The bytes were sent; the hearing is yours.")


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--nvgt", help="path to nvgt or nvgt_console")
    parser.add_argument("--in", dest="in_name", help="substring of the input port name")
    parser.add_argument("--out", dest="out_name", help="substring of the output port name")
    parser.add_argument("--list", action="store_true", help="list the ports and stop")
    parser.add_argument("--no-echo", action="store_true",
                        help="only measure the inbound half; send nothing")
    args = parser.parse_args()

    nvgt = find_nvgt(args.nvgt)
    if not nvgt:
        print("Cannot find nvgt. Pass it: python3 scripts/hear_test.py --nvgt C:\\nvgt\\nvgt.exe")
        return 2
    print("nvgt: %s" % nvgt)

    # First pass: no arguments, so the script lists the ports and falls back to
    # its own first choice. It is stopped as soon as the list is complete -
    # nothing is measured in this pass.
    transcript = run(nvgt, SCRIPT, "", "", False)
    if transcript is None:
        return 2
    ins, outs = parse_ports(transcript)
    if args.list:
        for index, name in ins:
            print("in  %d  %s" % (index, name))
        for index, name in outs:
            print("out %d  %s" % (index, name))
        return 0

    if args.in_name and args.out_name:
        in_name, out_name = args.in_name, args.out_name
    else:
        in_name, out_name = ask(ins, outs, not args.no_echo)
    if in_name is None:
        print("Nothing chosen, nothing run.")
        return 1
    in_name = name_of(ins, in_name) or in_name
    out_name = name_of(outs, out_name) or out_name
    want_echo = not args.no_echo
    if want_echo and out_name == in_name:
        print("\nNo output port chosen that is different from the input.")
        print("The echo would go straight back into the reader and the test")
        print("would measure itself, so the echo half stays off this run.")
        want_echo = False

    print("\nInput  %s" % in_name)
    print("Output %s" % (out_name if want_echo else "(not used)"))
    print("\nPlay the instrument when the script asks. Ctrl+C stops the test.")
    time.sleep(1)

    transcript = run(nvgt, SCRIPT, in_name, out_name, want_echo)
    if transcript:
        verdict(transcript, want_echo)
    if os.path.exists(CONFIG):
        print("\nThe ports this run used are in %s" % CONFIG)
    return 0


if __name__ == "__main__":
    sys.exit(main())
