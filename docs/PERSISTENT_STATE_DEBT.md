# Remaining restartable-state debt

This is a source audit at `9c99cd2`, not a live continuation result. The CMOS
component tests do not establish complete persistent snapshots. No fixes or
passing experiments for the gaps below are claimed here.

## CPU NMI state

`src/cpu/cpu.cpp:75-77` defines `CPU_NMI_gate`, `CPU_NMI_active` and
`CPU_NMI_pending`. `SerializeCPU` at line 4934 saves CPU registers, cycles, flags,
paging and other state but omits all three globals.

The values affect continuation:

- `CPU_Raise_NMI` at line 603 sets pending and checks delivery.
- `CPU_Check_NMI` at line 610 requires `!active && gate && pending`; it changes
  `CPU_CycleLeft`, `CPU_Cycles` and `PIC_IRQCheck` to stop the core for delivery.
- `CPU_NMI_Interrupt` at line 594-599 sets active and clears pending; `CPU_IRET` at line 1514
  clears active.
- CMOS index-port writes (`src/hardware/cmos.cpp:254`) update the gate. The
  PC/XT NMI handler (`src/hardware/pic.cpp:378-382`, installed on port A0h at line 1073)
  also updates it. That PC/XT port is not the AT slave-PIC command register.

Saving only the CMOS NMI bit cannot represent every writer of this CPU state.
A proposed native test captures the three flag combinations (masked pending,
active pending, deliverable pending), perturbs/restores, then compares actual
`CPU_Check_NMI` cycle/PIC effects with uninterrupted continuation. The fixture
must preserve the old globals directly for teardown when testing the old
serializer. A subsequent guest interrupt vector 2/IRET probe is needed for actual
delivery; the scheduling test alone cannot prove it. None has run in this slice.

## Open host files and pending timestamps

`DOS_File::SaveState` (`src/dos/dos_files.cpp:2370-2401`) writes name, flags,
open, attributes, DOS date/time, refcount, drive and seek position. It omits
`DOS_File::newtime` (`include/dos_system.h:106`). `LocalFile` has no serializer
override; its `last_action` and `read_only_medium` are likewise not written by
this path (`include/dos_system.h:180-196`). The latter may be reconstructed from
drive configuration; omission alone is not a demonstrated divergence.

The pending timestamp has observable consumers:

- `DOS_SetFileDate` (`dos_files.cpp:2314-2333`) stores the requested DOS date/time
  and sets `newtime=true`.
- `LocalFile::Close` (`src/dos/drive_local.cpp:3054`) tests `newtime` and
  `last_action`; its timestamp branch flushes before applying the DOS timestamp
  to the host file, explicitly avoiding a later buffered close replacing it
  with current host time.
- `LocalFile::Flush` at line 3197 also branches on these fields. The constructor at line 3133
  resets `last_action=NONE`; the base `newtime` initializer is false.
- `DOS_File::LoadState` at line 2404 restores metadata and calls Seek; it never restores
  `newtime`. `LocalFile::Seek` at line 3050 resets `last_action=NONE`.

`UpdateLocalDateTime` at line 3162 obtains guest date/time through INT21h AH2Ah/2Ch
before host `mktime`; it does not simply choose the current wall-clock time.
Normal buffered writes use `fwrite` at line 2890. `GetSeekPos` at line 3127 only calls
`ftell`/`lseek`, and the inspected save path contains no explicit flush.

A proposed deterministic test opens a scratch file, sets a known DOS date with
AH57h, snapshots before close, then compares uninterrupted versus restored close
with identical starting file bytes and host timestamp. Capture DOS metadata and
host timestamps as well as contents. Use an independent restored process or
carefully isolate original handles: `POD_Load_DOS_Files` at line 2839-2851 deliberately
detaches existing file objects without closing/deleting them before reopening
saved paths at line 2880. An in-process success could rely on those surviving host
resources and cannot prove restartability. This experiment has not run yet.

File checksums alone do not capture pending I/O, metadata, open-file policy or
missing/unlinked files. Forcing `LocalFile::Flush` is not by itself a parity fix:
it changes flags and can run guest date/time callbacks. A future storage envelope
must cover or explicitly reject unsupported dependencies before live mutation.

## Keyboard held-key reporting versus scancode modifiers

Additional source audit at `256c2cb`; no live keyboard continuation experiment
has been run. `src/hardware/keyboard.cpp:149-154` holds six independent left/right
Ctrl, Alt and Shift booleans. `SerializeKeyboard` at lines 2977-2997 saves
`keyb.key_pressed` but none of those six booleans. Its inherited POD restore does
not replay the key handlers. `KEYBOARD_IsKeyPressed` at line 1940 reads only the
saved array; agent `GetInputState` (`src/agent/debugger/debugger_adapter.cpp:632-647`)
uses that accessor. A correct-looking held-key response alone therefore cannot
prove the scancode generator's modifier state was restored.

The omitted flags have concrete consumers in scan-set 1: `KBD_pause` at lines
1789-1803 emits `E1 1D 45 E1 9D C5` without Ctrl, versus `E0 46 E0 C6` with
exactly one Ctrl held. PrintScreen at lines 1812-1825 separately branches on
Alt/Ctrl/Shift. `KEYBOARD_AddKey` at lines 1911-1937 first updates the array and
then dispatches to scan-set 1 when controller translation is enabled (or scan
set 1 is selected); the modifier handlers update the separate flags.

Proposed native continuation probe (unrun): configure translated scan-set 1,
press left Ctrl through `KEYBOARD_AddKey`, drain its initial scancode, and capture
all state. Record an uninterrupted Pause press's bytes. Release Ctrl, restore,
assert the reported held-key array, and repeat the same Pause press; compare the
actual queued/scanned bytes. The source predicts an E1/Pause sequence after the
old restore versus E0/Ctrl-Break uninterrupted, because the release cleared the
unsaved flag. Restore the full fixture and omitted flags between cases. A fix
needs a negative control against the prior serializer and equivalent coverage
for the other modifier branches; no success is claimed here.

Rechecked against pinned debugger integration `f763b7b61` on 2026-09-26:
`SerializeKeyboard` still registers `keyb.key_pressed` but none of the six
modifier booleans, while `KBD_pause` still reads the separate Ctrl flags.
This remains a source-backed gap; the proposed continuation probe has not run.
