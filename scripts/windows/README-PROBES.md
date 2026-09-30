# The probe sweep, and how its results are read

## What the sweep is

`windows.yml` compiles each probe under `scripts/windows/*.nvgt` into `nvgt.exe`
one at a time and runs it. Each case therefore answers with **its own** engine
diagnostics; that is the whole point of the design, and it is why the per-case
`script blob <sha>` line is printed next to each case's output.

## The two ways a reading has already gone wrong

### 1. Truncated stderr mistaken for an identical script

Measured on run 36759356922 (`b151016`): `detach_probe`'s captured stderr was
byte-for-byte identical to `e2e_winmm.nvgt`'s, which looks damning until the
size is read -- **both are exactly 4372 bytes**. The engine writes more stderr
than `Start-Process -RedirectStandardError` captures, and the cut lands on the
same `reg line` sequence in both, because registration is deterministic and the
truncation point falls inside it. The tail matched because the *ceiling*
matched, not because the script did.

This was read, wrongly, as "the engine ignores argv and every case ran the e2e
script". It does not ignore argv: see below.

### 2. Reading a case's file from a neighbouring case's block

Several cases cite **several** files (`n_type`'s block cites both
`n_type.nvgt` and `n_type1.nvgt`). That is not the engine running two scripts.
It is the *step* repeating: the sweep prints the last case's stdout and stderr
into the same job-log region, and a naive per-case regex walks past the
`--- <case> ---` boundary and picks up the next case's diagnostics. Any
per-case read must be cut at the next `--- <case> ---` header.

## How a case is read correctly

Cut the block at its own `--- <case> ---` header. Then:

- **No `ENGINE:` error and exit 0** -- it compiled and ran. Read the stdout.
- **`ENGINE: ERROR:` lines** -- the compiler refused it. The named file must be
  the case's own file and the line must exist within it. If it does, the case
  is sound and the refusal is the measurement.
- **exit `0xC0000005`, stderr only, stdout 0 bytes** -- it compiled, started,
  and died. `nvgt` buffers `print` and has no `fflush`, so a crash before the
  first flush loses every line the script printed.

### The verified reading of run 36759356922

Every compile-failing case cites its own file and a line inside it:

| case | file cited | lines | meaning |
|---|---|---|---|
| `n_type` | n_type.nvgt | 21, 23 | its two `@` declarations refused |
| `n_type1` | n_type1.nvgt | 10,12,14,16 bare / 18,20 `@` | all six names refused, **no pragma in file** |
| `n_ctor` | n_ctor.nvgt | 8, 10 | its two bare declarations refused |
| `n_sized`, `n_panic`, `n_array`, `c_handle`, `a_bare`, `b_decl`, `n_whole`, `n_noteonly`, `n_inputonly`, `e2e_types`, `j_pragma` | own file each | in range | each its own refusal |

Twenty distinct blobs in, twenty own-file refusals out, no foreign-file citation
inside a case's own block. **The engine honours the argv script path.**

## The rules, as measured

1. A **bare** declaration of a plugin type is refused:
   `Identifier 'midi_note' is not a data type`
   (`n_ctor` line 8; `n_type1` line 14, with no plugin connected at all).
2. A declaration carrying **`@`** is taken, in a script whose plugin is loaded
   and whose six types registered (`n_type` lines 17, 19 compile; refusals start
   at 21 and 23).
3. `@` is still refused by **certain contexts** -- `Expected ';'` /
   `Instead found '@'` (`n_type` 21/23, `n_type1` 18/20, `j_pragma`).
4. With **no `#pragma plugin`** in the file, all six names are refused
   (`n_type1`) -- i.e. the engine has none of them built in.

## `detach_probe` and the crash

`n_handle2` exits 0 and prints
`HANDLE2_BEGINBLOCK_NOTEBLOCK_NOTE_OKBLOCK_ARRAYBLOCK_ARRAY_OKHANDLE2_END`
-- so `midi_note@ n2` and `array<midi_note@>@ notes` declare and survive.

`detach_probe` is the same shape plus a single call:
`int n = midi_output_port_count();`. It dies `0xC0000005` at 1.6s with
**stdout 0 bytes** (buffered, never flushed) and stderr showing registration
completed cleanly -- `6 of the plugin's own types answered to their name`.

Stack: `winmmbase!InternalLoadDriver` -> `KERNELBASE!FreeLibrary` ->
`ntdll!LdrUnloadDll` -> `LdrpDereferenceModule` -> `LdrpUnmapModule`.

So the fault is **not** in the types, the registration, or the declaration. The
one difference from `n_handle2` is the call. The plugin carries no `DllMain`,
`DLL_PROCESS_DETACH` or `atexit` (grep of `src/nvmidi.cpp` finds none), so the
candidates are static destructors, or the winmm driver registration left behind
by the `with_port<>` path the call opens. **Neither has been measured yet.**
