/*
 * Unit tests for layered SoundFont2 instruments in TinySoundFont.
 *
 * Scope (per task #162):
 *   1. Presets may reuse one set of samples in different parts of the keyboard,
 *      and instruments blend timbres by key velocity.  These tests verify which
 *      sample layers the performer actually hears, following the SF2 rules for
 *      zone inheritance (instrument global zone, preset global zone) and local
 *      zone definitions (key range / velocity range generators and preset
 *      region filtering).
 *   2. When one instrument is referenced by several presets, each preset gets
 *      its own play range.  Layering and silence must be judged per preset and
 *      the sample source of every voice must be distinguishable.
 *
 * The test harness builds tiny SF2 files entirely in memory (no external
 * soundfont dependency).  Every PCM sample is a constant-level waveform, so the
 * rendered output level of a note is the sum of the levels of exactly the
 * layers that sounded, which lets the tests assert the audible layer set.
 *
 * The generated SF2 binaries are also written to a small "generated" folder
 * next to this test (override with TSF_TEST_OUT, suppress with TSF_TEST_NODUMP)
 * so the fixture soundfonts can be inspected with external tools.
 *
 * Build:  make           (or: cc -std=c99 -Wall -Wextra -I.. test_layers.c -lm -o test_layers)
 * Run:    ./test_layers
 */

#define TSF_IMPLEMENTATION
#include "../tsf.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <sys/stat.h>
#include <sys/types.h>

/* ---------------------------------------------------------------------------------------------------------
 * Minimal test framework
 * --------------------------------------------------------------------------------------------------------- */

static int tests_run = 0;
static int tests_failed = 0;
static int checks_failed = 0;

#define CHECK(cond) do { \
	if (!(cond)) { \
		printf("    FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
		checks_failed++; \
	} \
} while (0)

#define CHECK_INT(actual, expected) do { \
	long long _ai = (long long)(actual), _ei = (long long)(expected); \
	if (_ai != _ei) { \
		printf("    FAIL %s:%d: %s == %lld, expected %lld\n", __FILE__, __LINE__, #actual, _ai, _ei); \
		checks_failed++; \
	} \
} while (0)

#define CHECK_FLOAT_NEAR(actual, expected, tol) do { \
	double _af = (double)(actual), _ef = (double)(expected), _tf = (double)(tol); \
	if (fabs(_af - _ef) > _tf) { \
		printf("    FAIL %s:%d: %s == %.6f, expected %.6f (+- %.6f)\n", __FILE__, __LINE__, #actual, _af, _ef, _tf); \
		checks_failed++; \
	} \
} while (0)

#define RUN_TEST(testfn) do { \
	int _before = checks_failed; \
	tests_run++; \
	printf("[ RUN      ] %s\n", #testfn); \
	testfn(); \
	if (checks_failed == _before) printf("[       OK ] %s\n", #testfn); \
	else { tests_failed++; printf("[  FAILED  ] %s\n", #testfn); } \
} while (0)

/* ---------------------------------------------------------------------------------------------------------
 * In-memory RIFF/SF2 builder
 *
 * An SF2 file is:
 *   RIFF 'sfbk'
 *     LIST 'pdta'  phdr pbag pmod pgen inst ibag imod igen shdr (hydra chunks)
 *     LIST 'sdta'  smpl (16-bit little endian PCM sample pool)
 *
 * All multi-byte values in SF2 are little endian, which matches the test
 * targets (all mainstream desktop/mobile CPUs).  The structs in tsf.h read the
 * hydra records with direct fread, so the record layouts below use packed
 * little-endian fields written explicitly.
 * --------------------------------------------------------------------------------------------------------- */

typedef struct
{
	unsigned char* data;
	unsigned int size;
	unsigned int capacity;
} sf2buf;

static void sf2buf_init(sf2buf* b)
{
	b->capacity = 4096;
	b->size = 0;
	b->data = (unsigned char*)malloc(b->capacity);
}

static void sf2buf_free(sf2buf* b)
{
	free(b->data);
	b->data = NULL;
	b->size = b->capacity = 0;
}

static void sf2buf_reserve(sf2buf* b, unsigned int extra)
{
	if (b->size + extra <= b->capacity) return;
	while (b->size + extra > b->capacity) b->capacity *= 2;
	b->data = (unsigned char*)realloc(b->data, b->capacity);
}

static void sf2buf_put(sf2buf* b, const void* data, unsigned int size)
{
	sf2buf_reserve(b, size);
	memcpy(b->data + b->size, data, size);
	b->size += size;
}

static void sf2buf_put4cc(sf2buf* b, const char id[4])
{
	sf2buf_put(b, id, 4);
}

static void sf2buf_u8(sf2buf* b, unsigned char v)
{
	sf2buf_put(b, &v, 1);
}

static void sf2buf_u16(sf2buf* b, unsigned short v)
{
	unsigned char p[2] = { (unsigned char)(v & 0xFF), (unsigned char)(v >> 8) };
	sf2buf_put(b, p, 2);
}

static void sf2buf_s16(sf2buf* b, short v)
{
	sf2buf_u16(b, (unsigned short)v);
}

static void sf2buf_u32(sf2buf* b, unsigned int v)
{
	unsigned char p[4] = { (unsigned char)(v & 0xFF), (unsigned char)((v >> 8) & 0xFF),
	                       (unsigned char)((v >> 16) & 0xFF), (unsigned char)((v >> 24) & 0xFF) };
	sf2buf_put(b, p, 4);
}

static void sf2buf_zeros(sf2buf* b, unsigned int count)
{
	sf2buf_reserve(b, count);
	memset(b->data + b->size, 0, count);
	b->size += count;
}

static void sf2buf_name(sf2buf* b, const char* name)
{
	char tmp[20];
	unsigned int len = (unsigned int)strlen(name);
	if (len > 19) len = 19;
	memset(tmp, 0, sizeof(tmp));
	memcpy(tmp, name, len);
	sf2buf_put(b, tmp, 20);
}

/* Begin a RIFF/LIST chunk; the size field is patched by sf2buf_endchunk. */
typedef struct { unsigned int size_pos; unsigned int content_start; } sf2chunk_mark;

static sf2chunk_mark sf2buf_beginchunk(sf2buf* b, const char id[4])
{
	sf2chunk_mark m;
	sf2buf_put4cc(b, id);
	m.size_pos = b->size;
	sf2buf_u32(b, 0);
	m.content_start = b->size;
	return m;
}

/* Patch a 32-bit size field that was reserved earlier. */
static void sf2buf_u32_at(sf2buf* b, unsigned int pos, unsigned int v)
{
	b->data[pos]     = (unsigned char)(v & 0xFF);
	b->data[pos + 1] = (unsigned char)((v >> 8) & 0xFF);
	b->data[pos + 2] = (unsigned char)((v >> 16) & 0xFF);
	b->data[pos + 3] = (unsigned char)((v >> 24) & 0xFF);
}

static void sf2buf_endchunk(sf2buf* b, sf2chunk_mark m)
{
	sf2buf_u32_at(b, m.size_pos, b->size - m.content_start);
}

/* SF2 generator operands used by the fixtures (see SoundFont 2 spec / tsf.h genMetas). */
enum { GEN_Instrument = 41, GEN_KeyRange = 43, GEN_VelRange = 44,
       GEN_InitialAttenuation = 48, GEN_SampleID = 53, GEN_SampleModes = 54,
       GEN_Pan = 17, GEN_OverridingRootKey = 58 };

static void gen_word(sf2buf* b, unsigned short oper, unsigned short amount)
{
	sf2buf_u16(b, oper);
	sf2buf_u16(b, amount);
}

static void gen_short(sf2buf* b, unsigned short oper, short amount)
{
	sf2buf_u16(b, oper);
	sf2buf_s16(b, amount);
}

static void gen_range(sf2buf* b, unsigned short oper, unsigned char lo, unsigned char hi)
{
	sf2buf_u16(b, oper);
	sf2buf_u8(b, lo);
	sf2buf_u8(b, hi);
}

/* phdr record: name(20) preset(u16) bank(u16) presetBagNdx(u16) library/genre/morphology(u32 x3) */
static void phdr_record(sf2buf* b, const char* name, unsigned short preset, unsigned short bank,
                        unsigned short bag_ndx)
{
	sf2buf_name(b, name);
	sf2buf_u16(b, preset);
	sf2buf_u16(b, bank);
	sf2buf_u16(b, bag_ndx);
	sf2buf_zeros(b, 12);
}

/* inst record: name(20) instBagNdx(u16) */
static void inst_record(sf2buf* b, const char* name, unsigned short bag_ndx)
{
	sf2buf_name(b, name);
	sf2buf_u16(b, bag_ndx);
}

/* shdr record: name(20) start/end/startLoop/endLoop(u32 x4) sampleRate(u32)
 *              originalPitch(u8) pitchCorrection(s8) sampleLink(u16) sampleType(u16) */
static void shdr_record(sf2buf* b, const char* name, unsigned int start, unsigned int end,
                        unsigned int rate, unsigned char root, unsigned short sample_type)
{
	sf2buf_name(b, name);
	sf2buf_u32(b, start);
	sf2buf_u32(b, end);
	sf2buf_u32(b, 0); /* start loop */
	sf2buf_u32(b, 0); /* end loop   */
	sf2buf_u32(b, rate);
	sf2buf_u8(b, root);
	sf2buf_u8(b, 0);  /* pitch correction (s8) */
	sf2buf_u16(b, 0); /* sample link */
	sf2buf_u16(b, sample_type);
}

#define SAMPLE_RATE 44100
#define SAMPLE_FRAMES 64   /* each fixture sample is 64 frames of a constant level */
#define SAMPLE_TYPE_MONO 1

/* A constant-level PCM sample: rendered at its root key its output equals the
 * level below (times the velocity/envelope gain, which is 1.0 at full velocity
 * with the default envelope).  Levels are positive powers of two, so any layer
 * combination produces a unique sum. */
typedef struct { const char* name; short level; } fixture_sample;

static void write_pcm_sample(sf2buf* b, short level)
{
	int i;
	for (i = 0; i < SAMPLE_FRAMES; i++) sf2buf_s16(b, level);
}

/* Wrap the built hydra + sample pool in the RIFF sfbk envelope. */
static void sf2_wrap(sf2buf* out, const sf2buf* pdta, const sf2buf* sdta_smpl)
{
	sf2chunk_mark riff, list;
	sf2chunk_mark smpl;

	sf2buf_init(out);
	riff = sf2buf_beginchunk(out, "RIFF");
	sf2buf_put4cc(out, "sfbk");

	list = sf2buf_beginchunk(out, "LIST");
	sf2buf_put4cc(out, "pdta");
	sf2buf_put(out, pdta->data, pdta->size);
	sf2buf_endchunk(out, list);

	list = sf2buf_beginchunk(out, "LIST");
	sf2buf_put4cc(out, "sdta");
	smpl = sf2buf_beginchunk(out, "smpl");
	sf2buf_put(out, sdta_smpl->data, sdta_smpl->size);
	sf2buf_endchunk(out, smpl);
	sf2buf_endchunk(out, list);

	sf2buf_endchunk(out, riff);
}

/* ---------------------------------------------------------------------------------------------------------
 * Fixture 1: layered key splits + velocity layers + zone inheritance
 *
 * Samples (constant levels, root key 60, no loops):
 *   S0 level 1024, S1 level 2048, S2 level 4096
 *
 * Instrument "LayeredInst":
 *   global instrument zone : InitialAttenuation 0.5 dB (inherited by every
 *                            zone) and AttackVolEnv -12000 (instant attack,
 *                            constant output level from the first frame)
 *   zone 0 : key 0-47,                 vel 1-63,  SampleID S0
 *   zone 1 : key 0-47,                 vel 64-127, SampleID S1
 *   zone 2 : key 48-127,               vel 1-90,  SampleID S0
 *   zone 3 : key 48-127,               vel 64-127, SampleID S1  (overlaps zone 2 on vel 64-90)
 *   zone 4 : key 48-127,               vel 91-127, SampleID S2
 *
 * One preset "LayeredPreset" (bank 0, preset 0) with a preset global zone
 * carrying Pan -0.1, then one normal preset zone referencing the instrument
 * without any key/vel range restriction.  Every resulting region must inherit
 * the attenuation (instrument global zone) and the pan (preset global zone),
 * while keeping its own locally defined key and velocity ranges.
 * --------------------------------------------------------------------------------------------------------- */

static sf2buf* build_font_keyvel(void)
{
	static const fixture_sample samples[] = {
		{ "SampLow",  1024 },
		{ "SampMid",  2048 },
		{ "SampHigh", 4096 },
	};
	enum { NUM_SAMPLES = 3 };
	const unsigned int sample_words = NUM_SAMPLES * SAMPLE_FRAMES;

	sf2buf pdta, smpl, *out;
	sf2chunk_mark ch;
	unsigned int i, word_off;

	sf2buf_init(&pdta);
	sf2buf_init(&smpl);
	out = (sf2buf*)malloc(sizeof(sf2buf));

	/* sdta/smpl: PCM pool, 3 samples back to back, followed by the 46 zero
	 * words required after the last sample by the SF2 spec. */
	for (i = 0; i < NUM_SAMPLES; i++) write_pcm_sample(&smpl, samples[i].level);
	sf2buf_zeros(&smpl, 46 * sizeof(short));

	/* ---- phdr: one real preset + terminal record ---- */
	ch = sf2buf_beginchunk(&pdta, "phdr");
	phdr_record(&pdta, "LayeredPreset", 0, 0, 0); /* presetBagNdx 0 */
	phdr_record(&pdta, "EOP", 255, 255, 2);       /* real bag count; terminal bag follows in pbag */
	sf2buf_endchunk(&pdta, ch);

	/* ---- pbag: global preset zone, instrument zone, terminal record ----
	 * tsf reads ppbag[1].genNdx as the end of each bag's generator range.
	 * The terminal bag's genNdx is the index just past the last real
	 * generator (the terminal pgen record lives at that same index). */
	ch = sf2buf_beginchunk(&pdta, "pbag");
	sf2buf_u16(&pdta, 0); sf2buf_u16(&pdta, 0); /* bag 0: genNdx 0, modNdx 0 */
	sf2buf_u16(&pdta, 1); sf2buf_u16(&pdta, 0); /* bag 1: genNdx 1, modNdx 0 */
	sf2buf_u16(&pdta, 4); sf2buf_u16(&pdta, 0); /* terminal: gens of bag 1 end at 4 */
	sf2buf_endchunk(&pdta, ch);

	/* ---- pmod: single zeroed terminal record ---- */
	ch = sf2buf_beginchunk(&pdta, "pmod");
	sf2buf_zeros(&pdta, 10);
	sf2buf_endchunk(&pdta, ch);

	/* ---- pgen ---- */
	ch = sf2buf_beginchunk(&pdta, "pgen");
	/* bag 0, preset global zone: pan only, no instrument generator */
	gen_short(&pdta, GEN_Pan, -100); /* -0.1 pan (amount is in milli-pan) */
	/* bag 1: instrument 0, full key/vel range so nothing is filtered out */
	gen_range(&pdta, GEN_KeyRange, 0, 127);
	gen_range(&pdta, GEN_VelRange, 0, 127);
	gen_word(&pdta, GEN_Instrument, 0);
	/* terminal record */
	gen_word(&pdta, 0, 0);
	sf2buf_endchunk(&pdta, ch);

	/* ---- inst: one instrument + terminal ---- */
	ch = sf2buf_beginchunk(&pdta, "inst");
	inst_record(&pdta, "LayeredInst", 0);
	inst_record(&pdta, "EOI", 6); /* total ibag count: global + 5 zones */
	sf2buf_endchunk(&pdta, ch);

	/* ---- ibag: global zone, 5 sample zones, terminal ---- */
	ch = sf2buf_beginchunk(&pdta, "ibag");
	/* igen layout: 0..1 (global), 2..4 (zone0), 5..7 (zone1), 8..10 (zone2),
	 * 11..13 (zone3), 14..16 (zone4), terminal genNdx 17 */
	sf2buf_u16(&pdta, 0);  sf2buf_u16(&pdta, 0);
	sf2buf_u16(&pdta, 2);  sf2buf_u16(&pdta, 0);
	sf2buf_u16(&pdta, 5);  sf2buf_u16(&pdta, 0);
	sf2buf_u16(&pdta, 8);  sf2buf_u16(&pdta, 0);
	sf2buf_u16(&pdta, 11); sf2buf_u16(&pdta, 0);
	sf2buf_u16(&pdta, 14); sf2buf_u16(&pdta, 0);
	sf2buf_u16(&pdta, 17); sf2buf_u16(&pdta, 0); /* terminal: total igen count */
	sf2buf_endchunk(&pdta, ch);

	/* ---- imod: single zeroed terminal record ---- */
	ch = sf2buf_beginchunk(&pdta, "imod");
	sf2buf_zeros(&pdta, 10);
	sf2buf_endchunk(&pdta, ch);

	/* ---- igen ---- */
	ch = sf2buf_beginchunk(&pdta, "igen");
	/* global instrument zone: 0.5 dB attenuation and an instant attack
	 * (AttackVolEnv = -12000 timecents -> 0 s), no SampleID.  The zero attack
	 * keeps the rendered level constant from the very first frame. */
	gen_short(&pdta, GEN_InitialAttenuation, 50); /* amount in centi-dB: 50 = 0.5 dB */
	gen_short(&pdta, 34 /* AttackVolEnv */, -12000);
	/* Every zone sets its velocity range explicitly: an instrument zone
	 * without a VelRange generator is cleared to lovel=hivel=0 by tsf, which
	 * would make it unplayable, whereas the SF2 default range is 0-127. */
	/* zone 0: key 0-47, vel 1-63, S0 */
	gen_range(&pdta, GEN_KeyRange, 0, 47);
	gen_range(&pdta, GEN_VelRange, 1, 63);
	gen_word(&pdta, GEN_SampleID, 0);
	/* zone 1: key 0-47, vel 64-127, S1 */
	gen_range(&pdta, GEN_KeyRange, 0, 47);
	gen_range(&pdta, GEN_VelRange, 64, 127);
	gen_word(&pdta, GEN_SampleID, 1);
	/* zone 2: key 48-127, vel 1-90, S0 */
	gen_range(&pdta, GEN_KeyRange, 48, 127);
	gen_range(&pdta, GEN_VelRange, 1, 90);
	gen_word(&pdta, GEN_SampleID, 0);
	/* zone 3: key 48-127, vel 64-127, S1 */
	gen_range(&pdta, GEN_KeyRange, 48, 127);
	gen_range(&pdta, GEN_VelRange, 64, 127);
	gen_word(&pdta, GEN_SampleID, 1);
	/* zone 4: key 48-127, vel 91-127, S2 */
	gen_range(&pdta, GEN_KeyRange, 48, 127);
	gen_range(&pdta, GEN_VelRange, 91, 127);
	gen_word(&pdta, GEN_SampleID, 2);
	/* terminal record */
	gen_word(&pdta, 0, 0);
	sf2buf_endchunk(&pdta, ch);

	/* ---- shdr: 3 real samples + terminal record ---- */
	ch = sf2buf_beginchunk(&pdta, "shdr");
	for (i = 0, word_off = 0; i < NUM_SAMPLES; i++, word_off += SAMPLE_FRAMES)
		shdr_record(&pdta, samples[i].name, word_off, word_off + SAMPLE_FRAMES,
		            SAMPLE_RATE, 60, SAMPLE_TYPE_MONO);
	/* terminal "EOS" record, sample type 0 like the minimal reference font */
	shdr_record(&pdta, "EOS", sample_words, sample_words, 0, 0, 0);
	sf2buf_endchunk(&pdta, ch);

	sf2_wrap(out, &pdta, &smpl);
	sf2buf_free(&pdta);
	sf2buf_free(&smpl);
	return out;
}

/* ---------------------------------------------------------------------------------------------------------
 * Fixture 2: one instrument shared by several presets with different play ranges
 *
 * Samples (constant levels, root key 60, no loops):
 *   T0 level 256, T1 level 512, T2 level 1024
 *
 * Instrument "SharedInst":
 *   global zone : AttackVolEnv -12000 (instant attack, constant output level
 *                 from the first frame); default attenuation/pan otherwise
 *   zone 0: key 0-47,   SampleID T0
 *   zone 1: key 48-83,  SampleID T1
 *   zone 2: key 84-127, SampleID T2
 *
 * Presets (all bank 0):
 *   "LowHalf"   preset 0: preset zone key 0-47,   vel 0-127, instrument 0
 *   "HighHalf"  preset 1: preset zone key 64-127, vel 0-127, instrument 0
 *   "Layered"   preset 2: TWO overlapping preset zones referencing the same
 *                         instrument (key 40-69 and key 50-79).  Instrument
 *                         zones intersecting both preset zones are duplicated,
 *                         so notes in the overlap (50-69) stack two voices on
 *                         the same sample source.
 *
 * Each preset's range filters the instrument zones at load time (see the
 * ihikey/plokey intersection in tsf_load_presets), so silence and layering have
 * to be evaluated per preset even though the sample source is identical.
 * --------------------------------------------------------------------------------------------------------- */

static sf2buf* build_font_shared(void)
{
	static const fixture_sample samples[] = {
		{ "T0", 256  },
		{ "T1", 512  },
		{ "T2", 1024 },
	};
	enum { NUM_SAMPLES = 3 };
	const unsigned int sample_words = NUM_SAMPLES * SAMPLE_FRAMES;

	sf2buf pdta, smpl, *out;
	sf2chunk_mark ch;
	unsigned int i, word_off;

	sf2buf_init(&pdta);
	sf2buf_init(&smpl);
	out = (sf2buf*)malloc(sizeof(sf2buf));

	for (i = 0; i < NUM_SAMPLES; i++) write_pcm_sample(&smpl, samples[i].level);
	sf2buf_zeros(&smpl, 46 * sizeof(short));

	/* ---- phdr: 3 presets + terminal (sorted by bank/preset at load time) ---- */
	ch = sf2buf_beginchunk(&pdta, "phdr");
	phdr_record(&pdta, "LowHalf",  0, 0, 0); /* bag 0 */
	phdr_record(&pdta, "HighHalf", 1, 0, 1); /* bag 1 */
	phdr_record(&pdta, "Layered",  2, 0, 2); /* bags 2..3 */
	phdr_record(&pdta, "EOP", 255, 255, 4);  /* real bag count; terminal bag follows in pbag */
	sf2buf_endchunk(&pdta, ch);

	/* ---- pbag: 4 preset zone bags + terminal (tsf needs the terminal) ---- */
	ch = sf2buf_beginchunk(&pdta, "pbag");
	/* bag 0 (LowHalf):  gens 0..2 ; bag 1 (HighHalf): gens 3..5
	 * bag 2 (Layered A): gens 6..8 ; bag 3 (Layered B): gens 9..11
	 * bag 4 terminal: genNdx 12 = total pgen count */
	sf2buf_u16(&pdta, 0);  sf2buf_u16(&pdta, 0);
	sf2buf_u16(&pdta, 3);  sf2buf_u16(&pdta, 0);
	sf2buf_u16(&pdta, 6);  sf2buf_u16(&pdta, 0);
	sf2buf_u16(&pdta, 9);  sf2buf_u16(&pdta, 0);
	sf2buf_u16(&pdta, 12); sf2buf_u16(&pdta, 0);
	sf2buf_endchunk(&pdta, ch);

	/* ---- pmod ---- */
	ch = sf2buf_beginchunk(&pdta, "pmod");
	sf2buf_zeros(&pdta, 10);
	sf2buf_endchunk(&pdta, ch);

	/* ---- pgen ---- */
	ch = sf2buf_beginchunk(&pdta, "pgen");
	/* LowHalf: key 0-47 */
	gen_range(&pdta, GEN_KeyRange, 0, 47);
	gen_range(&pdta, GEN_VelRange, 0, 127);
	gen_word(&pdta, GEN_Instrument, 0);
	/* HighHalf: key 64-127 */
	gen_range(&pdta, GEN_KeyRange, 64, 127);
	gen_range(&pdta, GEN_VelRange, 0, 127);
	gen_word(&pdta, GEN_Instrument, 0);
	/* Layered zone A: key 40-69 */
	gen_range(&pdta, GEN_KeyRange, 40, 69);
	gen_range(&pdta, GEN_VelRange, 0, 127);
	gen_word(&pdta, GEN_Instrument, 0);
	/* Layered zone B: key 50-79 (overlaps zone A on 50-69 -> stacked layers) */
	gen_range(&pdta, GEN_KeyRange, 50, 79);
	gen_range(&pdta, GEN_VelRange, 0, 127);
	gen_word(&pdta, GEN_Instrument, 0);
	/* terminal */
	gen_word(&pdta, 0, 0);
	sf2buf_endchunk(&pdta, ch);

	/* ---- inst ---- */
	ch = sf2buf_beginchunk(&pdta, "inst");
	inst_record(&pdta, "SharedInst", 0);
	inst_record(&pdta, "EOI", 4); /* 4 real ibags: global + 3 zones; terminal follows */
	sf2buf_endchunk(&pdta, ch);

	/* ---- ibag: global zone, 3 sample zones, terminal ---- */
	ch = sf2buf_beginchunk(&pdta, "ibag");
	/* igen layout: 0 (global), 1..3 (zone0), 4..6 (zone1), 7..9 (zone2),
	 * terminal genNdx 10 */
	sf2buf_u16(&pdta, 0);  sf2buf_u16(&pdta, 0);
	sf2buf_u16(&pdta, 1);  sf2buf_u16(&pdta, 0);
	sf2buf_u16(&pdta, 4);  sf2buf_u16(&pdta, 0);
	sf2buf_u16(&pdta, 7);  sf2buf_u16(&pdta, 0);
	sf2buf_u16(&pdta, 10); sf2buf_u16(&pdta, 0);
	sf2buf_endchunk(&pdta, ch);

	/* ---- imod ---- */
	ch = sf2buf_beginchunk(&pdta, "imod");
	sf2buf_zeros(&pdta, 10);
	sf2buf_endchunk(&pdta, ch);

	/* ---- igen ---- */
	ch = sf2buf_beginchunk(&pdta, "igen");
	/* global instrument zone: instant attack so levels are steady from frame 0 */
	gen_short(&pdta, 34 /* AttackVolEnv */, -12000);
	/* Explicit VelRange 0-127 on every zone (SF2 default; tsf otherwise clears
	 * zones without one to lovel=hivel=0). */
	gen_range(&pdta, GEN_KeyRange, 0, 47);
	gen_range(&pdta, GEN_VelRange, 0, 127);
	gen_word(&pdta, GEN_SampleID, 0);
	gen_range(&pdta, GEN_KeyRange, 48, 83);
	gen_range(&pdta, GEN_VelRange, 0, 127);
	gen_word(&pdta, GEN_SampleID, 1);
	gen_range(&pdta, GEN_KeyRange, 84, 127);
	gen_range(&pdta, GEN_VelRange, 0, 127);
	gen_word(&pdta, GEN_SampleID, 2);
	gen_word(&pdta, 0, 0);
	sf2buf_endchunk(&pdta, ch);

	/* ---- shdr ---- */
	ch = sf2buf_beginchunk(&pdta, "shdr");
	for (i = 0, word_off = 0; i < NUM_SAMPLES; i++, word_off += SAMPLE_FRAMES)
		shdr_record(&pdta, samples[i].name, word_off, word_off + SAMPLE_FRAMES,
		            SAMPLE_RATE, 60, SAMPLE_TYPE_MONO);
	shdr_record(&pdta, "EOS", sample_words, sample_words, 0, 0, 0);
	sf2buf_endchunk(&pdta, ch);

	sf2_wrap(out, &pdta, &smpl);
	sf2buf_free(&pdta);
	sf2buf_free(&smpl);
	return out;
}

/* ---------------------------------------------------------------------------------------------------------
 * Test helpers
 * --------------------------------------------------------------------------------------------------------- */

/* Velocity float that maps exactly to the given MIDI velocity (tsf_note_on
 * converts with (short)(vel * 127)); 0.25 keeps the value clear of truncation
 * boundaries for every MIDI velocity in 1..127. */
static float vel_for(int midi_velocity)
{
	return ((float)midi_velocity + 0.25f) / 127.0f;
}

static tsf* load_fixture(const sf2buf* font)
{
	tsf* f = tsf_load_memory(font->data, (int)font->size);
	CHECK(f != NULL);
	if (f) tsf_set_output(f, TSF_MONO, SAMPLE_RATE, 0);
	return f;
}

/* Sample index 0..n-1 backing a region, identified through its sample offset. */
static int region_sample_index(const tsf* f, int preset_index, int region_index, int frames_per_sample)
{
	unsigned int off = f->presets[preset_index].regions[region_index].offset;
	return (int)(off / (unsigned int)frames_per_sample);
}

static int find_region_index(const tsf* f, int preset_index, unsigned char lo, unsigned char hi,
                             unsigned char vlo, unsigned char vhi)
{
	int i, n = f->presets[preset_index].regionNum;
	for (i = 0; i < n; i++)
	{
		const struct tsf_region* r = &f->presets[preset_index].regions[i];
		if (r->lokey == lo && r->hikey == hi && r->lovel == vlo && r->hivel == vhi) return i;
	}
	return -1;
}

/* Count active voices whose region offset points at the given fixture sample. */
static int count_voices_on_sample(const tsf* f, int sample_index, int frames_per_sample)
{
	int count = 0, i;
	unsigned int base = (unsigned int)sample_index * (unsigned int)frames_per_sample;
	for (i = 0; i < f->voiceNum; i++)
	{
		struct tsf_voice* v = &f->voices[i];
		if (v->playingPreset != -1 && v->region->offset == base) count++;
	}
	return count;
}

static int count_voices_in_preset(const tsf* f, int preset_index)
{
	int count = 0, i;
	for (i = 0; i < f->voiceNum; i++)
		if (f->voices[i].playingPreset == preset_index) count++;
	return count;
}

/* Voices still sounding (not yet in their release segment).  tsf_note_off moves
 * a voice into release but keeps it allocated until the envelope finishes. */
static int count_sounding_voices_in_preset(const tsf* f, int preset_index)
{
	int count = 0, i;
	for (i = 0; i < f->voiceNum; i++)
	{
		const struct tsf_voice* v = &f->voices[i];
		if (v->playingPreset == preset_index && v->ampenv.segment < 6 /* TSF_SEGMENT_RELEASE */)
			count++;
	}
	return count;
}

/* Render a handful of frames while the default (instant) envelope is at full
 * level and return the mean absolute output.  Constant-level samples make this
 * equal to the sum of the gains of the layers actually sounding. */
static float rendered_level(tsf* f)
{
	float buf[32];
	double sum = 0;
	int i;
	tsf_render_float(f, buf, 32, 0);
	for (i = 0; i < 32; i++) sum += fabs((double)buf[i]);
	return (float)(sum / 32.0);
}

static int buffer_is_silent(tsf* f)
{
	float buf[32];
	int i;
	tsf_render_float(f, buf, 32, 0);
	for (i = 0; i < 32; i++) if (fabs(buf[i]) > 1e-9f) return 0;
	return 1;
}

/* Persist the in-memory fixtures so they can be examined as standalone SF2
 * files; this is the "small timbre generated along with the tests" deliverable. */
static void dump_fixture(const char* filename, const sf2buf* font)
{
	const char* outdir = getenv("TSF_TEST_OUT");
	char path[512];
	FILE* fp;
	if (getenv("TSF_TEST_NODUMP")) return;
	if (!outdir) outdir = "generated";
#ifdef _WIN32
	_mkdir(outdir);
#else
	mkdir(outdir, 0755); /* harmless if it already exists */
#endif
	snprintf(path, sizeof(path), "%s/%s", outdir, filename);
	fp = fopen(path, "wb");
	CHECK(fp != NULL);
	if (fp)
	{
		fwrite(font->data, 1, font->size, fp);
		fclose(fp);
		printf("    fixture written: %s (%u bytes)\n", path, font->size);
	}
}

/* ---------------------------------------------------------------------------------------------------------
 * Tests for fixture 1: key splits, velocity layers, inheritance, velocity blend
 * --------------------------------------------------------------------------------------------------------- */

static sf2buf* font_keyvel; /* built once in main() */

static void test_keyvel_structure(void)
{
	tsf* f = load_fixture(font_keyvel);
	int idx, i;
	if (!f) return;

	CHECK_INT(tsf_get_presetcount(f), 1);
	CHECK_INT(tsf_get_presetindex(f, 0, 0), 0);
	CHECK(strcmp(tsf_get_presetname(f, 0), "LayeredPreset") == 0);

	/* 5 instrument zones all survive the unfiltered preset zone. */
	CHECK_INT(f->presets[0].regionNum, 5);

	/* Local key/vel ranges survive, in instrument zone order. */
	/* region 0: key 0-47 vel 1-63 S0 */
	idx = find_region_index(f, 0, 0, 47, 1, 63);
	CHECK(idx >= 0);
	if (idx >= 0) CHECK_INT(region_sample_index(f, 0, idx, SAMPLE_FRAMES), 0);

	/* region 1: key 0-47 vel 64-127 S1 */
	idx = find_region_index(f, 0, 0, 47, 64, 127);
	CHECK(idx >= 0);
	if (idx >= 0) CHECK_INT(region_sample_index(f, 0, idx, SAMPLE_FRAMES), 1);

	/* region 2: key 48-127 vel 1-90 S0 */
	idx = find_region_index(f, 0, 48, 127, 1, 90);
	CHECK(idx >= 0);
	if (idx >= 0) CHECK_INT(region_sample_index(f, 0, idx, SAMPLE_FRAMES), 0);

	/* region 3: key 48-127 vel 64-127 S1 */
	idx = find_region_index(f, 0, 48, 127, 64, 127);
	CHECK(idx >= 0);
	if (idx >= 0) CHECK_INT(region_sample_index(f, 0, idx, SAMPLE_FRAMES), 1);

	/* region 4: key 48-127 vel 91-127 S2 */
	idx = find_region_index(f, 0, 48, 127, 91, 127);
	CHECK(idx >= 0);
	if (idx >= 0) CHECK_INT(region_sample_index(f, 0, idx, SAMPLE_FRAMES), 2);

	/* Inheritance: every region carries the instrument global attenuation
	 * (0.5 dB) and the preset global pan (-0.1). */
	for (i = 0; i < f->presets[0].regionNum; i++)
	{
		struct tsf_region* r = &f->presets[0].regions[i];
		CHECK_FLOAT_NEAR(r->attenuation, 0.5, 1e-6);
		CHECK_FLOAT_NEAR(r->pan, -0.1, 1e-6);
	}

	tsf_close(f);
}

static void test_keyvel_low_key_single_layers(void)
{
	tsf* f = load_fixture(font_keyvel);
	if (!f) return;

	/* key 24, soft velocity -> only S0 (zone 0) */
	CHECK(tsf_note_on(f, 0, 24, vel_for(40)));
	CHECK_INT(tsf_active_voice_count(f), 1);
	CHECK_INT(count_voices_on_sample(f, 0, SAMPLE_FRAMES), 1);
	CHECK_INT(count_voices_on_sample(f, 1, SAMPLE_FRAMES), 0);
	tsf_close(f);

	f = load_fixture(font_keyvel);
	/* key 24, hard velocity -> only S1 (zone 1) */
	CHECK(tsf_note_on(f, 0, 24, vel_for(120)));
	CHECK_INT(tsf_active_voice_count(f), 1);
	CHECK_INT(count_voices_on_sample(f, 0, SAMPLE_FRAMES), 0);
	CHECK_INT(count_voices_on_sample(f, 1, SAMPLE_FRAMES), 1);
	tsf_close(f);
}

static void test_keyvel_high_key_velocity_blend(void)
{
	tsf* f;
	const double gain_05db = pow(10.0, -0.5 / 20.0); /* attenuation inherited on every layer */

	/* Layer selection at representative velocities (which samples sound). */

	/* Soft: key 60 vel 50 -> zone 2 only, S0 */
	f = load_fixture(font_keyvel);
	CHECK(tsf_note_on(f, 0, 60, vel_for(50)));
	CHECK_INT(tsf_active_voice_count(f), 1);
	CHECK_INT(count_voices_on_sample(f, 0, SAMPLE_FRAMES), 1);
	CHECK_INT(count_voices_on_sample(f, 1, SAMPLE_FRAMES), 0);
	tsf_close(f);

	/* Overlap: key 60 vel 80 -> zones 2 and 3 together, S0 + S1 blended */
	f = load_fixture(font_keyvel);
	CHECK(tsf_note_on(f, 0, 60, vel_for(80)));
	CHECK_INT(tsf_active_voice_count(f), 2);
	CHECK_INT(count_voices_on_sample(f, 0, SAMPLE_FRAMES), 1);
	CHECK_INT(count_voices_on_sample(f, 1, SAMPLE_FRAMES), 1);
	CHECK_INT(count_voices_on_sample(f, 2, SAMPLE_FRAMES), 0);
	tsf_close(f);

	/* Loud: key 60 vel 110 -> zones 3 and 4 together, S1 + S2 blended */
	f = load_fixture(font_keyvel);
	CHECK(tsf_note_on(f, 0, 60, vel_for(110)));
	CHECK_INT(tsf_active_voice_count(f), 2);
	CHECK_INT(count_voices_on_sample(f, 1, SAMPLE_FRAMES), 1);
	CHECK_INT(count_voices_on_sample(f, 2, SAMPLE_FRAMES), 1);
	CHECK_INT(count_voices_on_sample(f, 0, SAMPLE_FRAMES), 0);
	tsf_close(f);

	/* Audible level at FULL velocity (velocity gain = 1.0) equals the subset
	 * sum of the sample levels, attenuated by the inherited 0.5 dB.  Rendering
	 * at the root key keeps the resampling pitch ratio at exactly 1.0. */

	/* key 60, full velocity -> zones 3 (S1) + 4 (S2), level = (S1+S2) */
	f = load_fixture(font_keyvel);
	CHECK(tsf_note_on(f, 0, 60, 1.0f));
	CHECK_INT(tsf_active_voice_count(f), 2);
	CHECK_INT(count_voices_on_sample(f, 1, SAMPLE_FRAMES), 1);
	CHECK_INT(count_voices_on_sample(f, 2, SAMPLE_FRAMES), 1);
	CHECK_FLOAT_NEAR(rendered_level(f), (2048.0 + 4096.0) / 32767.0 * gain_05db,
	                 0.02 * ((2048.0 + 4096.0) / 32767.0));
	tsf_close(f);

	/* Single-layer full-velocity level: low split key 24 hard -> S1 only. */
	f = load_fixture(font_keyvel);
	CHECK(tsf_note_on(f, 0, 24, 1.0f));
	CHECK_INT(tsf_active_voice_count(f), 1);
	CHECK_INT(count_voices_on_sample(f, 1, SAMPLE_FRAMES), 1);
	CHECK_FLOAT_NEAR(rendered_level(f), 2048.0 / 32767.0 * gain_05db,
	                 0.02 * (2048.0 / 32767.0));
	tsf_close(f);

	/* And S0 alone via the low split soft layer, full velocity cannot reach it;
	 * use the high-split soft zone at a velocity that selects only S0 and
	 * account for the velocity gain explicitly. */
	f = load_fixture(font_keyvel);
	{
		const int midi_vel = 50;
		const double vel_gain = (double)midi_vel / 127.0; /* tsf velocity gain */
		CHECK(tsf_note_on(f, 0, 60, vel_for(midi_vel)));
		CHECK_INT(tsf_active_voice_count(f), 1);
		CHECK_INT(count_voices_on_sample(f, 0, SAMPLE_FRAMES), 1);
		CHECK_FLOAT_NEAR(rendered_level(f),
		                 1024.0 / 32767.0 * gain_05db * vel_gain,
		                 0.03 * (1024.0 / 32767.0 * vel_gain));
	}
	tsf_close(f);
}

static void test_keyvel_range_boundaries(void)
{
	tsf* f;

	/* Velocity boundary inside the high key split: vel 63 -> S0 only */
	f = load_fixture(font_keyvel);
	CHECK(tsf_note_on(f, 0, 60, vel_for(63)));
	CHECK_INT(tsf_active_voice_count(f), 1);
	CHECK_INT(count_voices_on_sample(f, 0, SAMPLE_FRAMES), 1);
	tsf_close(f);

	/* vel 64 -> S0 + S1 (overlap starts) */
	f = load_fixture(font_keyvel);
	CHECK(tsf_note_on(f, 0, 60, vel_for(64)));
	CHECK_INT(tsf_active_voice_count(f), 2);
	CHECK_INT(count_voices_on_sample(f, 0, SAMPLE_FRAMES), 1);
	CHECK_INT(count_voices_on_sample(f, 1, SAMPLE_FRAMES), 1);
	tsf_close(f);

	/* vel 90 -> still S0 + S1; vel 91 -> S1 + S2 */
	f = load_fixture(font_keyvel);
	CHECK(tsf_note_on(f, 0, 60, vel_for(90)));
	CHECK_INT(tsf_active_voice_count(f), 2);
	CHECK_INT(count_voices_on_sample(f, 0, SAMPLE_FRAMES), 1);
	CHECK_INT(count_voices_on_sample(f, 2, SAMPLE_FRAMES), 0);
	tsf_close(f);

	f = load_fixture(font_keyvel);
	CHECK(tsf_note_on(f, 0, 60, vel_for(91)));
	CHECK_INT(tsf_active_voice_count(f), 2);
	CHECK_INT(count_voices_on_sample(f, 0, SAMPLE_FRAMES), 0);
	CHECK_INT(count_voices_on_sample(f, 2, SAMPLE_FRAMES), 1);
	tsf_close(f);

	/* Key boundary 47/48: low split at 47 gives one layer, high split at 48
	 * can give two (velocity dependent). */
	f = load_fixture(font_keyvel);
	CHECK(tsf_note_on(f, 0, 47, vel_for(80)));
	CHECK_INT(tsf_active_voice_count(f), 1);
	CHECK_INT(count_voices_on_sample(f, 1, SAMPLE_FRAMES), 1);
	tsf_close(f);

	f = load_fixture(font_keyvel);
	CHECK(tsf_note_on(f, 0, 48, vel_for(80)));
	CHECK_INT(tsf_active_voice_count(f), 2);
	CHECK_INT(count_voices_on_sample(f, 0, SAMPLE_FRAMES), 1);
	CHECK_INT(count_voices_on_sample(f, 1, SAMPLE_FRAMES), 1);
	tsf_close(f);

	/* Lowest and highest MIDI keys still map to defined layers. */
	f = load_fixture(font_keyvel);
	CHECK(tsf_note_on(f, 0, 0, vel_for(40)));
	CHECK_INT(tsf_active_voice_count(f), 1);
	CHECK_INT(count_voices_on_sample(f, 0, SAMPLE_FRAMES), 1);
	tsf_close(f);

	f = load_fixture(font_keyvel);
	CHECK(tsf_note_on(f, 0, 127, vel_for(127)));
	CHECK_INT(tsf_active_voice_count(f), 2);
	CHECK_INT(count_voices_on_sample(f, 1, SAMPLE_FRAMES), 1);
	CHECK_INT(count_voices_on_sample(f, 2, SAMPLE_FRAMES), 1);
	tsf_close(f);
}

static void test_keyvel_velocity_gain(void)
{
	/* A non-maximum velocity scales the audible output; the layer set stays
	 * the same, only the gain changes (tsf note gain = -gainToDecibels(1/vel)). */
	tsf* f = load_fixture(font_keyvel);
	float level_soft, level_hard;
	CHECK(tsf_note_on(f, 0, 24, vel_for(64)));
	level_soft = rendered_level(f);
	tsf_close(f);

	f = load_fixture(font_keyvel);
	CHECK(tsf_note_on(f, 0, 24, vel_for(127)));
	level_hard = rendered_level(f);
	CHECK(level_hard > level_soft);
	/* 64/127 velocity gain ratio, within rendering tolerance. */
	CHECK_FLOAT_NEAR(level_soft / level_hard, 64.0 / 127.0, 0.01);
	tsf_close(f);
}

/* ---------------------------------------------------------------------------------------------------------
 * Tests for fixture 2: shared instrument, per-preset play ranges, stacking, silence
 * --------------------------------------------------------------------------------------------------------- */

static sf2buf* font_shared;

static void test_shared_structure(void)
{
	tsf* f = load_fixture(font_shared);
	int p;
	if (!f) return;

	CHECK_INT(tsf_get_presetcount(f), 3);
	CHECK_INT(tsf_get_presetindex(f, 0, 0), 0);
	CHECK_INT(tsf_get_presetindex(f, 0, 1), 1);
	CHECK_INT(tsf_get_presetindex(f, 0, 2), 2);
	CHECK(strcmp(tsf_get_presetname(f, 0), "LowHalf") == 0);
	CHECK(strcmp(tsf_get_presetname(f, 1), "HighHalf") == 0);
	CHECK(strcmp(tsf_get_presetname(f, 2), "Layered") == 0);

	/* LowHalf keeps instrument zones intersecting key 0-47: T0 only. */
	CHECK_INT(f->presets[0].regionNum, 1);
	CHECK_INT(region_sample_index(f, 0, 0, SAMPLE_FRAMES), 0);
	CHECK_INT(f->presets[0].regions[0].lokey, 0);
	CHECK_INT(f->presets[0].regions[0].hikey, 47);

	/* HighHalf keeps zones intersecting key 64-127: T1 (clamped to 64-83)
	 * and T2. */
	CHECK_INT(f->presets[1].regionNum, 2);
	CHECK_INT(find_region_index(f, 1, 64, 83, 0, 127) >= 0, 1);
	CHECK_INT(find_region_index(f, 1, 84, 127, 0, 127) >= 0, 1);
	CHECK_INT(region_sample_index(f, 1, find_region_index(f, 1, 64, 83, 0, 127), SAMPLE_FRAMES), 1);
	CHECK_INT(region_sample_index(f, 1, find_region_index(f, 1, 84, 127, 0, 127), SAMPLE_FRAMES), 2);

	/* Layered: each of the two overlapping preset zones duplicates the
	 * instrument zones it intersects.  Zone A (40-69) keeps T0 and a T1
	 * clamped to 48-69; zone B (50-79) keeps a T1 clamped to 50-79.  The two
	 * T1 regions overlap on keys 50-69 and stack there. */
	CHECK_INT(f->presets[2].regionNum, 3);
	{
		int t0 = 0, t1 = 0, t2 = 0, i;
		for (i = 0; i < 3; i++)
		{
			switch (region_sample_index(f, 2, i, SAMPLE_FRAMES))
			{
				case 0: t0++; break;
				case 1: t1++; break;
				case 2: t2++; break;
			}
		}
		CHECK_INT(t0, 1);
		CHECK_INT(t1, 2);
		CHECK_INT(t2, 0);
	}

	/* The duplicated T1 regions keep the two distinct preset zone ranges. */
	for (p = 0; p < f->presets[2].regionNum; p++)
	{
		struct tsf_region* r = &f->presets[2].regions[p];
		CHECK((r->hikey <= 79 && r->lokey >= 40));
	}

	tsf_close(f);
}

static void test_shared_lowhalf_preset(void)
{
	tsf* f = load_fixture(font_shared);
	if (!f) return;

	/* In range: T0 sounds. */
	CHECK(tsf_note_on(f, 0, 24, 1.0f));
	CHECK_INT(tsf_active_voice_count(f), 1);
	CHECK_INT(count_voices_on_sample(f, 0, SAMPLE_FRAMES), 1);
	CHECK_INT(count_voices_in_preset(f, 0), 1);
	tsf_close(f);

	/* Edge of the preset range. */
	f = load_fixture(font_shared);
	CHECK(tsf_note_on(f, 0, 47, 1.0f));
	CHECK_INT(tsf_active_voice_count(f), 1);
	CHECK_INT(count_voices_on_sample(f, 0, SAMPLE_FRAMES), 1);
	tsf_close(f);

	/* Out of range: preset filters every instrument zone -> silence. */
	f = load_fixture(font_shared);
	CHECK(tsf_note_on(f, 0, 48, 1.0f));
	CHECK_INT(tsf_active_voice_count(f), 0);
	CHECK(buffer_is_silent(f));
	tsf_close(f);

	f = load_fixture(font_shared);
	CHECK(tsf_note_on(f, 0, 100, 1.0f));
	CHECK_INT(tsf_active_voice_count(f), 0);
	CHECK(buffer_is_silent(f));
	tsf_close(f);

	/* Zero velocity never starts a voice. */
	f = load_fixture(font_shared);
	CHECK(tsf_note_on(f, 0, 24, 0.0f));
	CHECK_INT(tsf_active_voice_count(f), 0);
	CHECK(buffer_is_silent(f));
	tsf_close(f);
}

static void test_shared_highhalf_preset(void)
{
	tsf* f = load_fixture(font_shared);
	if (!f) return;

	/* Below the preset range even though the instrument has zones there. */
	CHECK(tsf_note_on(f, 1, 48, 1.0f));
	CHECK_INT(tsf_active_voice_count(f), 0);
	CHECK(buffer_is_silent(f));
	tsf_close(f);

	/* T1 portion, clamped to start at key 64. */
	f = load_fixture(font_shared);
	CHECK(tsf_note_on(f, 1, 64, 1.0f));
	CHECK_INT(tsf_active_voice_count(f), 1);
	CHECK_INT(count_voices_on_sample(f, 1, SAMPLE_FRAMES), 1);
	tsf_close(f);

	f = load_fixture(font_shared);
	CHECK(tsf_note_on(f, 1, 83, 1.0f));
	CHECK_INT(tsf_active_voice_count(f), 1);
	CHECK_INT(count_voices_on_sample(f, 1, SAMPLE_FRAMES), 1);
	tsf_close(f);

	/* T2 portion. */
	f = load_fixture(font_shared);
	CHECK(tsf_note_on(f, 1, 100, 1.0f));
	CHECK_INT(tsf_active_voice_count(f), 1);
	CHECK_INT(count_voices_on_sample(f, 2, SAMPLE_FRAMES), 1);
	tsf_close(f);

	f = load_fixture(font_shared);
	CHECK(tsf_note_on(f, 1, 127, 1.0f));
	CHECK_INT(tsf_active_voice_count(f), 1);
	CHECK_INT(count_voices_on_sample(f, 2, SAMPLE_FRAMES), 1);
	tsf_close(f);
}

static void test_shared_layered_preset(void)
{
	tsf* f;

	/* key 45: only preset zone A (40-69) matches -> single T0 layer. */
	f = load_fixture(font_shared);
	CHECK(tsf_note_on(f, 2, 45, 1.0f));
	CHECK_INT(tsf_active_voice_count(f), 1);
	CHECK_INT(count_voices_on_sample(f, 0, SAMPLE_FRAMES), 1);
	CHECK_FLOAT_NEAR(rendered_level(f), 256.0 / 32767.0,
	                 0.02 * (256.0 / 32767.0));
	tsf_close(f);

	/* key 60: both preset zones match (inside the 50-69 overlap), each
	 * contributes a T1 -> two voices on the SAME sample source, i.e. an
	 * audible double layer with doubled output level. */
	f = load_fixture(font_shared);
	CHECK(tsf_note_on(f, 2, 60, 1.0f));
	CHECK_INT(tsf_active_voice_count(f), 2);
	CHECK_INT(count_voices_on_sample(f, 1, SAMPLE_FRAMES), 2);
	CHECK_INT(count_voices_on_sample(f, 0, SAMPLE_FRAMES), 0);
	CHECK_FLOAT_NEAR(rendered_level(f), (512.0 + 512.0) / 32767.0,
	                 0.02 * (1024.0 / 32767.0));
	tsf_close(f);

	/* key 55 and 65 are inside the overlap too: same double T1 stack. */
	f = load_fixture(font_shared);
	CHECK(tsf_note_on(f, 2, 55, 1.0f));
	CHECK_INT(tsf_active_voice_count(f), 2);
	CHECK_INT(count_voices_on_sample(f, 1, SAMPLE_FRAMES), 2);
	tsf_close(f);

	f = load_fixture(font_shared);
	CHECK(tsf_note_on(f, 2, 65, 1.0f));
	CHECK_INT(tsf_active_voice_count(f), 2);
	CHECK_INT(count_voices_on_sample(f, 1, SAMPLE_FRAMES), 2);
	tsf_close(f);

	/* key 75: only preset zone B (50-79) matches -> single T1 layer. */
	f = load_fixture(font_shared);
	CHECK(tsf_note_on(f, 2, 75, 1.0f));
	CHECK_INT(tsf_active_voice_count(f), 1);
	CHECK_INT(count_voices_on_sample(f, 1, SAMPLE_FRAMES), 1);
	tsf_close(f);

	/* Outside every preset zone: silence regardless of instrument zones. */
	f = load_fixture(font_shared);
	CHECK(tsf_note_on(f, 2, 39, 1.0f));
	CHECK_INT(tsf_active_voice_count(f), 0);
	CHECK(buffer_is_silent(f));
	tsf_close(f);

	f = load_fixture(font_shared);
	CHECK(tsf_note_on(f, 2, 80, 1.0f));
	CHECK_INT(tsf_active_voice_count(f), 0);
	CHECK(buffer_is_silent(f));
	tsf_close(f);
}

static void test_shared_preset_independence(void)
{
	tsf* f = load_fixture(font_shared);
	if (!f) return;

	/* The same key in all three presets must be judged against each preset's
	 * own range, and voices must stay attributable to their preset.
	 * key 65: LowHalf is silent, HighHalf plays T1 once, Layered stacks T1
	 * twice (the two overlapping preset zones both match). */
	CHECK(tsf_note_on(f, 0, 65, 1.0f));
	CHECK(tsf_note_on(f, 1, 65, 1.0f));
	CHECK(tsf_note_on(f, 2, 65, 1.0f));

	CHECK_INT(count_voices_in_preset(f, 0), 0);
	CHECK_INT(count_voices_in_preset(f, 1), 1);
	CHECK_INT(count_voices_in_preset(f, 2), 2);
	CHECK_INT(tsf_active_voice_count(f), 3);
	/* Every audible voice traces back to the shared T1 sample, but through
	 * distinct presets whose ranges decided whether it sounds at all. */
	CHECK_INT(count_voices_on_sample(f, 1, SAMPLE_FRAMES), 3);

	/* Note-off on one preset must not touch the other presets' voices.  The
	 * released voice stays allocated while its release envelope plays out, so
	 * distinguish "still sounding" (before the release segment) from the
	 * released one. */
	tsf_note_off(f, 1, 65);
	CHECK_INT(count_sounding_voices_in_preset(f, 1), 0); /* HighHalf released */
	CHECK_INT(count_sounding_voices_in_preset(f, 2), 2); /* Layered untouched */
	CHECK_INT(count_sounding_voices_in_preset(f, 0), 0);
	/* Releasing HighHalf does not stop the Layered voices' audio. */
	CHECK_INT(count_voices_on_sample(f, 1, SAMPLE_FRAMES), 3);

	/* A note-off for a preset that had no voice is a harmless no-op. */
	tsf_note_off(f, 0, 65);
	CHECK_INT(count_sounding_voices_in_preset(f, 2), 2);

	tsf_close(f);
}

static void test_shared_bank_api_routing(void)
{
	/* The bank/preset lookup path applies the same per-preset filtering. */
	tsf* f = load_fixture(font_shared);
	if (!f) return;

	CHECK_INT(tsf_bank_note_on(f, 0, 1, 100, 1.0f), 1); /* HighHalf, T2 */
	CHECK_INT(tsf_active_voice_count(f), 1);
	CHECK_INT(count_voices_on_sample(f, 2, SAMPLE_FRAMES), 1);

	CHECK_INT(tsf_bank_note_on(f, 0, 0, 100, 1.0f), 1); /* LowHalf, out of range */
	CHECK_INT(tsf_active_voice_count(f), 1);

	CHECK_INT(tsf_bank_note_off(f, 0, 1, 100), 1);
	CHECK_INT(tsf_bank_note_off(f, 0, 99, 100), 0); /* unknown preset number */
	tsf_close(f);
}

/* ---------------------------------------------------------------------------------------------------------
 * Entry point
 * --------------------------------------------------------------------------------------------------------- */

int main(void)
{
	printf("Building fixture soundfonts in memory...\n");
	font_keyvel = build_font_keyvel();
	font_shared = build_font_shared();
	CHECK(font_keyvel->data != NULL);
	CHECK(font_shared->data != NULL);
	dump_fixture("layers_keyvel.sf2", font_keyvel);
	dump_fixture("layers_shared.sf2", font_shared);

	printf("\n=== Fixture 1: key splits, velocity layers and zone inheritance ===\n");
	RUN_TEST(test_keyvel_structure);
	RUN_TEST(test_keyvel_low_key_single_layers);
	RUN_TEST(test_keyvel_high_key_velocity_blend);
	RUN_TEST(test_keyvel_range_boundaries);
	RUN_TEST(test_keyvel_velocity_gain);

	printf("\n=== Fixture 2: shared instrument with per-preset play ranges ===\n");
	RUN_TEST(test_shared_structure);
	RUN_TEST(test_shared_lowhalf_preset);
	RUN_TEST(test_shared_highhalf_preset);
	RUN_TEST(test_shared_layered_preset);
	RUN_TEST(test_shared_preset_independence);
	RUN_TEST(test_shared_bank_api_routing);

	sf2buf_free(font_keyvel); free(font_keyvel);
	sf2buf_free(font_shared); free(font_shared);

	printf("\n========================================\n");
	printf("Tests run: %d, failed: %d\n", tests_run, tests_failed);
	printf("========================================\n");
	return tests_failed ? 1 : 0;
}
