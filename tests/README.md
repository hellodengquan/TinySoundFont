# Layered-instrument unit tests

`test_layers.c` adds unit tests for the SoundFont 2 layer selection logic in
TinySoundFont (`tsf_load_presets` zone inheritance/filtering and
`tsf_note_on` region matching).  It has no external test dependency and builds
with any C99 compiler.

## Build and run

```sh
make test
# or manually:
cc -std=c99 -Wall -Wextra -I.. test_layers.c -lm -o test_layers
./test_layers
```

Environment variables:

| Variable | Effect |
| --- | --- |
| `TSF_TEST_OUT=/some/dir` | write the generated fixture SF2 files there instead of `./generated` |
| `TSF_TEST_NODUMP=1` | do not write the fixture SF2 files to disk |

## What is tested

The test builds two tiny SF2 soundfonts entirely in memory.  Every PCM sample
is a constant-level waveform (64 frames), so the rendered output of a note is
the sum of the levels of exactly the sample layers that sounded.  Levels are
distinct powers of two, which makes every layer combination identifiable both
through the active voices (`region->offset` → sample) and through the rendered
audio level.

### Fixture 1 — `layers_keyvel.sf2`: key splits, velocity layers, inheritance

One instrument with a global instrument zone (`InitialAttenuation` 0.5 dB and
an instant `AttackVolEnv`, so output is steady from the first frame) and five
sample zones covering two key splits and overlapping velocity ranges, exposed
through one preset whose own global zone sets `Pan` -0.1:

- structure: all five zones survive with their locally defined key/velocity
  ranges, and every resulting region inherits both the instrument-global
  attenuation and the preset-global pan;
- single layers in the low key split (soft → S0, hard → S1);
- velocity blending in the high key split: soft → S0, mid velocity → S0+S1,
  loud → S1+S2, including exact boundary velocities (63/64, 90/91) and key
  boundary 47/48;
- rendered audio level equals the expected subset sum of sample levels (with
  the inherited attenuation; at full velocity the velocity gain is 1.0),
  proving which layers the performer hears.  Notes used for level checks are
  rendered at the sample root key so the resampling ratio is exactly 1.0;
- velocity changes the gain but not the selected layer set.

### Fixture 2 — `layers_shared.sf2`: one instrument, three presets

One instrument with an instant-attack global zone and three key-split zones
(T0/T1/T2) referenced by:

- preset `LowHalf` (play range 0–47),
- preset `HighHalf` (play range 64–127),
- preset `Layered` with **two overlapping** preset zones (40–69 and 50–79)
  referencing the same instrument.  An instrument zone intersecting both
  preset zones is emitted twice, so notes in the overlap (50–69) stack two
  voices on the same sample source.

Checks:

- per-preset region counts after SF2 range filtering/intersection, including
  clamping of an instrument zone to the preset range and region duplication in
  the layered preset;
- silence outside a preset's own play range even though the shared instrument
  defines samples for those keys;
- single layers outside the overlap versus stacked voices on the same sample
  source inside it (two T1 voices, doubled output level);
- preset independence: sounding the same key through all three presets keeps
  every voice attributable to its own preset and sample source, and note-off
  in one preset only moves its own voice to the release segment, leaving the
  other presets' voices sounding;
- the bank/preset-number API (`tsf_bank_note_on/off`) follows the same rules.

## Generated files

`generated/layers_keyvel.sf2` and `generated/layers_shared.sf2` are the small
fixture soundfonts produced by the test run (a few hundred bytes each).  They
are regenerated on every run and are safe to delete; `make clean` removes them
together with the test binary.
