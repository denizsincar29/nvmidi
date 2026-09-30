# nvmidi — state of the investigation

Last updated: 2026-09-30, after run 36779797186 (the buffering-story
retraction, and the probe loop cut to the cases whose load the run verifies).

## The root cause, found and fixed

`midi_message` holds a `mutable std::string buffer` and was registered with
`asOBJ_POD`. AngelScript asks a subtype for a default constructor only when
the flags carry `asOBJ_VALUE` and **not** `asOBJ_POD`
(`third_party/angelscript/scriptarray.cpp:142`). So the engine had been told
the type needs no constructor while the type it describes has a destructor
and a copy constructor — and anything that builds one by copying bytes,
`array<midi_message>` included, gets a `std::string` that was never
constructed.

This is also why the compile errors looked like they were about the
*declaration*: `Expected ';'` after `midi_message m;` is the engine refusing
to promise a default constructor it was told does not exist, and the error
lands on whichever line the next statement sat on.

Fix, commit 99a109b: drop `asOBJ_POD`, keep `asGetTypeTraits<midi_message>()`.
`midi_duration` was already registered that way (line 2529) with a comment
saying why; `midi_message` was the one that never got the same treatment.

## The measurement it came from — run 36768219053

The crash stack, from cdb inside the e2e job. Three frames in the plugin,
then nvgt.exe:

    (1a88.21e8): Access violation - code c0000005 (first chance)
    nvmidi!midi_output_port_count+0x7b5f
    nvmidi!midi_output_port_count+0x7d23
    nvmidi!midi_output_port_count+0x7f27
    nvmidi!midi_output_port_count+0x96      <- this address sits in nvgt.exe
    nvgt!asCreateLockableSharedBool+0x52f9d
    ...

The `+0x96` frame is the tell: `midi_output_port_count` is a handful of
instructions, so an offset past its end means the plugin's IAT import stub,
i.e. the plugin calling *into* the engine. `crash_e2e.txt` is cdb's console
transcript, the same trace the step already prints inline.

The probe split is what named the type, not the failing call:

    exit 0    q_engine, d_call, l_int, m_str, k_lit, n_handle2
              (q_engine: QE_PROBE=10 QE_PORTS=1 QE_OK, in all four layouts
               with the array addon; QE_PORTS=11 without it)
    exit 0x41 n_type, n_array, n_sized, a_bare, c_handle, e2e_types
              every one a Compilation error, "Expected ';'"

Every script that touches a **value type** dies; every script that does not,
runs. `midi_note` is `asOBJ_REF|asOBJ_NOCOUNT` (a handle type) — which is why
the reference half of the plugin has always worked and the value half never
did.

Registration itself was clean throughout: `calls=153 refused=0`, all six
crumb groups reached the end of the function, and the heartbeat inside the
dying process ran five beats. The instrument was right; the fault was never
in a call.

## Given up as not the cause

- `getpid` / std::getpid — two CI round trips spent looking up a name that
  does not exist. The only build error this file has produced.
- `shared` use-after-free. The engine `free(shared)`s the struct after
  `plugin_main` returns (nvgt_plugin.cpp:84). Real, but reachable only after
  a clean return, so it cannot be this crash.
- The third-party comparison user asked for: `ethindp/nvgt_plugins`'s online
  path is `prepare_plugin(shared)` then straight `shared->script_engine->
  Register…` calls, ending at `plugin_main` exactly as ours does. Upstream
  offered no candidate — correctly, because the difference was in our source.

## Open

- Watch runs 36769172127/141/149 (nvgt / windows / python on 99a109b).
- `midi_message.buffer` is dead weight now: nothing string-shaped is handed
  to the engine any more, and `to_string_byte`/`to_string_byte_count` were
  removed from the registration. Worth deleting the member outright.
- `NVGT_MIDI_LOAD_PROBE_DIR` never reaches the launched process, so probes
  write to cwd. Fix or drop the variable.
- The heartbeat reader prints content without a closing marker, so an absent
  `--- end of nvmidi-heartbeat.log ---` is indistinguishable from a
  truncated log. Move the verdict into a finally-equivalent.
- `build_plugin.sh`'s engine-compile loop has never run in CI (no caller sets
  `ANGELS_SRC_DIR`).
- Octave echo (the feature Дениз asked for): `midi_output::send_transposed`
  exists at src/nvmidi.cpp:1029 and the comment at :1023 says this is what it
  is for. Not yet wired to an input→output path or tested.
- PROBE-FINDINGS.md and README-PROBES.md still disagree.

## Not sent to Дениз yet

Self-corrections owed, in his words and all at once when there is a live
Telegram context:

1. `getpid` — two CI round trips spent on a name I could have looked up in
   the header that is sitting in this repo.
2. The verdict "nvmidi-load.log does not exist, so the plugin never reached
   its first statement" was **false**. The plugin reaches its first statement
   in every run; the probe wrote to cwd because the env var never reached the
   child. Checking one path is not a measurement of absence.
3. The build failure was a missing include plus two missing declaration
   families, not the "silent step at the end of a chain" I first claimed.
4. The sweep invalidation, retracted.
5. **The buffering story was half wrong.** I told him a 0 byte
   `logs_e2e.txt` was the crash's fingerprint because a dying process skips
   the stdout flush. Run 36779797186 shows `e2e_min` exiting **0** with the
   same 4783 bytes on stderr as every crashing probe, and `n_inputonly`
   printing 440 bytes to stdout with **0** on stderr - so the 4783 bytes are
   the plugin's registration trace on every load, not a fault symptom. The
   buffering half is real; the "fingerprint" half was mine and it was wrong.
   It is corrected in `windows.yml`, in `e2e_winmm.nvgt` and above, and the
   probe loop is cut from twenty-three cases to six because twelve of the
   others only ever measured the compiler's refusals or the same
   registration baseline.
6. **`probe_pragma_only` was never killed at a 60-second limit** - it exits 0
   in 0.6 s (run 36779797186). I wrote that it was cut short. It was not.
7. **`e2e_winmm.nvgt`'s "the script never started" guard is not evidence the
   plugin failed.** The job's guard says `nvgt did not run it at all` while
   `e2e_min` runs to a clean exit on the same runner, so the guard is
   measuring the death, not the load - and the death is now sited earlier
   than the line-542 create call I had named.

## The reader, resolved (512c960)

`midi_message` carries an `opAssign` behaviour now, and that is what the
engine was asking for all along. A call to `next_message(midi_message&out)`
compiles to a copy of the argument into the caller's object, and this engine
routes that copy through the type's assignment operator: measured on run
36772964284, "No appropriate opAssign method found in 'midi_message' for
value assignment" on the caller's line, with the script failing to build.

`midi_duration` already carries two opAssign behaviours for the same reason,
which is the clue that was sitting in the file from the start.

The handle form (74c3ed5) was an attempt to avoid that behaviour and it
cannot work in this engine: a handle is formed only for a type carrying
asOBJ_REF, asOBJ_ASHANDLE, asOBJ_FUNCDEF or asOBJ_TEMPLATE_SUBTYPE
(asCDataType::MakeHandle in the SDK), and midi_message is asOBJ_VALUE
because it holds a std::string. Registering it as a handle produced "Object
handle is not supported for this type" on every script in the repository,
including two that never name the reader.

Green again on run 36773886009: scripts and e2e-midi both success,
`calls=157 refused=0`, all five scripts compiling and running. The only
FAIL inside the log is the e2e script's own `E2E_NO_BUS` - the runner has
the Dummy backend and lists no ports, which is the machine, not the plugin.

## What green does NOT mean (measured this turn)

The e2e-midi job succeeded on 512c960, and that success covers *less* than
it looks like. From its own log:

    kernel: 6.17.0-1022-azure
    modules on this machine that mention sound:
      none - the runner ships a kernel with no sound subsystem
    modprobe: FATAL: Module snd-seq not found in directory /lib/modules/6.17.0-1022-azure
    the sequencer cannot be opened: No such device or address
    ##[warning]the kernel has no snd-seq, so no note can be played into a port

The job is built to stand down when NVGT_MIDI_SINK=none, and that is the
branch every run so far has taken. The consequences, in terms of what is
actually known:

- Registration and compilation: proven. calls=157 refused=0, six types, all
  five scripts build and run.
- The ALSA backend is real, not the Dummy stub: proven by the separate
  backend.nvgt step.
- That a note sent in from another process is read out of the queue
  (E2E_HEARD): NEVER MEASURED. No message has ever entered this plugin.
- That a message sent out reaches a port (SENT/PLAYED): NEVER MEASURED.
- The octave echo, end to end: never measured, and it cannot be measured on
  a machine with no sequencer.

So the plugin currently has a proven *type surface* and an unproven
*transport*. Those are different things and the green build does not
distinguish them.

## Where the sequencer could come from

The github-hosted ubuntu runner will not have it: the kernel image carries
no sound modules and the workflow has already measured that twice.

1. A self-hosted runner on hardware that has snd-seq (Deniz's Pi, or the
   VPS if it allows module loads). Cheapest correct answer, and the same
   runner serves the octave-echo test on real hardware.
2. A container whose kernel is the host's and which can modprobe snd-seq /
   snd-virmidi. "Container" does not by itself help: a container shares the
   host kernel, so it inherits exactly the same absence. The requirement is
   a *host* with the sound subsystem, not a container.
3. Port the transport assertions onto a userspace loopback that needs no
   kernel sequencer. This tests the plugin against itself and is therefore
   weak evidence - say so in the log rather than letting it read as proof.

Recommendation: (1), plus (3) as a smoke test that at least exercises the
code paths between runs. (3) must never be presented as "the note arrived".

## Still open after 512c960


- Windows e2e run 36773886116 was still in_progress when this was written.
  The last Windows run this tree has measured is 74c3ed5, and it failed -
  but 74c3ed5 is the handle detour, so it says nothing about 512c960.
- The transport half of the plugin has no instrument at all (see above).
  This is the largest open item in the file, larger than the handle
  question was: every MIDI claim the readme makes is currently untested.
- `logs/` is not uploaded as an artefact, so the running `logs_e2e.txt` that
  the Windows job prints cannot be read back after the fact. Worth an
  upload-artifact step before the next Windows attempt, or the same
  guessing starts over.
- A compiler warning worth clearing: `src/nvmidi.cpp:748` memsets a
  midi_message over a std::string member (`-Wclass-memaccess`). It is
  pre-existing and on a path that predates the buffer, but it is exactly the
  class of fault the POD flag caused.

## The hearing test (this turn)

`scripts/hear_test.nvgt` and `scripts/hear_test.py` exist now, and they are
the answer to the one question ci cannot ask. The script measures both halves
of the transport on a machine that has a sequencer and a person at it: every
message the instrument sends is printed and spoken, and then the same bytes
are sent back out an octave higher - the feature that was asked for.

Design, and the reasons that are not obvious:

- It speaks through `screen_reader_speak` and not through the engine's alert
  box. `alert` waits for a person to press its button, and a voice prompt that
  must be dismissed before the next one is heard turns an arpeggio into a queue
  of dialogs. `e2e_midi.nvgt` avoids the reader for the opposite reason: on a
  build machine with no reader attached the call never returns.
- The port is chosen through `midi_config.txt`, which is how the plugin's own
  parser works - `match` is a substring matched case-insensitively against the
  port names (`midi_config::load`/`pick`, src/nvmidi.cpp:1525 and :1561), and
  `port`/`index` are an index. The earlier draft of this script called
  `config.set_input_port()`, which does not exist; there is no such method.
- Phase two reopens both ports through configs of their own, because a port is
  chosen by the config its object was opened with, and because playing into the
  port that is being read would read the echo back as input for ever.
- The launcher exists for one Windows reason: nvgt opens its own console and
  hides it behind the game, so a test that talks to the console is invisible
  exactly when it matters. It reads that console back, prints each line, and
  turns the transcript into a verdict that separates measured from reported.

Neither file has run on hardware yet. Nothing in this repository has ever
carried a byte between two processes - that is the whole reason they exist.

## The buffering/registration-trace story, and its retraction

The Windows run has been failing with `nvgt exited -1073741819 (0xC0000005)`
after about a second for a long stretch of runs, and a reading of it was
built around the file that was always empty. Part of that reading is now
retracted by the run's own numbers.

**What was claimed.** The engine's `print()` buffers, and its buffer is
flushed at a clean exit. `scripts/windows/e2e_min.nvgt` - a `#pragma plugin
nvmidi` and two prints - exits 0 leaving `MIN_BEGINMIN_END` in stdout,
sixteen bytes, no trailing newline. That much is still true. The claim that
followed is not: that a *crashing* process skips that flush, so that the 0
byte `logs_e2e.txt` beside the 4783 byte stderr file is the crash's own
fingerprint.

**What run 36779797186 measured.** From the probe loop's own printed line
(`exit {code} after {sec}s, stdout {n} bytes, stderr {n} bytes`):

    e2e_min           exit 0 after 0.6s, stdout 16 bytes, stderr 4783 bytes
    l_int, m_str, k_lit  exit 0,            stdout  4 bytes, stderr 4783 bytes
    i_var             exit 0,               stdout  7 bytes, stderr 4783 bytes
    g_float           exit 0,               stdout 12 bytes, stderr 4783 bytes
    n_handle2         exit 0,               stdout 72 bytes, stderr 4783 bytes
    probe_pragma_only exit 0,               stdout 21 bytes, stderr 4783 bytes
    n_type1, n_ctor   exit 65 after 0.2s,   stdout  0 bytes, stderr    0 bytes
    j_pragma          exit 65,              stdout 94 bytes, stderr    0 bytes
    n_noteonly        exit 65,              stdout 254 bytes, stderr   0 bytes
    n_inputonly       exit 65,              stdout 440 bytes, stderr   0 bytes

Two things fall out and neither can be argued with:

- `e2e_min` exits **0** with **the same 4783 bytes on stderr** as every
  crashing probe, and all eight of those `.err` files are byte-identical
  (md5 `20775200`). A clean exit and a crash cannot share one stderr file if
  that file is the crash's own trace.
- The sizes are mutually exclusive. `i_var` reports stdout 7 with stderr 4783;
  `n_inputonly` reports stdout 440 with stderr **0**. If a death before the
  flush produced the 4783 bytes, `n_inputonly` could not have zero.

The 4783 bytes are the plugin's **registration trace**, emitted on every load
of the library, and they are the baseline of the run rather than the symptom
of anything. They say one thing and it is worth saying: the plugin
registered. Registration completes and the script runs to a clean exit 0 for
at least eight probes - that is the safe baseline, and it is established by
the run itself rather than by a grep.

**What replaced it as the read of the crash.** A 0 byte `logs_e2e.txt` now
means only that the process died before any print reached the file. The
probes that die at 0.2 s with a 0-byte stderr die *before* the registration
trace exists at all, i.e. before the plugin's load-time work is done; and the
plugin's own heartbeat log (`scripts/windows/nvmidi-heartbeat.log`, 112
bytes) shows the static constructor entered, a heartbeat thread started, a
beat at ms=0 and another at ms≈251 - then silence, while the process lives on
to 1.1 s. That points at the plugin's load-time path or the engine's startup,
which is earlier than the `midi_output_create()` call the previous session
had designated as the control. That is a hypothesis and is marked as one; it
is not measured yet.

**`detach_probe` is the sharpest instrument this tree has on the crash.**
Added back to the probe loop in the cut-down list, it reports the engine's own
exit line for a plugin the engine has *detached*:

    detach_probe  exit -1073741819 (0xC0000005) after 1.2s,
                  stdout   0 bytes, stderr 4783 bytes   (run 36780860938)

Three things follow from that one line, and they are the first measurements
the windows job has produced about the death itself:

- The 4783 bytes are present on a run that dies, in a step whose library is
  the **detached** build. The trace appears whether the process lives or
  dies, which is the retraction above stated a third way.
- The death is reproducible with the plugin detached, so it is not the
  registered plugin's own code running. Whatever dies, dies at load.
- `stdout 0` against `e2e_min`'s `stdout 16` on the same runner is the
  measured form of "died before any print reached the file" - not a
  fingerprint, just an ordering: the death precedes the script's first line,
  and `e2e_min`'s sixteen bytes are the proof that the harness *can* print
  when it gets that far.

The rest of the cut-down loop behaved as the retraction describes: `e2e_min`
exit 0 / 0.4 s / 16 / 4783, `probe_pragma_only` exit 0 / 21 / 4783,
`n_handle2` exit 0 / 72 / 4783, and the two compiler refusals `n_noteonly`
(254 / 0) and `n_inputonly` (440 / 0) unchanged. The trim changed the round
trip, not the answer.

**Superseded claims corrected in the tree.** Commit `6ca957c` said the
compile pass truncated `logs_e2e.txt`; run 36776804229, on that very
revision, still reports 0 bytes, which a pass writing elsewhere cannot
produce. And the sentence "a 0 byte logs_e2e.txt IS the crash's fingerprint"
is gone from `windows.yml` and from `e2e_winmm.nvgt`. The `can_flush` /
`fflush()` refusals stand as measurements: `can_flush = true;` was refused
with "ERROR: No matching symbol 'can_flush'" (exit 65, run 36778136862) and
`fflush()` the same way (run 36710433374). A script still cannot turn the
buffer off; that part was never in doubt.

## What green still does not mean

Unchanged by any of this, and worth keeping beside it: no build machine has
ever carried a MIDI byte. The GitHub runner's kernel has no sound subsystem,
so the transport has no instrument there at all, and the octave echo asked for
in message 5506 is two unmeasured halves stacked on each other. A green e2e
would prove the type surface and the port enumeration. The hearing test is
where the transport gets measured, on the one machine that has a sequencer and
a person sitting at it.
