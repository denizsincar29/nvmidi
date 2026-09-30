# nvmidi — state of the investigation

Last updated: 2026-09-30, at commit 512c960.

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

## The window into the crash, opened (this session)

The Windows run has been failing with `nvgt exited -1073741819 (0xC0000005)`
after about two seconds for a long stretch of runs, and every reading of it
was taken through a file that was always empty. That file is now explained.

**stdout is buffered by the engine and the buffer does not survive a crash.**
The control is `scripts/windows/e2e_min.nvgt` - a `#pragma plugin nvmidi` and
two prints - and on the runner it exits 0 in 1.3 seconds leaving
`MIN_BEGINMIN_END` in stdout: sixteen bytes, no trailing newline. That is a
clean exit flushing. `e2e_winmm.nvgt` on the same runner dies at 1.9 seconds
and leaves **0 bytes of stdout** beside **4783 bytes of stderr**, and stderr
was never buffered. So the 0 byte `logs_e2e.txt` is the crash's fingerprint,
not a defect of the harness, and no line added to that script could have been
read back while the crash stood. `can_flush = true` at the top of `main()` is
the attempt to change that; the compile pass will refuse the name if a script
cannot reach it, and that refusal is itself an answer.

**The plugin is not what dies.** The 4783 byte stderr file from run
36775764009 carries the whole registration trace on a real Windows runner:
`engine plugin api version 5, this plugin built against 5`, `calls=157
refused=0`, every registration crumb through `end of register_nvmidi`, `engine
type probe: 6 of 6 registered types are in the engine`, and
`6 of the plugin's own types answered to their name after registration`. The
plugin loads and registers; the death is after that, inside the e2e script's
own run. The note in windows.yml saying no script can reach `main()` with the
plugin loaded and that the crash precedes it is refuted by `e2e_min`, which
reaches `main()` and exits 0.

**Two of my own claims were wrong and are corrected in the tree.** The commit
`6ca957c` said the compile pass truncated `logs_e2e.txt`; run 36776804229, on
that very revision, still reports 0 bytes, which a pass writing elsewhere
cannot produce. And the older comment that the crash precedes any script was
asserted as measurement when it was inference. Both rewritten.

## What green still does not mean

Unchanged by any of this, and worth keeping beside it: no build machine has
ever carried a MIDI byte. The GitHub runner's kernel has no sound subsystem,
so the transport has no instrument there at all, and the octave echo asked for
in message 5506 is two unmeasured halves stacked on each other. A green e2e
would prove the type surface and the port enumeration. The hearing test is
where the transport gets measured, on the one machine that has a sequencer and
a person sitting at it.
