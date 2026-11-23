# TinySoundFont playback tests

Unit tests for the SF2 sample-rendering path around the *hold → release*
transition.

```
cd tests
make          # build and run
make clean
```

The tests are white-box (`TSF_IMPLEMENTATION` is defined in the test
translation unit) but only read library internals to assert what was loaded
from the SoundFont; playback is observed through the public
`tsf_note_on` / `tsf_note_off` / `tsf_render_float` API.

## Sample material

No binary fixtures are committed. `sf2_builder.h` writes a complete `.sf2`
image into memory. Each sample is 258 mono frames, 32 kHz, root key 60:

| frames        | role                                    |
|---------------|-----------------------------------------|
| 0 – 31        | pre-loop region                         |
| 32 – 95       | loop body (64-frame period)             |
| 96 – 257      | independent release tail                |

Two probe samples share one SoundFont:

* **constant probe** — every frame is `16384`; the rendered level of this voice
  is the amplitude envelope alone.
* **ramp probe** — frame `i` is `i * 120`; dividing its output by the constant
  probe's output cancels the shared envelope gain and recovers the source
  playback position (fractional positions included, through linear
  interpolation).

The probes are panned hard left / hard right, so one stereo render yields the
envelope and the sample position at the same instant. The output rate equals
the sample rate and is 32 kHz, which keeps the default 13 500-cent low-pass
below Nyquist and therefore inactive, so the PCM is the unfiltered sample
value.

Each sample is followed by two guard frames filled by linear extrapolation of
its last two frames. TinySoundFont advances the region end by one frame and
interpolates one frame ahead, so the guards keep every read inside the sample
buffer while leaving the PCM decodable right up to the voice's end.

## What is asserted

1. **Loop-mode-driven playback order on release**
   * `TSF_LOOPMODE_CONTINUOUS` (`sampleModes` 1): looping continues after
     `tsf_note_off`; the playback position never leaves the loop, the release
     tail is never heard, and every step is the forward pitch ratio modulo the
     loop span.
   * `TSF_LOOPMODE_SUSTAIN` (`sampleModes` 3): looping continues while the key
     is held; on release the voice stops wrapping and continues **forward**
     from the position it had reached, crosses into the independent tail,
     never returns into the loop, and ends at the sample end.
   * Both modes keep the library's normal forward direction; nothing is
     restarted.

2. **Release junction at different pitches**
   * The PCM at the first release frame matches the sample frame at the
     position that pitch had actually reached (checked twice: by decoding the
     position, and by an absolute PCM value).
   * The envelope is judged separately: it is still at its held level on the
     first release block, so PCM motion there comes purely from the sample
     position; after that first block it decays by the documented exponential
     release slope.
   * Keys 60, 67 and 72 release at distinct, ordered positions.

`TestFixtureDecodesLoopModes` first proves the generated SoundFont really
declares the intended loop mode, loop bounds and region span, so the
playback tests cannot pass on a fixture that silently failed to load.

## Rejection confidence

The suite was checked against mutated copies of `tsf.h`: making sustain mode
keep looping, making continuous mode stop looping, reversing loop direction,
restarting the sample at note off, pre-advancing the release envelope at note
off, and ignoring the played key each produce large failures.
