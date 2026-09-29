# Probe result — run 36541458301 (commit 6dcc6e5)

## What the run measured

    the engine shipped no nvmidi.dll of its own
    probe engine exited 0
    PROBE_BEGIN PROBE_OTHER_BEGIN <garbage> PROBE_OTHER_END <garbage> PROBE_END

## What this settles, by measurement

1. The dll name collision is ruled out. The installer ships no nvmidi.dll.
   The engine used to exit -1073740791 (0xC0000409) before `main`; with the
   name-collision workaround in place it exits 0 and runs the script. The
   startup crash was the collision — two modules claiming `#pragma plugin
   nvmidi` under one filename.
2. The plugin DOES execute. `PROBE_BEGIN` and `PROBE_END`, both literals from
   the script, are intact and the engine exits 0.
3. Every string that crosses the boundary from the plugin is garbage:
   - `midi_api_name()` — built as `"nvmidi/" + RtMidiIn::getApiDisplayName(apis[0])`. Its "nvmidi/" prefix is absent from the output.
   - `midi_output_port_name(0)` — a control with no expression in it, returning
     what `RtMidiOut::getPortName()` hands back. Garbage too.
   Both registered identically: `asFUNCTION(fn), asCALL_CDECL`, declared
   `string`.

## Revised conclusion

The two probed functions differ in construction style and both fail, so the
`operator+` / temporary lifetime theory (which a prior commit message stated as
settled) is falsified. What remains is the return path itself: a C++
`std::string` returned from a plugin into Angelscript's `string`.

I am instrumenting the plugin to print, from inside `midi_output_port_name`,
whether RtMidi even got called and what it returned. Until that lands, the
cause is a hypothesis, not a finding.

## Second measurement — run 36544610694 (commit 4a11831)

    NVSTR plugin_angelscript=2.39.0 WIP (23900)
    NVSTR GetStringFactory ret=67108876  (-2 = asINVALID_ARG)  typeModifiers=0  factory=0x16c79b147c0

The engine was asked for its string type and factory and returned
asINVALID_ARG. NVGT's engine does not publish a string factory: its `string`
is a built-in reference type with its own layout, not an asIStringFactory
string.

So a plugin function returning `std::string`, registered as `string f()`
with asCALL_CDECL, hands back bytes in the wrong layout. Literals written in
the script are built by the engine and are fine; every value that crosses out
of the plugin is not. That is the observed split, exactly.

The fix is not in nvmidi.cpp's functions - they return correct values. It is
in how a std::string is converted into the engine's string type on return.
