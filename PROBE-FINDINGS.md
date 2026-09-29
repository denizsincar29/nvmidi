# The string boundary — what was measured, and what it cost

Three runs, each answering the question the one before it raised. Kept because
the conclusion is counterintuitive enough that someone will otherwise re-derive
it the expensive way.

## Run 36541458301 (commit 6dcc6e5) — rule out the dll collision

    the engine shipped no nvmidi.dll of its own
    probe engine exited 0
    PROBE_BEGIN PROBE_OTHER_BEGIN <garbage> PROBE_OTHER_END <garbage> PROBE_END

The engine ships no `nvmidi.dll` of its own, so the earlier startup crash
(`-1073740791` / `0xC0000409`, before `main`) was a name collision: two modules
claiming `#pragma plugin nvmidi` under one filename. With that fixed the engine
exits 0 and runs the script. But both script literals survive intact while
every value that came out of the plugin is garbage — including
`midi_output_port_name(0)`, which does no string building of its own, just
returns what `RtMidiOut::getPortName()` hands back.

That killed the `operator+` / temporary-lifetime theory a prior commit message
had called settled: the two functions differ in construction style and fail
identically.

## Run 36544610694 (commit 4a11831) — ask the engine about its strings

    NVSTR plugin_angelscript=2.39.0 WIP (23900)
    NVSTR GetStringFactory ret=67108876  (-2 = asINVALID_ARG)  typeModifiers=0  factory=0

`67108876` as a signed int is `-2` = `asINVALID_ARG`. The engine publishes no
string factory. NVGT's `string` is a built-in reference type with the engine's
own layout, not an `asIStringFactory` string.

So a plugin returning `std::string` or `const char*` registered as `string`
hands back bytes in the wrong layout, and the engine writes them where its own
string object is not. The address is nonzero, so it does not fault cleanly — it
corrupts and surfaces later as a stack overrun.

The return *type* was checked and is not the fault: run 36544974785 registered
the function returning `const char*` instead and died at `-1073740791` before
its first print, with no output file at all. No spelling of a string return
survives.

## Run 36546349372 — one process, both ways, same second

This is the decisive one, and the reason this file is short. One process read
the api name through both paths:

    byte path    110118109105100105478710511010011111911532771171081161057710110010597
                 → nvmidi/Windows MultiMedia  (25 bytes, exact)

    string path  PROBE_API_STR 0u??z     (garbage)

Same call, same underlying value, same second. The defect is not specific to
the port name, not to RtMidi, and not to this plugin's string building: it is
the return slot itself.

With the byte path the port name also came through exactly —
`PROBE_BYTE_COUNT 30` decoding to `Microsoft GS Wavetable Synth 0` — and the
process exited 0.

## What the plugin does about it

Integers cross the boundary; strings do not. Every string surface is a pair:
`_byte_count()` returning the length, `_byte(uint index)` returning one byte, or
**-1** when there is no byte at that index. `-1` rather than 0 because a name
may legitimately contain a zero byte, so "no such byte" has to stay
distinguishable from "an actual NUL".

`register_nvmidi()` asks for the string factory and, if a future engine ever
publishes one, prints a warning saying the byte path has become unnecessary and
must be re-measured rather than trusted either way.

See the readme section "Strings arrive as bytes" for the four-line script-side
helper that reassembles the text, where the engine's own strings are intact.

## substr() on a literal, and what the byte path cost to find (measured 2026-09-29)

The byte protocol was never the fault. The backend name the plugin hands over
is right, and the runner now rebuilds it exactly:

    E2E_API_TEXT nvmidi/Windows MultiMedia
    E2E_API_FIND_WINDOWS 7

What was wrong was the test harness rebuilding a string out of those bytes, in
three successive ways, each measured rather than argued:

    string(byte)                gives the decimal digits: a 25 byte name came
                                back as "110118109105100105...".
    byte_letters.substr(b, 1)   is not the character at b. Measured on a 68
                                character literal: index 32 answers 'W', index
                                65 answers '-', index 47 answers 'l', and
                                substr(67, 1) is empty although length() is 68.
                                So in-range reads give wrong characters, above
                                roughly the midpoint they give nothing, and the
                                last character is unreachable. The offsets are
                                not a constant (32→'W' and 65→'-' are different
                                shifts), so the mechanism is still UNMEASURED
                                and is not claimed anywhere in the code.
    byte_letters.find(one)      was blamed for answering 0 for every character.
                                That blame was wrong. Measured: find() answers 1
                                for "1", 10 for "A", 62 for "/", and -1 for a
                                miss. It was never the fault.

The fix that works is not a lookup at all: a branch over the byte, reading only
the first ten and first twenty six characters of a literal (the two ranges this
engine does read correctly, find("1")=1 and find("A")=10), and returning the
punctuation a backend name is made of one byte at a time.

### Engine facts learned on the way

    `b >= '0'`                  "No conversion from 'const string' to math type
                                available" - a character literal is a one
                                character string, not a code. Every bound in the
                                conversion is the number.
    int x[25] = {...}           "Expected '('" / "Instead found identifier" - a
                                sized array with an initialiser list on one line
                                is rejected. Fill it one element at a time.
    find()                      index for a hit, -1 for a miss, 0 for the empty
                                needle: sound.
    probe stdout                lands in the JOB LOG, not in the uploaded
                                artifact. Engine prints carry no trailing
                                newline, so a probe's output has to be split on
                                its own markers, not on lines.
    PowerShell                  Get-Content -Raw on an empty file answers $null
                                and a method call on $null kills the step:
                                guard with `.Trim().Length -gt 0`.
