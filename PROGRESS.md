# nvmidi — state of the investigation

Last updated: 2026-09-30, at commit 99a109b.

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
