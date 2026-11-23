/* Tiny in-memory SoundFont2 writer used by the playback unit tests.
 *
 * The tests need very small, fully controlled instruments. Two kinds of
 * probe are generated:
 *
 *   - a constant sample: rendered output is proportional to the amplitude
 *     envelope only, so the envelope can be observed in isolation.
 *   - a linear ramp sample: frame i holds the value i * RAMP_STEP, so the
 *     source playback position can be recovered from the rendered PCM by
 *     dividing the ramp voice by the constant voice (the shared envelope
 *     gain cancels out).
 *
 * Building the SoundFont in memory keeps the test self contained and avoids
 * committing opaque binary fixtures. Every preset gets its own sample,
 * instrument and instrument zone. Loop points, loop mode, pan, sample rate
 * and the root key come from the preset spec.
 */
#ifndef TSF_TEST_SF2_BUILDER_H
#define TSF_TEST_SF2_BUILDER_H

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* Two extra frames are appended after every sample. TinySoundFont advances
 * the region end by one frame and its linear interpolation reads one frame
 * ahead, so these frames keep every read inside the sample buffer. They are
 * filled by linear extrapolation of the last two sample frames (so a ramp
 * stays a ramp and a constant stays constant), which keeps the rendered PCM
 * decodable right up to the voice's end instead of introducing a silent step. */
#define TSF_TEST_SF2_GUARD_FRAMES 2

typedef struct tsf_test_preset_spec
{
	const char* name;
	int loop_mode;              /* 1 = continuous loop, 3 = sustain loop (SF2 sampleModes) */
	const int16_t* pcm;         /* mono frames, frame_count entries */
	unsigned int frame_count;
	unsigned int sample_rate;
	unsigned int loop_start;    /* frame offset of the loop start, relative to the sample */
	unsigned int loop_end;      /* frame offset one past the last looped frame, relative to the sample */
	int release_timecents;      /* ReleaseVolEnv generator amount (0 = one second) */
	int pan;                    /* Pan generator amount in 0.1% units, -500 = hard left, +500 = hard right */
	unsigned int original_pitch;
} tsf_test_preset_spec;

typedef struct tsf_test_bytebuf
{
	unsigned char* p;
	size_t n, cap;
} tsf_test_bytebuf;

static void tsf_test_bb_reserve(tsf_test_bytebuf* b, size_t extra)
{
	size_t need = b->n + extra;
	if (need <= b->cap) return;
	if (!b->cap) b->cap = 256;
	while (b->cap < need) b->cap *= 2;
	b->p = (unsigned char*)realloc(b->p, b->cap);
}

static void tsf_test_bb_put(tsf_test_bytebuf* b, const void* data, size_t size)
{
	tsf_test_bb_reserve(b, size);
	if (size) memcpy(b->p + b->n, data, size);
	b->n += size;
}

static void tsf_test_bb_u8(tsf_test_bytebuf* b, unsigned int v)
{
	unsigned char c = (unsigned char)v;
	tsf_test_bb_put(b, &c, 1);
}

static void tsf_test_bb_u16(tsf_test_bytebuf* b, unsigned int v)
{
	unsigned char c[2];
	c[0] = (unsigned char)(v & 0xFF); c[1] = (unsigned char)((v >> 8) & 0xFF);
	tsf_test_bb_put(b, c, 2);
}

static void tsf_test_bb_u32(tsf_test_bytebuf* b, uint32_t v)
{
	unsigned char c[4];
	c[0] = (unsigned char)(v & 0xFF); c[1] = (unsigned char)((v >> 8) & 0xFF);
	c[2] = (unsigned char)((v >> 16) & 0xFF); c[3] = (unsigned char)((v >> 24) & 0xFF);
	tsf_test_bb_put(b, c, 4);
}

static void tsf_test_bb_i16(tsf_test_bytebuf* b, int v)
{
	tsf_test_bb_u16(b, (unsigned int)(uint16_t)(int16_t)v);
}

static void tsf_test_bb_fourcc(tsf_test_bytebuf* b, const char* id)
{
	tsf_test_bb_put(b, id, 4);
}

static void tsf_test_bb_name20(tsf_test_bytebuf* b, const char* name)
{
	unsigned char buf[20];
	size_t len = name ? strlen(name) : 0;
	if (len > 19) len = 19;
	memset(buf, 0, sizeof(buf));
	if (len) memcpy(buf, name, len);
	tsf_test_bb_put(b, buf, sizeof(buf));
}

/* Appends a RIFF sub-chunk (id + size + data, word aligned). */
static void tsf_test_bb_chunk(tsf_test_bytebuf* out, const char* id, const tsf_test_bytebuf* data)
{
	tsf_test_bb_fourcc(out, id);
	tsf_test_bb_u32(out, (uint32_t)data->n);
	tsf_test_bb_put(out, data->p, data->n);
	if (data->n & 1) tsf_test_bb_u8(out, 0);
}

/* Appends a LIST chunk with a form type. */
static void tsf_test_bb_list(tsf_test_bytebuf* out, const char* form, const tsf_test_bytebuf* data)
{
	tsf_test_bb_fourcc(out, "LIST");
	tsf_test_bb_u32(out, (uint32_t)(data->n + 4));
	tsf_test_bb_fourcc(out, form);
	tsf_test_bb_put(out, data->p, data->n);
	if ((data->n + 4) & 1) tsf_test_bb_u8(out, 0);
}

/* Builds a complete .sf2 file image from the preset specs.
 * Returns a malloc'ed buffer that the caller must free and stores its size in
 * *out_size. Returns NULL on allocation failure. */
static unsigned char* tsf_test_build_sf2(const tsf_test_preset_spec* specs, int count, int* out_size)
{
	int i;
	tsf_test_bytebuf pdta, sdta, smpl, phdr, pbag, pmod, pgen, inst, ibag, imod, igen, shdr, out;
	unsigned int sample_offset = 0;
	int ok = 0;

	memset(&pdta, 0, sizeof(pdta)); memset(&sdta, 0, sizeof(sdta)); memset(&smpl, 0, sizeof(smpl));
	memset(&phdr, 0, sizeof(phdr)); memset(&pbag, 0, sizeof(pbag)); memset(&pmod, 0, sizeof(pmod));
	memset(&pgen, 0, sizeof(pgen)); memset(&inst, 0, sizeof(inst)); memset(&ibag, 0, sizeof(ibag));
	memset(&imod, 0, sizeof(imod)); memset(&igen, 0, sizeof(igen)); memset(&shdr, 0, sizeof(shdr));
	memset(&out, 0, sizeof(out));
	if (count < 0) return NULL;

	/* Sample data, followed by extrapolated guard frames. */
	for (i = 0; i < count; i++)
	{
		int f;
		int last = (int)specs[i].frame_count - 1;
		int step = (last >= 1 ? specs[i].pcm[last] - specs[i].pcm[last - 1] : 0);
		for (f = 0; f < (int)specs[i].frame_count; f++) tsf_test_bb_i16(&smpl, specs[i].pcm[f]);
		for (f = 1; f <= TSF_TEST_SF2_GUARD_FRAMES; f++)
		{
			int v = specs[i].pcm[last] + step * f;
			if (v > 32767) v = 32767;
			if (v < -32768) v = -32768;
			tsf_test_bb_i16(&smpl, v);
		}
	}

	/* phdr / pbag / pgen */
	for (i = 0; i < count; i++)
	{
		tsf_test_bb_name20(&phdr, specs[i].name);
		tsf_test_bb_u16(&phdr, (unsigned int)i); /* preset number */
		tsf_test_bb_u16(&phdr, 0);               /* bank */
		tsf_test_bb_u16(&phdr, (unsigned int)i); /* presetBagNdx */
		tsf_test_bb_u32(&phdr, 0);
		tsf_test_bb_u32(&phdr, 0);
		tsf_test_bb_u32(&phdr, 0);

		tsf_test_bb_u16(&pbag, (unsigned int)i); /* genNdx: one generator per preset zone */
		tsf_test_bb_u16(&pbag, 0);

		tsf_test_bb_u16(&pgen, 41);              /* Instrument */
		tsf_test_bb_u16(&pgen, (unsigned int)i);
	}
	tsf_test_bb_name20(&phdr, "EOH"); tsf_test_bb_u16(&phdr, 0); tsf_test_bb_u16(&phdr, 0);
	tsf_test_bb_u16(&phdr, (unsigned int)count); tsf_test_bb_u32(&phdr, 0); tsf_test_bb_u32(&phdr, 0); tsf_test_bb_u32(&phdr, 0);
	tsf_test_bb_u16(&pbag, (unsigned int)count); tsf_test_bb_u16(&pbag, 0);
	tsf_test_bb_u16(&pgen, 0); tsf_test_bb_u16(&pgen, 0);

	/* inst / ibag / igen. Four generators per instrument zone. */
	#define TSF_TEST_GENS_PER_ZONE 4
	for (i = 0; i < count; i++)
	{
		tsf_test_bb_name20(&inst, specs[i].name);
		tsf_test_bb_u16(&inst, (unsigned int)i); /* instBagNdx */

		tsf_test_bb_u16(&ibag, (unsigned int)(i * TSF_TEST_GENS_PER_ZONE));
		tsf_test_bb_u16(&ibag, 0);

		/* SampleID must come last: TinySoundFont materializes the region as
		 * soon as it sees it, so any generator after it would be dropped. */
		tsf_test_bb_u16(&igen, 54); tsf_test_bb_u16(&igen, (unsigned int)specs[i].loop_mode); /* sampleModes */
		tsf_test_bb_u16(&igen, 17); tsf_test_bb_i16(&igen, specs[i].pan);                    /* pan */
		tsf_test_bb_u16(&igen, 38); tsf_test_bb_i16(&igen, specs[i].release_timecents);      /* releaseVolEnv */
		tsf_test_bb_u16(&igen, 53); tsf_test_bb_u16(&igen, (unsigned int)i);                 /* sampleID */
	}
	#undef TSF_TEST_GENS_PER_ZONE
	tsf_test_bb_name20(&inst, "EOI"); tsf_test_bb_u16(&inst, (unsigned int)count);
	tsf_test_bb_u16(&ibag, (unsigned int)(count * 4)); tsf_test_bb_u16(&ibag, 0);
	tsf_test_bb_u16(&igen, 0); tsf_test_bb_u16(&igen, 0);

	/* shdr. startLoop/endLoop are absolute frame offsets in the smpl chunk. */
	for (i = 0; i < count; i++)
	{
		tsf_test_bb_name20(&shdr, specs[i].name);
		tsf_test_bb_u32(&shdr, sample_offset);                                  /* start */
		tsf_test_bb_u32(&shdr, sample_offset + specs[i].frame_count);          /* end */
		tsf_test_bb_u32(&shdr, sample_offset + specs[i].loop_start);           /* startLoop */
		tsf_test_bb_u32(&shdr, sample_offset + specs[i].loop_end);             /* endLoop */
		tsf_test_bb_u32(&shdr, specs[i].sample_rate);
		tsf_test_bb_u8(&shdr, specs[i].original_pitch);
		tsf_test_bb_u8(&shdr, 0);                                              /* pitchCorrection */
		tsf_test_bb_u16(&shdr, 0);                                             /* sampleLink */
		tsf_test_bb_u16(&shdr, 1);                                             /* mono sample */
		sample_offset += specs[i].frame_count + TSF_TEST_SF2_GUARD_FRAMES;
	}
	tsf_test_bb_name20(&shdr, "EOS");
	tsf_test_bb_u32(&shdr, 0); tsf_test_bb_u32(&shdr, 0); tsf_test_bb_u32(&shdr, 0);
	tsf_test_bb_u32(&shdr, 0); tsf_test_bb_u32(&shdr, 0); tsf_test_bb_u8(&shdr, 0);
	tsf_test_bb_u8(&shdr, 0); tsf_test_bb_u16(&shdr, 0); tsf_test_bb_u16(&shdr, 0);

	/* Terminal modulator records are required to be present. */
	tsf_test_bb_u32(&pmod, 0); tsf_test_bb_u32(&pmod, 0); tsf_test_bb_u16(&pmod, 0);
	tsf_test_bb_u32(&imod, 0); tsf_test_bb_u32(&imod, 0); tsf_test_bb_u16(&imod, 0);

	if (!phdr.p || !smpl.p || (count && (!pbag.p || !pgen.p || !inst.p || !ibag.p || !igen.p || !shdr.p))) goto done;

	tsf_test_bb_chunk(&pdta, "phdr", &phdr);
	tsf_test_bb_chunk(&pdta, "pbag", &pbag);
	tsf_test_bb_chunk(&pdta, "pmod", &pmod);
	tsf_test_bb_chunk(&pdta, "pgen", &pgen);
	tsf_test_bb_chunk(&pdta, "inst", &inst);
	tsf_test_bb_chunk(&pdta, "ibag", &ibag);
	tsf_test_bb_chunk(&pdta, "imod", &imod);
	tsf_test_bb_chunk(&pdta, "igen", &igen);
	tsf_test_bb_chunk(&pdta, "shdr", &shdr);
	tsf_test_bb_chunk(&sdta, "smpl", &smpl);

	tsf_test_bb_fourcc(&out, "RIFF");
	tsf_test_bb_u32(&out, 0); /* patched below */
	tsf_test_bb_fourcc(&out, "sfbk");
	tsf_test_bb_list(&out, "pdta", &pdta);
	tsf_test_bb_list(&out, "sdta", &sdta);
	if (out.n < 8) goto done;
	out.p[4] = (unsigned char)((out.n - 8) & 0xFF);
	out.p[5] = (unsigned char)(((out.n - 8) >> 8) & 0xFF);
	out.p[6] = (unsigned char)(((out.n - 8) >> 16) & 0xFF);
	out.p[7] = (unsigned char)(((out.n - 8) >> 24) & 0xFF);
	*out_size = (int)out.n;
	ok = 1;

done:
	free(pdta.p); free(sdta.p); free(smpl.p);
	free(phdr.p); free(pbag.p); free(pmod.p); free(pgen.p);
	free(inst.p); free(ibag.p); free(imod.p); free(igen.p); free(shdr.p);
	if (!ok) { free(out.p); return NULL; }
	return out.p;
}

#endif /* TSF_TEST_SF2_BUILDER_H */
