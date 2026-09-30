# Probe findings

Measured on GitHub Actions runners via `.github/workflows/windows.yml`.
Every line below names the run it came from. Nothing here is inferred from
the source; where a reading is a hypothesis it says so.

## What the runner does with a script that does not compile

nvgt writes its AngelScript diagnostics to **stdout** and leaves stderr at
0 bytes, with exit code **65 (0x41)**. Measured on every probe that fails to
compile, across runs 36612782413, 36613545609, 36614248662.

That is why `logs_e2e.txt.err` was always empty: the fault was never on that
stream. The file that carries it is the plain `.log`.

## Line attribution

Raw stdout of `c_handle.nvgt` (run 36614248662):

    Compilation error: file: .../c_handle.nvgt
    line: 3 (1)
    INFO: Compiling void main()
    file: .../c_handle.nvgt
    line: 5 (13)
    ERROR: Expected ';'

and the same again with `ERROR: Instead found '@'`.

The script is 7 lines: 1 `#pragma`, 2 blank, 3 `void main() {`, 4 the
`print("C1");`, 5 the handle declaration, 6 `print("C2");`, 7 `}`.
`line: 3 (1)` is the opening brace of `main`. `line: 5 (13)` is column 13 of
line 5 — which is the last character of `print("C1")` plus its semicolon.

The engine reports the fault at the closing semicolon of the statement
**before** the one it cannot parse, and names the offending token — `@` —
from the statement after it.

`a_bare` and `b_decl` print the identical pair from the same column, and they
open differently (`midi_output@ out = midi_output();` and three bare
declarations respectively). So the fault travels with the plugin's type
appearing in a declaration, not with any particular statement.

## The pragma is not the cause

`j_pragma.nvgt` (`#pragma plugin nosuchplugin_zzz`) exits 65 with a
different diagnostic entirely: `Compilation error: file: nosuchplugin_zzz`,
`line: 0 (0)`, `ERROR: failed to load plugin`. A pragma that cannot be
satisfied does not produce the `@` pair.

This is counter-evidence to the hypothesis that a load failure clears the
registered types before the script is parsed. A missing plugin is reported as
a missing plugin.

## i_var

`i_var.nvgt` is four lines: pragma, blank, `void main() {`, `string h;`,
`print("I_BEGIN");`. It exits **0** with 7 bytes on stdout: `I_BEGIN`.

An earlier reading of this file recorded 8 lines of leading `//` comments and
the marker `H_BEGIN`. Both were wrong: the file in the checkout carries no
comments and prints `I_BEGIN`, which is what the runner measured. The
`H_BEGIN` reading came from a version that no longer exists.

## The real script

`E2E_RAN_SECONDS: 0.2` and `logs_e2e.txt: 0 bytes` on every run of this
session (`36a2bb9`, `34d35b9`). Its `E2E_EXIT_HEX: 0xC0000005` is not a
measurement of the script: that wait step times out without writing
`E2E_EXIT_CODE`, so the value printed is the previous step's environment
leaking forward. The step's own log line says "the script never named a port
within 60s".

The reason it always sat the full 60 s is a malformed regex — `'E2E_PORT\s'`
with no capture, `'E2E_END'` unanchored — both of which are true from the
first read of an empty file. The step never broke early and never printed the
log it was there to print. Corrected in 776770d.

## Probes measured, with their verdicts

| probe | exit | stdout |
|---|---|---|
| `l_int` | 0 | `L_OK` |
| `m_str` | 0 | `M1M2` |
| `k_lit` | 0 | `K1K2` |
| `c_handle` | 65 | 428 B, `@` at line 5 (13) |
| `e2e_min` | 0 | `MIN_BEGINMIN_END` |
| `i_var` | 0 | `I_BEGIN` |
| `j_pragma` | 65 | `failed to load plugin`, line 0 (0) |
| `e2e_types` | 65 | 434 B, `@` at line 14 (13) |
| `a_bare` | 65 | 420 B, `@` at line 5 (13) |
| `b_decl` | 65 | 420 B, `@` at line 5 (13) |
| `d_call` | 0xC0000005 | 0 B |
| `e_call` | 0 | `E1E2 api_bytes=25E3` |
| `f_open` | 0xC0000005 | 0 B |
| `g_float` | 0 | `G_BEGING_END` |

`l_int`, `m_str` and `k_lit` were added in 776770d and their column is filled
from that run. They exist to separate "the plugin type" from "a statement
after a print" as the thing the parser refuses.


## k_lit decides it

Added in 776770d, measured on run 36615043830.

`k_lit.nvgt` is `print("K1");` then `int k = 1;` then `print("K2");` — the
same three statements in the same order as `c_handle`, with an `int` where
the handle was. It exits **0** and prints `K1K2`, 4 bytes. `m_str` does the
same with a `string` declaration between two prints and also exits 0 with
`M1M2`.

So a declaration between two prints is not what the parser refuses, and the
name is not it either. The fault is the plugin's type: `midi_output@`, the
handle the plugin registers, cannot appear in a declaration the engine
accepts.

`l_int` declares an `int` in a script that requests the plugin and exits 0,
which rules out the request itself as the cause.

The remaining question is what the plugin did wrong when it registered that
type. The engine says `Expected ';'` and `Instead found '@'`, which is what a
parser says when the token before the `@` is not a type it knows: the
registration that should have made `midi_output` a type name did not take
effect in this script, while the calls that need no such name
(`midi_api_name_byte_count()`, exit 0, `api_bytes=25`) still resolve.

## The array add-on takes the native path, and that is what kills the build

Measured, run 36719278988, in both directories that reach the call:

    nvmidi: asGetLibraryVersion=2.39.0 WIP
    nvmidi: asGetLibraryOptions= AS_64BIT_PTR AS_WIN AS_X64_MSVC

The options carry no `AS_MAX_PORTABILITY`. So the branch test in
`RegisterScriptArray` — `strstr(asGetLibraryOptions(), "AS_MAX_PORTABILITY") == 0`
(scriptarray.cpp:274) — is true and `RegisterScriptArray_Native` runs.

That function's first statement is

    engine->SetTypeInfoUserDataCleanupCallback(CleanupTypeInfoArrayCache, ARRAY_CACHE);

(scriptarray.cpp:292). It is the add-on's first call into the engine, and the
process dies inside it: stderr shows `calling RegisterScriptArray` then
`system exception`, with no `RegisterScriptArray returned`.

### It is a second registration, not a first

The control build never calls the add-on at all, yet it registers every
function `nvmidi` has — and those that take an `array<midi_note>` succeed
(reg lines 1836..1841 are the `midi_message` set, and `play_chord` registers
after it with no complaint). So the engine already knows `array<T>` before the
plugin loads. NVGT's own loader has installed an array add-on already. The
plugin's call is the second one over an existing registration.

### Settled: the vtable is sound, so the fault is the registration itself

Run 36721024269, control build, locations 1 and 2:

    exit 0  stdout=[QE_PROBE=10QE_PORTS=1QE_OK]

`midi_engine_probe()` is a global function the plugin registers, and it does
nothing but call `GetEngineProperty((asEEngineProp)30)` through `g_engine` —
the same pointer, of the same type, through which the add-on makes its call.
It answers **10**, a legitimate value of `asEP_INIT_CALL_STACK_SIZE`, and the
same process then calls `midi_output_port_count()` and gets **1**.

So the plugin's `asIScriptEngine*` resolves its vtable correctly and the
property slot this header assigns really is that engine's property slot. The
plugin's engine calls work. The fault is not at the call boundary.

The same cell settles the second half: the control build registers
`midi_engine_probe` at line 2117 and keeps going to 2138, so the plugin
called nothing of the add-on's and lived. The ordinary build calls
`RegisterScriptArray` at the same point and dies. The difference between the
two builds is exactly that call.

**The duplicate array-addon registration is what kills the build** — not the
vtable, not the load location, not the pragma.

### It is the second registration, and a second one is the fault

`RegisterScriptArray_Native`'s first statement is the cleanup-callback call
above; the add-on registers `array<T>` and a cleanup callback over a type the
engine already has. The control build proves the engine has it: every nvmidi
function taking `array<midi_note>` registers with no complaint there. What
the process refuses is the *second* registration, not the type name being
known twice — `SetTypeInfoUserDataCleanupCallback` on an already-populated
cache is where it goes.

The fix that follows: the plugin must not call the add-on at all when the
engine already has the arrays. `NVGT_SKIP_ARRAY_ADDON` is the control build;
that path works end to end. The unsolved half is detection — deciding at
runtime whether the engine already has `array<T>`, since the add-on call
cannot be made twice and cannot be made speculatively.

## Retracted

- **A "60.4 s hang" of the real script.** There was no hang. The step above
  could not end and the process was observed only at the deadline. See the
  regex note above.
- **"The call boundary is what takes the process down."** Written into the
  message of commit 36a2bb9. `c_handle` makes no plugin call at all — it has
  three statements, one print, one declaration, one print — so it cannot die
  on one. The `0xC0000005` cited as evidence for it was the env leakage
  described above. The error was mine and is corrected here.
- **A `h_decl`/`c_handle` reproducibility fault.** Refuted: the worktree
  bytes, the committed blob and the GitHub blob hash identically for
  `c_handle`, `h_decl`, `g_float` and `e2e_min`. The runner got the committed
  bytes.

## Closed

- `midi_duration` and `asCALL_CDECL_OBJLAST` as the fault.
- Global initialisers as the fault: `g_float` declares nine of them, including
  an array handle, and exits 0.
- "One named type per script body": `e2e_types` and `a_bare` each name one.
- A missing plugin DLL: `e2e_min` requests the plugin and exits 0.

## Still open

- Why a declaration of a plugin handle is refused while a declaration of
  `string` in the same position is accepted. `k_lit` is the probe that
  decides whether the refusal is about the type at all.
- `open-failed slot=midi1 error=0` and `W_VERDICT input=false` — the question
  the whole apparatus exists to answer.
- The loopback driver was not built on run 36603915214; the listener's verdict
  is not trustworthy until that is understood.


## The host runs: the plugin registers end to end against a live engine

Measured on the VPS (denizsincar.ru, x86_64 Debian 12, g++ 12) on 2026-09-30,
not on a GitHub runner. Build:

    g++ -std=c++17 -O1 -fPIC -Isrc -Ithird_party/rtmidi -Ithird_party/angelscript \
        -DNVGT_PLUGIN_STATIC tools/probe_host.cpp src/nvmidi.cpp \
        third_party/rtmidi/RtMidi.cpp third_party/angelscript/scriptarray.cpp \
        libangelscript_239.a -lpthread -Wl,--allow-multiple-definition -o probe_host

`libangelscript_239.a` is built from upstream master, which reports
`ANGELSCRIPT_VERSION 23900` - the same number the plugin vendors in
`third_party/angelscript/angelscript.h:61` ("2.39.0 WIP"). This is load-bearing
and was the last blocker: `asCreateScriptEngine` defaults its argument to
`ANGELSCRIPT_VERSION` and returns 0 when the caller's major/minor differ from
the library's (as_scriptengine.cpp:233), so a 2.38.0 library answers a 2.39.0
header with a null pointer and the message "could not create an Angelscript
engine". The first archive built here was 2.38.0 and produced exactly that.

Result:

    the plugin's api version is 5
    a real Angelscript engine was created (version 2.39.0 WIP)
    calling the entry point, with a real engine and no stub table...
    the entry point returned true
    REGISTERED: the plugin ran its whole registration against a live Angelscript
    the plugin asked the nvgt side of the table 0 time(s)
    PROBE_EXIT=0

**The nvgt-side count is the claim this host was built to test and it holds:**
through the entire registration the plugin never reached for `nvgt_wait`,
`ticks`, `refresh_window` or any other engine-supplied function. Everything it
needed came from Angelscript.

### The array add-on question is answered, and answered at run time

    nvmidi: blind probe array<int> answered -8
    nvmidi: blind probe array<uint> answered -8
    nvmidi: blind probe array<double> answered -8
    nvmidi: the engine already has array<T>, the array add-on is not called

-8 is `asNO_TYPE_INFO`. The plugin asks the engine whether it knows the array
types before registering them, gets a clear no from a bare host, and registers
the add-on itself; against nvgt, where the types exist, it skips. This is the
detection the "Still open" section below said was unsolved - it is solved, and
it was in the tree while that paragraph was being written.

### 22 refusals, all one code, and what they do *not* say

Every refused registration answers **-10**, and -10 is `asALREADY_REGISTERED`
(angelscript.h:655). The plugin's own trailer text - "at least one registration
was refused for a reason other than asALREADY_REGISTERED" - is wrong: nothing
was refused for any other reason. That sentence is what sent this session
looking for a second class of failure that does not exist.

The refusals are all **second and later registrations of a name that is already
in the engine**: `nvmidi_note_create(int,int)` is registered twice in the source
(2103 and 2109), `midi_note_number` twice (2108 and 2112), `midi_config`'s
`match` property twice, its `load`/`load_if_present` twice, the two port lookups
twice. The lines that *introduce* each type - `midi_note` at 1952, `midi_output`,
`midi_input` - are not among them, and neither is `midi_config`'s first
property.

Which gives the host its reading, and the reading is the useful part:

**A name that appears for the first time cannot be already registered.** So a
-10 against a first-time name would mean the *type* was rolled back and the
whole type is unusable - the fault a script hits when it writes
`midi_output@ out;` and is told the type does not exist. A -10 against a
duplicate only means the duplicate is redundant. The log above holds only the
second kind, and `register_nvmidi` is entered exactly once (`grep -c` = 1), so
the plugin registers once and some of its declarations are literally repeated in
the source.

Two things follow, and they are different sizes:

- **Cosmetic, in this repo:** the duplicate registrations should go, and so
  should the trailer sentence, which reports a category of error it never checks
  for.
- **Load-bearing, for nvgt:** "genuinely already registered" and "rolled back"
  share one error code, so a plugin cannot tell them apart locally. That is the
  whole reason the bound-type mystery below stayed unsolved for a day of
  scripting: a failed registration at load and a successful one are the same
  answer to the next question.

## Corrected in this section

The note in `tools/probe_host.cpp` saying the harness "has never once compiled"
- and the same claim repeated in this file's history of messages - was true when
written and is not true now. It compiles, links and runs; the three faults in
it were a missing `#include <RtMidi.h>`, stand-ins that collided with the
header's own forward declarations, and a hand-written `extern "C"` declaration
of the entry point where the header, in this mode, declares it C++ and mangled.
