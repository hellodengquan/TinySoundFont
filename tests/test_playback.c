/* SF2 sample-playback unit tests for the hold -> release transition.
 *
 * Scope of this file: tests, plus the tiny sample material generated below.
 * No library behaviour is modified.
 *
 * What is locked down
 * -------------------
 * 1. Sample order after a held key goes into release is decided by the loop
 *    mode declared by the instrument ("sampleModes" generator):
 *      - TSF_LOOPMODE_CONTINUOUS (sampleModes 1):
 *          playback loops while held and keeps looping after release; the
 *          release tail beyond the loop is never heard.
 *      - TSF_LOOPMODE_SUSTAIN (sampleModes 3):
 *          playback loops while held; on release looping stops and playback
 *          continues forward from the position reached at that moment,
 *          through the independent release tail, until the sample end.
 *    In both cases the playback direction stays the library's normal forward
 *    direction; nothing is restarted and nothing plays backwards.
 *
 * 2. When the same instrument is played at different keys, the PCM at the
 *    release junction is the frame at the position that pitch had really
 *    reached. The amplitude-envelope change and the sample continuation are
 *    judged independently:
 *      - the envelope is read from a constant-value probe voice,
 *      - the playback position is recovered from a ramp probe voice divided by
 *        the constant probe (the common envelope gain cancels out).
 *
 * Test instrument (generated in memory, 258 mono frames per sample)
 * -----------------------------------------------------------------
 *     frames   0 ..  31  pre-loop region          value keeps increasing
 *     frames  32 ..  95  loop (64-frame period)   value keeps increasing
 *     frames  96 .. 257  independent release tail value keeps increasing
 *
 *   constant sample: every frame is 16384, so output == envelope gain.
 *   ramp sample:     frame i is i * 120, so (ramp output / constant output)
 *                    recovers the (possibly fractional) source frame index.
 *
 *   Both samples live in one SoundFont as two presets, panned hard left and
 *   hard right, with identical loop points, envelope and release time. A
 *   single stereo render therefore yields envelope (left) and position
 *   (right/left) at the same instant.
 *
 *   Output rate is 32 kHz at unity gain and unity pitch for key 60. That
 *   keeps the default 13500-cent low-pass inactive (its cutoff would be
 *   ~19.9 kHz, i.e. above Nyquist / 0.499), so the rendered PCM is the
 *   unfiltered sample value and positions decode exactly.
 *
 * Build and run (from this directory):
 *     make test
 */
#define TSF_IMPLEMENTATION
/* The library header trips -Wextra in places (null pointer arithmetic in its
 * generator table); those warnings are not the subject of these tests. */
#if defined(__clang__)
#  pragma clang diagnostic push
#  pragma clang diagnostic ignored "-Wnull-pointer-subtraction"
#elif defined(__GNUC__)
#  pragma GCC diagnostic push
#  pragma GCC diagnostic ignored "-Wnull-pointer-subtraction"
#endif
#include "../tsf.h"
#if defined(__clang__)
#  pragma clang diagnostic pop
#elif defined(__GNUC__)
#  pragma GCC diagnostic pop
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "sf2_builder.h"

/* ------------------------------------------------------------------------- */
/* Instrument design                                                         */
/* ------------------------------------------------------------------------- */

#define FRAME_COUNT   258
#define LOOP_START     32
#define LOOP_END       96 /* first frame after the loop; loop covers 32..95 */
#define RAMP_STEP     120
#define CONST_VALUE 16384
#define SAMPLE_RATE 32000 /* <= ~39.9 kHz so the default low-pass stays off */

/* ReleaseVolEnv generator amount in timecents: -6000 == 2^-5 s == 1/32 s.
 * Long enough that every observed voice stays in release for the whole
 * observation window, short enough to measure the envelope decay. */
#define RELEASE_TC (-6000)

#define HOLD_SAMPLES 300
#define MAX_RENDER   800

/* Tolerance for recovered sample positions (frames). */
#define POS_TOL 2e-3

#define SF2_SAMPLEMODE_CONTINUOUS 1
#define SF2_SAMPLEMODE_SUSTAIN    3

/* The library converts samples with /32767 and applies a unity gain. */
#define CONST_NORM ((double)CONST_VALUE / 32767.0)

static short g_ConstPcm[FRAME_COUNT];
static short g_RampPcm[FRAME_COUNT];

static int g_Checks;
static int g_Failures;

static void InitPcm(void)
{
	int i;
	for (i = 0; i < FRAME_COUNT; i++)
	{
		g_ConstPcm[i] = CONST_VALUE;
		g_RampPcm[i] = (short)(i * RAMP_STEP);
	}
}

#define CHECK(cond, ...) do { \
	g_Checks++; \
	if (!(cond)) { \
		g_Failures++; \
		fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
		fprintf(stderr, __VA_ARGS__); \
		fprintf(stderr, "\n"); \
	} \
} while (0)

#define CHECK_NEAR(actual, expected, tol, ...) do { \
	double a_ = (actual), e_ = (expected); \
	g_Checks++; \
	if (!(fabs(a_ - e_) <= (tol))) { \
		g_Failures++; \
		fprintf(stderr, "FAIL %s:%d: got %.9g expected %.9g (tol %g): ", \
			__FILE__, __LINE__, a_, e_, (double)(tol)); \
		fprintf(stderr, __VA_ARGS__); \
		fprintf(stderr, "\n"); \
	} \
} while (0)

static int IsFinite(double v) { return v == v && v < 1e30 && v > -1e30; }

/* Interpolation reads the frame at floor(pos) and the next one. The loop wrap
 * at LOOP_END - 1 makes the look-ahead jump back to the loop start, so that
 * one position is excluded from exact position comparisons. */
static int Interpolatable(double pos)
{
	if (!IsFinite(pos) || pos < 0.0) return 0;
	if (pos >= LOOP_END - 1 && pos < LOOP_END) return 0;
	return 1;
}

/* ------------------------------------------------------------------------- */
/* SoundFont and rendering helpers                                           */
/* ------------------------------------------------------------------------- */

/* Builds the two-probe font. Preset 0 = constant (hard left),
 * preset 1 = ramp (hard right), both using `sample_mode`. */
static tsf* MakeProbeFont(int sample_mode)
{
	tsf_test_preset_spec specs[2];
	unsigned char* bytes;
	int size;
	tsf* f;

	memset(specs, 0, sizeof(specs));

	specs[0].name = "constant-left";
	specs[0].loop_mode = sample_mode;
	specs[0].pcm = g_ConstPcm;
	specs[0].frame_count = FRAME_COUNT;
	specs[0].sample_rate = SAMPLE_RATE;
	specs[0].loop_start = LOOP_START;
	specs[0].loop_end = LOOP_END;
	specs[0].release_timecents = RELEASE_TC;
	specs[0].pan = -500; /* hard left */
	specs[0].original_pitch = 60;

	specs[1].name = "ramp-right";
	specs[1].loop_mode = sample_mode;
	specs[1].pcm = g_RampPcm;
	specs[1].frame_count = FRAME_COUNT;
	specs[1].sample_rate = SAMPLE_RATE;
	specs[1].loop_start = LOOP_START;
	specs[1].loop_end = LOOP_END;
	specs[1].release_timecents = RELEASE_TC;
	specs[1].pan = 500; /* hard right */
	specs[1].original_pitch = 60;

	bytes = tsf_test_build_sf2(specs, 2, &size);
	if (!bytes) return NULL;
	f = tsf_load_memory(bytes, size);
	free(bytes);
	if (!f) return NULL;

	tsf_set_output(f, TSF_STEREO_INTERLEAVED, SAMPLE_RATE, 0.0f);
	return f;
}

/* Renders `samples` stereo frames, splitting channels into outL / outR. */
static int RenderStereo(tsf* f, int samples, double* outL, double* outR)
{
	int i;
	float* buf = (float*)malloc((size_t)samples * 2 * sizeof(float));
	if (!buf) return 0;
	tsf_render_float(f, buf, samples, 0);
	for (i = 0; i < samples; i++)
	{
		outL[i] = buf[i * 2 + 0];
		outR[i] = buf[i * 2 + 1];
	}
	free(buf);
	return 1;
}

/* Recovers the source frame position from a probe pair:
 *   left  = CONST_VALUE/32767 * envelopeGain
 *   right = frameValue/32767   * envelopeGain
 * so right/left is the sample frame value divided by CONST_VALUE, which for
 * the ramp equals the playback position. The envelope cancels out exactly.
 * Returns NAN when the voice produced no output (killed voice). */
static double DecodePosition(double left, double right)
{
	if (!IsFinite(left) || left <= 1e-4) return NAN;
	return (right / left) * ((double)CONST_VALUE / (double)RAMP_STEP);
}

/* Stereo probe results for one key and one loop mode. */
typedef struct ProbeRun
{
	int key;
	int sample_mode;
	int hold;
	int total;
	/* index 0 .. total-1: hold phase first, then release phase */
	double pos[MAX_RENDER]; /* decoded source position, NAN if silent */
	double env[MAX_RENDER]; /* envelope gain observed on the constant probe */
	double rawL[MAX_RENDER];
	double rawR[MAX_RENDER];
	int voicesAfterRelease;
} ProbeRun;

/* Pitch ratio seen by the renderer: root key 60, equal sample and output
 * rates, so key 60 plays at 1.0 and each octave doubles it. */
static double PitchRatioFor(int key)
{
	return pow(2.0, (key - 60) / 12.0);
}

/* Plays preset 0 + preset 1 at `key`, renders `hold` frames, sends note off,
 * then renders `rel_samples` frames and decodes both probe channels. */
static void RunProbe(int sample_mode, int key, int hold, int rel_samples, ProbeRun* run)
{
	tsf* f;
	int i;

	memset(run, 0, sizeof(*run));
	run->key = key;
	run->sample_mode = sample_mode;
	run->hold = hold;
	run->total = hold + rel_samples;

	if (hold < 0 || rel_samples < 0 || hold + rel_samples > MAX_RENDER)
	{
		CHECK(0, "invalid probe window %d + %d", hold, rel_samples);
		return;
	}

	f = MakeProbeFont(sample_mode);
	if (!f)
	{
		CHECK(0, "could not build/load probe SoundFont");
		return;
	}

	tsf_note_on(f, 0, key, 1.0f); /* constant probe, hard left  */
	tsf_note_on(f, 1, key, 1.0f); /* ramp probe, hard right     */
	CHECK(tsf_active_voice_count(f) == 2, "key %d: expected 2 probe voices, got %d",
		key, tsf_active_voice_count(f));

	if (!RenderStereo(f, hold, run->rawL, run->rawR))
	{
		CHECK(0, "allocation failure rendering hold phase");
		tsf_close(f);
		return;
	}

	tsf_note_off(f, 0, key);
	tsf_note_off(f, 1, key);

	if (!RenderStereo(f, rel_samples, run->rawL + hold, run->rawR + hold))
	{
		CHECK(0, "allocation failure rendering release phase");
		tsf_close(f);
		return;
	}

	run->voicesAfterRelease = tsf_active_voice_count(f);
	tsf_close(f);

	for (i = 0; i < run->total; i++)
	{
		run->env[i] = run->rawL[i] / CONST_NORM;
		run->pos[i] = DecodePosition(run->rawL[i], run->rawR[i]);
	}
}

/* Reference model of the playback order the library must honour:
 *   - while held: position advances by `ratio`; it runs through the pre-loop
 *     region and then wraps inside the loop;
 *   - after release, sustain mode stops wrapping, continuous mode keeps
 *     wrapping. The direction is always forward.
 * The model follows the renderer's per-sample advance, independently of the
 * renderer's internal block structure.
 *
 * `alive` marks frames the renderer still renders (the region end is the last
 * real frame + 1, and the two extrapolated guard frames keep the look-ahead
 * read — and the decoded position — valid up to that limit).
 * `loop` marks frames where the playback position must stay inside the loop
 * bounds (after the pre-loop region, while looping). */
typedef struct ProbeModel
{
	int total;
	int alive[MAX_RENDER];
	int loop[MAX_RENDER];
	double pos[MAX_RENDER];
} ProbeModel;

static void BuildModel(int sample_mode, double ratio, int hold, int rel_samples, ProbeModel* m)
{
	const double loopEndDbl = (double)LOOP_END;          /* exclusive wrap point */
	const double loopSpan   = (double)(LOOP_END - LOOP_START);
	/* The library ends the region at the last real frame + 1 and renders
	 * while position < end, so this is the exclusive render limit. */
	const double renderEnd  = (double)(FRAME_COUNT + 1);
	double p = 0.0;
	int t, n = hold + rel_samples;
	int enteredLoop = 0;

	m->total = n;
	for (t = 0; t < n; t++)
	{
		int looping = (t < hold) || (sample_mode == SF2_SAMPLEMODE_CONTINUOUS);
		if (p >= LOOP_START) enteredLoop = 1;
		m->alive[t] = (p < renderEnd);
		m->loop[t] = (enteredLoop && looping);
		m->pos[t] = p;
		p += ratio;
		if (looping && p >= loopEndDbl) p -= loopSpan;
	}
}

/* ------------------------------------------------------------------------- */
/* Fixture sanity: the SoundFont really declares the requested loop modes    */
/* ------------------------------------------------------------------------- */

static void TestFixtureDecodesLoopModes(void)
{
	int i;
	for (i = 0; i < 2; i++)
	{
		int mode = (i == 0 ? SF2_SAMPLEMODE_CONTINUOUS : SF2_SAMPLEMODE_SUSTAIN);
		tsf* f = MakeProbeFont(mode);
		struct tsf_region* r0;
		struct tsf_region* r1;

		CHECK(f != NULL, "probe font (sampleModes %d) failed to load", mode);
		if (!f) continue;

		CHECK(tsf_get_presetcount(f) == 2, "expected 2 presets, got %d", tsf_get_presetcount(f));
		CHECK(tsf_get_presetname(f, 0) && strcmp(tsf_get_presetname(f, 0), "constant-left") == 0,
			"unexpected preset 0 name %s", tsf_get_presetname(f, 0) ? tsf_get_presetname(f, 0) : "(null)");
		CHECK(tsf_get_presetname(f, 1) && strcmp(tsf_get_presetname(f, 1), "ramp-right") == 0,
			"unexpected preset 1 name %s", tsf_get_presetname(f, 1) ? tsf_get_presetname(f, 1) : "(null)");

		r0 = &f->presets[0].regions[0];
		r1 = &f->presets[1].regions[0];

		CHECK(r0->loop_mode == (mode == SF2_SAMPLEMODE_CONTINUOUS ? TSF_LOOPMODE_CONTINUOUS : TSF_LOOPMODE_SUSTAIN),
			"preset 0 loop mode not decoded as declared");
		CHECK(r1->loop_mode == r0->loop_mode, "probe presets disagree on loop mode");
		CHECK(r0->loop_start == LOOP_START && r0->loop_end == LOOP_END - 1,
			"loop bounds %u..%u, expected %d..%d", r0->loop_start, r0->loop_end, LOOP_START, LOOP_END - 1);
		CHECK(r0->offset == 0 && r0->end == FRAME_COUNT + 1,
			"preset 0 region span %u..%u, expected 0..%d", r0->offset, r0->end, FRAME_COUNT + 1);
		CHECK(r1->offset == FRAME_COUNT + TSF_TEST_SF2_GUARD_FRAMES,
			"preset 1 region start %u, expected %d", r1->offset, FRAME_COUNT + TSF_TEST_SF2_GUARD_FRAMES);

		tsf_close(f);
	}
}

/* ------------------------------------------------------------------------- */
/* 1a. Continuous loop: order after release stays inside the loop            */
/* ------------------------------------------------------------------------- */

static void TestContinuousLoopOrder(void)
{
	int keys[2] = { 60, 72 };
	int k;
	for (k = 0; k < 2; k++)
	{
		ProbeRun run;
		ProbeModel model;
		double ratio = PitchRatioFor(keys[k]);
		int i, wraps = 0;

		RunProbe(SF2_SAMPLEMODE_CONTINUOUS, keys[k], HOLD_SAMPLES, 400, &run);
		BuildModel(SF2_SAMPLEMODE_CONTINUOUS, ratio, HOLD_SAMPLES, 400, &model);

		for (i = 0; i < run.total; i++)
		{
			CHECK(IsFinite(run.pos[i]), "key %d frame %d: no output from a continuously looping voice",
				keys[k], i);
			if (!IsFinite(run.pos[i])) continue;

			/* Once the loop is running, the release tail must never be heard. */
			if (model.loop[i])
				CHECK(run.pos[i] >= LOOP_START - POS_TOL && run.pos[i] <= LOOP_END - 1 + POS_TOL,
					"key %d frame %d: position %.4f left the loop [%d, %d] after release (%s phase)",
					keys[k], i, run.pos[i], LOOP_START, LOOP_END - 1, i < HOLD_SAMPLES ? "hold" : "release");
			else
				CHECK(run.pos[i] < LOOP_END + POS_TOL,
					"key %d frame %d: pre-loop position %.4f jumped past the loop", keys[k], i, run.pos[i]);

			if (Interpolatable(model.pos[i]) && Interpolatable(run.pos[i]))
				CHECK_NEAR(run.pos[i], model.pos[i], POS_TOL,
					"key %d frame %d: sample order mismatch in %s phase",
					keys[k], i, i < HOLD_SAMPLES ? "hold" : "release");
		}

		/* The loop must still wrap during release, proving looping continued. */
		for (i = HOLD_SAMPLES + 1; i < run.total; i++)
			if (Interpolatable(run.pos[i - 1]) && Interpolatable(run.pos[i]) && run.pos[i] + 1e-3 < run.pos[i - 1])
				wraps++;
		CHECK(wraps >= 1, "key %d: no loop wrap after release in continuous mode", keys[k]);

		/* Direction is forward: every step is +ratio modulo one loop span. */
		for (i = 1; i < run.total; i++)
		{
			double d;
			if (!Interpolatable(run.pos[i - 1]) || !Interpolatable(run.pos[i])) continue;
			d = run.pos[i] - run.pos[i - 1];
			if (d < 0) d += (LOOP_END - LOOP_START);
			CHECK_NEAR(d, ratio, POS_TOL, "key %d frame %d: step is not the forward pitch ratio %g",
				keys[k], i, ratio);
		}

		CHECK(run.voicesAfterRelease == 2,
			"key %d: continuously looping voices must still be playing, active=%d",
			keys[k], run.voicesAfterRelease);
	}
}

/* ------------------------------------------------------------------------- */
/* 1b. Sustain loop: on release playback continues forward through the tail  */
/* ------------------------------------------------------------------------- */

static void TestSustainLoopOrder(void)
{
	int keys[2] = { 60, 72 };
	int k;
	for (k = 0; k < 2; k++)
	{
		ProbeRun run;
		ProbeModel model;
		double ratio = PitchRatioFor(keys[k]);
		int i, died = 0, tailFrames = 0;

		RunProbe(SF2_SAMPLEMODE_SUSTAIN, keys[k], HOLD_SAMPLES, 400, &run);
		BuildModel(SF2_SAMPLEMODE_SUSTAIN, ratio, HOLD_SAMPLES, 400, &model);

		/* Held phase: still a loop, tail never reached. */
		for (i = 0; i < HOLD_SAMPLES; i++)
		{
			CHECK(IsFinite(run.pos[i]), "key %d hold frame %d: no output", keys[k], i);
			if (!IsFinite(run.pos[i])) continue;
			if (model.loop[i])
				CHECK(run.pos[i] <= LOOP_END - 1 + POS_TOL,
					"key %d hold frame %d: position %.4f exceeded loop end %d while held",
					keys[k], i, run.pos[i], LOOP_END - 1);
			if (Interpolatable(model.pos[i]) && Interpolatable(run.pos[i]))
				CHECK_NEAR(run.pos[i], model.pos[i], POS_TOL,
					"key %d hold frame %d: held order mismatch", keys[k], i);
		}

		/* Release phase: the voice continues forward from where it was, crosses
		 * from the loop into the independent tail, then ends at the sample end. */
		for (i = HOLD_SAMPLES; i < run.total; i++)
		{
			if (!model.alive[i])
			{
				died = 1;
				CHECK(!IsFinite(run.pos[i]) && run.rawL[i] == 0.0 && run.rawR[i] == 0.0,
					"key %d frame %d: output after the sample end", keys[k], i);
				continue;
			}
			CHECK(IsFinite(run.pos[i]), "key %d release frame %d: no output before the sample end", keys[k], i);
			if (!IsFinite(run.pos[i])) continue;

			if (Interpolatable(model.pos[i]) && Interpolatable(run.pos[i]))
			{
				CHECK_NEAR(run.pos[i], model.pos[i], POS_TOL,
					"key %d release frame %d: release order mismatch", keys[k], i);
				if (i > HOLD_SAMPLES && Interpolatable(run.pos[i - 1]))
					CHECK(run.pos[i] > run.pos[i - 1] - POS_TOL,
						"key %d release frame %d: playback went backwards (%.4f -> %.4f)",
						keys[k], i, run.pos[i - 1], run.pos[i]);
			}

			if (run.pos[i] >= LOOP_END && run.pos[i] < FRAME_COUNT - 1) tailFrames++;
		}

		CHECK(tailFrames > 0,
			"key %d: release never moved from the loop into the independent release tail", keys[k]);
		CHECK(died, "key %d: sustain voice never reached the end of the sample", keys[k]);
		CHECK(run.voicesAfterRelease == 0,
			"key %d: sustain voice should have ended after the tail, active=%d",
			keys[k], run.voicesAfterRelease);

		/* Once playback has entered the tail it must never return to the loop. */
		{
			int inTail = 0;
			for (i = HOLD_SAMPLES; i < run.total; i++)
			{
				if (!IsFinite(run.pos[i])) continue;
				if (run.pos[i] >= LOOP_END) inTail = 1;
				CHECK(!(inTail && run.pos[i] < LOOP_END - POS_TOL),
					"key %d release frame %d: dropped back from the tail into the loop", keys[k], i);
			}
		}
	}
}

/* ------------------------------------------------------------------------- */
/* 2. Release junction at different pitches: PCM follows the playback        */
/*    position; envelope and sample continuation are judged separately       */
/* ------------------------------------------------------------------------- */

static void TestReleaseJunctionAtDifferentPitches(void)
{
	int keys[3] = { 60, 67, 72 };
	double junctionPos[3];
	const int relSamples = 400;
	int k;

	for (k = 0; k < 3; k++)
	{
		ProbeRun run;
		ProbeModel model;
		double ratio = PitchRatioFor(keys[k]);
		double expectedEnvDecay;
		int nReleaseSamples, i;

		RunProbe(SF2_SAMPLEMODE_SUSTAIN, keys[k], HOLD_SAMPLES, relSamples, &run);
		BuildModel(SF2_SAMPLEMODE_SUSTAIN, ratio, HOLD_SAMPLES, relSamples, &model);

		/* --- Sample side: the PCM at the junction is the frame at the
		 *     position this pitch had reached when the key was released. --- */
		junctionPos[k] = model.pos[HOLD_SAMPLES];
		CHECK(IsFinite(run.pos[HOLD_SAMPLES]), "key %d: no release-junction output", keys[k]);
		if (IsFinite(run.pos[HOLD_SAMPLES]))
		{
			CHECK_NEAR(run.pos[HOLD_SAMPLES], junctionPos[k], POS_TOL,
				"key %d: release-junction PCM does not come from the position reached while held",
				keys[k]);

			/* Absolute PCM check that does not use the position decoder:
			 * the first release frame still has the held envelope level, so
			 * the ramp channel equals ramp(position)/32767. */
			CHECK_NEAR(run.rawR[HOLD_SAMPLES], (double)RAMP_STEP * junctionPos[k] / 32767.0, 1e-5,
				"key %d: junction PCM does not match the sample frame at the junction position",
				keys[k]);
			CHECK_NEAR(run.rawL[HOLD_SAMPLES], CONST_NORM, 1e-6,
				"key %d: junction PCM was shaped by the release envelope instead of the sample position",
				keys[k]);

			/* No restart and no jump: the junction continues the forward
			 * advance from the last held frame (modulo the loop span). */
			{
				double expectedStep = model.pos[HOLD_SAMPLES] - model.pos[HOLD_SAMPLES - 1];
				double actualStep = run.pos[HOLD_SAMPLES] - run.pos[HOLD_SAMPLES - 1];
				if (expectedStep < 0) expectedStep += (LOOP_END - LOOP_START);
				if (actualStep < 0) actualStep += (LOOP_END - LOOP_START);
				CHECK_NEAR(actualStep, expectedStep, POS_TOL,
					"key %d: playback jumped or restarted at the release junction", keys[k]);
				CHECK_NEAR(actualStep, ratio, POS_TOL,
					"key %d: release junction step is not the forward pitch ratio", keys[k]);
			}
		}

		/* --- Envelope side, judged separately from the sample values. --- */
		CHECK_NEAR(run.env[HOLD_SAMPLES - 1], 1.0, 1e-6,
			"key %d: held envelope is not at sustain level", keys[k]);
		CHECK_NEAR(run.env[HOLD_SAMPLES], run.env[HOLD_SAMPLES - 1], 1e-6,
			"key %d: release shaped the very first release frame, mixing envelope into the junction PCM",
			keys[k]);

		/* Inside the first effects block the sample position keeps advancing
		 * while the envelope stays flat: the two effects are separable. */
		for (i = HOLD_SAMPLES; i < HOLD_SAMPLES + TSF_RENDER_EFFECTSAMPLEBLOCK; i++)
		{
			CHECK_NEAR(run.env[i], run.env[HOLD_SAMPLES - 1], 1e-5,
				"key %d: envelope changed inside the first release block", keys[k]);
			if (Interpolatable(model.pos[i]) && Interpolatable(run.pos[i]))
				CHECK_NEAR(run.pos[i], model.pos[i], POS_TOL,
					"key %d: sample position did not keep advancing inside the first release block", keys[k]);
		}

		/* The PCM changes right at the junction although the envelope is flat:
		 * that difference is produced purely by the advancing sample position. */
		CHECK(run.rawR[HOLD_SAMPLES] != run.rawR[HOLD_SAMPLES - 1],
			"key %d: PCM did not advance across the release junction", keys[k]);

		/* After the first block the envelope follows the documented
		 * exponential release slope, independent of the sample values. */
		nReleaseSamples = (int)(pow(2.0, RELEASE_TC / 1200.0) * SAMPLE_RATE);
		expectedEnvDecay = pow(exp(-9.226 / (double)nReleaseSamples), (double)TSF_RENDER_EFFECTSAMPLEBLOCK);
		CHECK(run.env[HOLD_SAMPLES + TSF_RENDER_EFFECTSAMPLEBLOCK] < run.env[HOLD_SAMPLES] - 1e-4,
			"key %d: envelope did not decay after the first release block", keys[k]);
		CHECK_NEAR(run.env[HOLD_SAMPLES + TSF_RENDER_EFFECTSAMPLEBLOCK] / run.env[HOLD_SAMPLES],
			expectedEnvDecay, 2e-3, "key %d: release envelope slope is not the documented exponential",
			keys[k]);
	}

	/* Different pitches must have reached different positions at release. */
	CHECK(fabs(junctionPos[0] - junctionPos[2]) > 1.0,
		"keys 60 and 72 released at the same sample position (%.2f, %.2f)",
		junctionPos[0], junctionPos[2]);
	CHECK(fabs(junctionPos[0] - junctionPos[1]) > 1.0 && fabs(junctionPos[1] - junctionPos[2]) > 1.0,
		"key 67 junction position (%.2f) is not distinct from keys 60 (%.2f) / 72 (%.2f)",
		junctionPos[1], junctionPos[0], junctionPos[2]);
}

/* ------------------------------------------------------------------------- */

int main(void)
{
	InitPcm();

	printf("SF2 playback tests: hold -> release transition\n");

	TestFixtureDecodesLoopModes();
	TestContinuousLoopOrder();
	TestSustainLoopOrder();
	TestReleaseJunctionAtDifferentPitches();

	if (g_Failures)
	{
		printf("FAILED: %d of %d checks\n", g_Failures, g_Checks);
		return 1;
	}
	printf("OK: %d checks passed\n", g_Checks);
	return 0;
}
