#!/usr/bin/env python3
# Generates keyvolenv.sf2, the minimal SoundFont 2.1 file used by
# test_keyvolenv.c to verify key-number-scaled volume envelopes
# (SF2 generators keyNumToVolEnvHold / keyNumToVolEnvDecay).
#
# The font contains a single preset (bank 0, preset 0) with one instrument
# and one looping 441 Hz sine sample (root key 60, 44100 Hz).  The sine
# frequency 441 Hz = 44100 / 100 gives an integer number of samples per
# cycle, so the loop is seamless and the rendered pitch is easy to measure
# independently from the amplitude envelope.
#
# Instrument generator values (all envelope times in SF2 timecents,
# i.e. 1200 * log2(seconds)):
#   holdVolEnv         = -2400 tc  -> 0.25 s base hold at key 60
#   decayVolEnv        = -1200 tc  -> 0.5  s base decay at key 60
#   sustainVolEnv      =   400 cb  -> 40 dB attenuation, sustain gain 0.01
#   keyNumToVolEnvHold =   100 tc/key   (hold scales x2 per octave down)
#   keyNumToVolEnvDecay=    50 tc/key   (decay scales x2 per 2 octaves down)
#   sampleModes        = 1 (loop continuously)
# attack/delay/release keep the SF2 default of -12000 tc (instant).
#
# Expected effective times at key K (per SF2 2.01 sec 8.1.2/8.1.3):
#   hold(K)  = 2^((-2400 + 100*(60-K))/1200) s
#   decay(K) = 2^((-1200 +  50*(60-K))/1200) s
#
# Usage: python3 gen_keyvolenv_sf2.py [output.sf2]

import math
import struct
import sys

SAMPLE_RATE = 44100
SINE_FREQ = 441.0          # 100 samples per cycle at 44100 Hz
CYCLES = 22                # 2200 samples, ~50 ms, looped seamlessly
SAMPLE_COUNT = int(SAMPLE_RATE / SINE_FREQ * CYCLES)  # 2200
ROOT_KEY = 60

# SF2 generator IDs (see SF2 2.01 spec, sec 8.1)
GEN_KEYRANGE = 43
GEN_HOLDVOLENV = 35
GEN_DECAYVOLENV = 36
GEN_SUSTAINVOLENV = 37
GEN_KEYNUMTOVOLENVHOLD = 39
GEN_KEYNUMTOVOLENVDECAY = 40
GEN_SAMPLEMODES = 54
GEN_SAMPLEID = 53

HOLD_TIMECENTS = -2400
DECAY_TIMECENTS = -1200
SUSTAIN_CENTIBELS = 400
KEYNUM_TO_HOLD = 100
KEYNUM_TO_DECAY = 50


def chunk(tag, payload):
    """RIFF chunk with 4-char tag, little-endian size and even padding."""
    data = tag + struct.pack('<I', len(payload)) + payload
    if len(payload) % 2:
        data += b'\0'
    return data


def list_chunk(kind, payload):
    return chunk(b'LIST', kind + payload)


def gen_amount(value):
    """Signed 16-bit generator amount."""
    return struct.pack('<h', value)


def gen_range(lo, hi):
    return struct.pack('<BB', lo, hi)


def build_sf2():
    # --- sample data: 16-bit mono sine, amplitude 0.5 ----------------------
    pcm = b''.join(
        struct.pack('<h', int(round(32767 * 0.5 *
                    math.sin(2.0 * math.pi * SINE_FREQ * i / SAMPLE_RATE))))
        for i in range(SAMPLE_COUNT))
    sdta = list_chunk(b'sdta', chunk(b'smpl', pcm))

    # --- hydra (pdta) -------------------------------------------------------
    # phdr: preset 0 at bank 0, plus terminal record.
    phdr = (b'KeyVolEnvTest\x00'.ljust(20, b'\0') +
            struct.pack('<HHHIii', 0, 0, 0, 0, 0, 0) +
            b'EOI'.ljust(20, b'\0') +
            struct.pack('<HHHIii', 0, 0, 1, 0, 0, 0))

    # pbag: one preset zone, plus terminal.
    pbag = struct.pack('<HH', 0, 0) + struct.pack('<HH', 1, 0)

    # pmod: no modulators, terminal only.
    pmod = b'\0' * 10

    # pgen: preset zone -> instrument 0, plus terminal.
    pgen = (struct.pack('<H', 41) + struct.pack('<H', 0) +
            struct.pack('<H', 0) + struct.pack('<H', 0))

    # inst: one instrument, plus terminal.
    inst = (b'KeyVolEnvInst\x00'.ljust(20, b'\0') + struct.pack('<H', 0) +
            b'EOI'.ljust(20, b'\0') + struct.pack('<H', 1))

    # ibag: one instrument zone, plus terminal.
    ibag = struct.pack('<HH', 0, 0) + struct.pack('<HH', 8, 0)

    # imod: no modulators, terminal only.
    imod = b'\0' * 10

    # igen: instrument zone generators.  sampleID must come last so the
    # zone's generators are all set before the sample is referenced.
    igens = [
        (GEN_KEYRANGE, gen_range(0, 127)),
        (GEN_HOLDVOLENV, gen_amount(HOLD_TIMECENTS)),
        (GEN_DECAYVOLENV, gen_amount(DECAY_TIMECENTS)),
        (GEN_SUSTAINVOLENV, gen_amount(SUSTAIN_CENTIBELS)),
        (GEN_KEYNUMTOVOLENVHOLD, gen_amount(KEYNUM_TO_HOLD)),
        (GEN_KEYNUMTOVOLENVDECAY, gen_amount(KEYNUM_TO_DECAY)),
        (GEN_SAMPLEMODES, gen_amount(1)),  # loop continuously
        (GEN_SAMPLEID, struct.pack('<H', 0)),
    ]
    igen = b''.join(struct.pack('<H', op) + amount for op, amount in igens)
    igen += struct.pack('<H', 0) + struct.pack('<H', 0)  # terminal

    # shdr: one sample header, plus terminal.
    shdr = (b'sine441\x00'.ljust(20, b'\0') +
            struct.pack('<IIIII', 0, SAMPLE_COUNT, 0, SAMPLE_COUNT,
                        SAMPLE_RATE) +
            struct.pack('<BbH', ROOT_KEY, 0, 0) +
            struct.pack('<H', 1))  # mono sample
    shdr += b'EOS'.ljust(46, b'\0')

    pdta = list_chunk(b'pdta',
                      chunk(b'phdr', phdr) + chunk(b'pbag', pbag) +
                      chunk(b'pmod', pmod) + chunk(b'pgen', pgen) +
                      chunk(b'inst', inst) + chunk(b'ibag', ibag) +
                      chunk(b'imod', imod) + chunk(b'igen', igen) +
                      chunk(b'shdr', shdr))

    # --- INFO (optional metadata) -------------------------------------------
    info = list_chunk(b'INFO',
                      chunk(b'ifil', struct.pack('<HH', 2, 1)) +
                      chunk(b'INAM', b'Key-scaled volume envelope test font'))

    return chunk(b'RIFF', b'sfbk' + info + sdta + pdta)


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else 'keyvolenv.sf2'
    data = build_sf2()
    with open(out, 'wb') as f:
        f.write(data)
    print('wrote %s (%d bytes, %d samples)' % (out, len(data), SAMPLE_COUNT))


if __name__ == '__main__':
    main()
