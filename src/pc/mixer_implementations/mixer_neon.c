#ifdef __ARM_NEON // Useful for debugging

#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <ultra64.h>
#include <arm_neon.h>

/*
 * Stock mixer.c ARM Neon implementation.
 * Enhanced RSPA emulation is not supported.
 */

#pragma GCC optimize ("unroll-loops")

#define ROUND_UP_32(v) (((v) + 31) & ~31)
#define ROUND_UP_16(v) (((v) + 15) & ~15)
#define ROUND_UP_8(v)  (((v) +  7) &  ~7)

static struct {
    uint16_t in;
    uint16_t out;
    uint16_t nbytes;

    int16_t vol[2];

    uint16_t dry_right;
    uint16_t wet_left;
    uint16_t wet_right;

    int16_t target[2];
    int32_t rate[2];

    int16_t vol_dry;
    int16_t vol_wet;

    ADPCM_STATE *adpcm_loop_state;

    int16_t adpcm_table[8][2][8];
    union {
        int16_t as_s16[2512 / sizeof(int16_t)];
        uint8_t as_u8[2512];
    } buf;
} rspa;

static int16_t resample_table[64][4] = {
    {0x0c39, 0x66ad, 0x0d46, 0xffdf}, {0x0b39, 0x6696, 0x0e5f, 0xffd8},
    {0x0a44, 0x6669, 0x0f83, 0xffd0}, {0x095a, 0x6626, 0x10b4, 0xffc8},
    {0x087d, 0x65cd, 0x11f0, 0xffbf}, {0x07ab, 0x655e, 0x1338, 0xffb6},
    {0x06e4, 0x64d9, 0x148c, 0xffac}, {0x0628, 0x643f, 0x15eb, 0xffa1},
    {0x0577, 0x638f, 0x1756, 0xff96}, {0x04d1, 0x62cb, 0x18cb, 0xff8a},
    {0x0435, 0x61f3, 0x1a4c, 0xff7e}, {0x03a4, 0x6106, 0x1bd7, 0xff71},
    {0x031c, 0x6007, 0x1d6c, 0xff64}, {0x029f, 0x5ef5, 0x1f0b, 0xff56},
    {0x022a, 0x5dd0, 0x20b3, 0xff48}, {0x01be, 0x5c9a, 0x2264, 0xff3a},
    {0x015b, 0x5b53, 0x241e, 0xff2c}, {0x0101, 0x59fc, 0x25e0, 0xff1e},
    {0x00ae, 0x5896, 0x27a9, 0xff10}, {0x0063, 0x5720, 0x297a, 0xff02},
    {0x001f, 0x559d, 0x2b50, 0xfef4}, {0xffe2, 0x540d, 0x2d2c, 0xfee8},
    {0xffac, 0x5270, 0x2f0d, 0xfedb}, {0xff7c, 0x50c7, 0x30f3, 0xfed0},
    {0xff53, 0x4f14, 0x32dc, 0xfec6}, {0xff2e, 0x4d57, 0x34c8, 0xfebd},
    {0xff0f, 0x4b91, 0x36b6, 0xfeb6}, {0xfef5, 0x49c2, 0x38a5, 0xfeb0},
    {0xfedf, 0x47ed, 0x3a95, 0xfeac}, {0xfece, 0x4611, 0x3c85, 0xfeab},
    {0xfec0, 0x4430, 0x3e74, 0xfeac}, {0xfeb6, 0x424a, 0x4060, 0xfeaf},
    {0xfeaf, 0x4060, 0x424a, 0xfeb6}, {0xfeac, 0x3e74, 0x4430, 0xfec0},
    {0xfeab, 0x3c85, 0x4611, 0xfece}, {0xfeac, 0x3a95, 0x47ed, 0xfedf},
    {0xfeb0, 0x38a5, 0x49c2, 0xfef5}, {0xfeb6, 0x36b6, 0x4b91, 0xff0f},
    {0xfebd, 0x34c8, 0x4d57, 0xff2e}, {0xfec6, 0x32dc, 0x4f14, 0xff53},
    {0xfed0, 0x30f3, 0x50c7, 0xff7c}, {0xfedb, 0x2f0d, 0x5270, 0xffac},
    {0xfee8, 0x2d2c, 0x540d, 0xffe2}, {0xfef4, 0x2b50, 0x559d, 0x001f},
    {0xff02, 0x297a, 0x5720, 0x0063}, {0xff10, 0x27a9, 0x5896, 0x00ae},
    {0xff1e, 0x25e0, 0x59fc, 0x0101}, {0xff2c, 0x241e, 0x5b53, 0x015b},
    {0xff3a, 0x2264, 0x5c9a, 0x01be}, {0xff48, 0x20b3, 0x5dd0, 0x022a},
    {0xff56, 0x1f0b, 0x5ef5, 0x029f}, {0xff64, 0x1d6c, 0x6007, 0x031c},
    {0xff71, 0x1bd7, 0x6106, 0x03a4}, {0xff7e, 0x1a4c, 0x61f3, 0x0435},
    {0xff8a, 0x18cb, 0x62cb, 0x04d1}, {0xff96, 0x1756, 0x638f, 0x0577},
    {0xffa1, 0x15eb, 0x643f, 0x0628}, {0xffac, 0x148c, 0x64d9, 0x06e4},
    {0xffb6, 0x1338, 0x655e, 0x07ab}, {0xffbf, 0x11f0, 0x65cd, 0x087d},
    {0xffc8, 0x10b4, 0x6626, 0x095a}, {0xffd0, 0x0f83, 0x6669, 0x0a44},
    {0xffd8, 0x0e5f, 0x6696, 0x0b39}, {0xffdf, 0x0d46, 0x66ad, 0x0c39}
};

static inline int16_t clamp16(int32_t v) {
    if (v < -0x8000) {
        return -0x8000;
    } else if (v > 0x7fff) {
        return 0x7fff;
    }
    return (int16_t)v;
}

static inline int32_t clamp32(int64_t v) {
    if (v < -0x7fffffff - 1) {
        return -0x7fffffff - 1;
    } else if (v > 0x7fffffff) {
        return 0x7fffffff;
    }
    return (int32_t)v;
}

void aClearBufferImpl(uint16_t addr, int nbytes) {
    nbytes = ROUND_UP_16(nbytes);
    memset(rspa.buf.as_u8 + addr, 0, nbytes);
}

void aLoadBufferImpl(const void *source_addr) {
    memcpy(rspa.buf.as_u8 + rspa.in, source_addr, ROUND_UP_8(rspa.nbytes));
}

void aSaveBufferImpl(int16_t *dest_addr) {
    memcpy(dest_addr, rspa.buf.as_s16 + rspa.out / sizeof(int16_t), ROUND_UP_8(rspa.nbytes));
}

void aLoadADPCMImpl(int num_entries_times_16, const int16_t *book_source_addr) {
    memcpy(rspa.adpcm_table, book_source_addr, num_entries_times_16);
}

void aSetBufferImpl(uint8_t flags, uint16_t in, uint16_t out, uint16_t nbytes) {
    if (flags & A_AUX) {
        rspa.dry_right = in;
        rspa.wet_left = out;
        rspa.wet_right = nbytes;
    } else {
        rspa.in = in;
        rspa.out = out;
        rspa.nbytes = nbytes;
    }
}

void aSetVolumeImpl(uint8_t flags, int16_t v, int16_t t, int16_t r) {
    if (flags & A_AUX) {
        rspa.vol_dry = v;
        rspa.vol_wet = r;
    } else if (flags & A_VOL) {
        if (flags & A_LEFT) {
            rspa.vol[0] = v;
        } else {
            rspa.vol[1] = v;
        }
    } else {
        if (flags & A_LEFT) {
            rspa.target[0] = v;
            rspa.rate[0] = (int32_t)((uint16_t)t << 16 | ((uint16_t)r));
        } else {
            rspa.target[1] = v;
            rspa.rate[1] = (int32_t)((uint16_t)t << 16 | ((uint16_t)r));
        }
    }
}

// Non-3DS and 3DS-non-audio version
void aInterleaveImpl(uint16_t left, uint16_t right) {
    int count = ROUND_UP_16(rspa.nbytes) / sizeof(int16_t) / 8;
    int16_t *l = rspa.buf.as_s16 + left / sizeof(int16_t);
    int16_t *r = rspa.buf.as_s16 + right / sizeof(int16_t);
    int16_t *d = rspa.buf.as_s16 + rspa.out / sizeof(int16_t);
    while (count > 0) {
        int16_t l0 = *l++;
        int16_t l1 = *l++;
        int16_t l2 = *l++;
        int16_t l3 = *l++;
        int16_t l4 = *l++;
        int16_t l5 = *l++;
        int16_t l6 = *l++;
        int16_t l7 = *l++;
        int16_t r0 = *r++;
        int16_t r1 = *r++;
        int16_t r2 = *r++;
        int16_t r3 = *r++;
        int16_t r4 = *r++;
        int16_t r5 = *r++;
        int16_t r6 = *r++;
        int16_t r7 = *r++;
        *d++ = l0;
        *d++ = r0;
        *d++ = l1;
        *d++ = r1;
        *d++ = l2;
        *d++ = r2;
        *d++ = l3;
        *d++ = r3;
        *d++ = l4;
        *d++ = r4;
        *d++ = l5;
        *d++ = r5;
        *d++ = l6;
        *d++ = r6;
        *d++ = l7;
        *d++ = r7;
        --count;
    }
}

void aDMEMMoveImpl(uint16_t in_addr, uint16_t out_addr, int nbytes) {
    nbytes = ROUND_UP_16(nbytes);
    memmove(rspa.buf.as_u8 + out_addr, rspa.buf.as_u8 + in_addr, nbytes);
}

void aSetLoopImpl(ADPCM_STATE *adpcm_loop_state) {
    rspa.adpcm_loop_state = adpcm_loop_state;
}

// Decompresses ADPCM data
void aADPCMdecImpl(uint8_t flags, ADPCM_STATE state) {
    static const int8_t pos0_data[] = {-1, 0, -1, 0, -1, 1, -1, 1, -1, 2, -1, 2, -1, 3, -1, 3};
    static const int8_t pos1_data[] = {-1, 4, -1, 4, -1, 5, -1, 5, -1, 6, -1, 6, -1, 7, -1, 7};
    static const int16_t mult_data[] = {0x01, 0x10, 0x01, 0x10, 0x01, 0x10, 0x01, 0x10};
    static const int16_t table_prefix_data[] = {0, 0, 0, 0, 0, 0, 0, 1 << 11};
    const int8x16_t pos0 = vld1q_s8(pos0_data);
    const int8x16_t pos1 = vld1q_s8(pos1_data);
    const int16x8_t mult = vld1q_s16(mult_data);
    const int16x8_t mask = vdupq_n_s16((int16_t)0xf000);
    const int16x8_t table_prefix = vld1q_s16(table_prefix_data);
    
    uint8_t *in = rspa.buf.as_u8 + rspa.in;
    int16_t *out = rspa.buf.as_s16 + rspa.out / sizeof(int16_t);
    int nbytes = ROUND_UP_32(rspa.nbytes);
    if (flags & A_INIT) {
        memset(out, 0, 16 * sizeof(int16_t));
    } else if (flags & A_LOOP) {
        memcpy(out, rspa.adpcm_loop_state, 16 * sizeof(int16_t));
    } else {
        memcpy(out, state, 16 * sizeof(int16_t));
    }
    out += 16;
    int16x8_t result = vld1q_s16(out - 8);
    
    while (nbytes > 0) {
        int shift = *in >> 4; // should be in 0..12
        int table_index = *in++ & 0xf; // should be in 0..7
        int16_t (*tbl)[8] = rspa.adpcm_table[table_index];
        int i;

        int8x8_t inv = vld1_s8((int8_t *)in);
        int16x8_t tblvec[2] = {vld1q_s16(tbl[0]), vld1q_s16(tbl[1])};
        int16x8_t invec[2] = {vreinterpretq_s16_s8(vcombine_s8(vtbl1_s8(inv, vget_low_s8(pos0)),
                                                               vtbl1_s8(inv, vget_high_s8(pos0)))),
                              vreinterpretq_s16_s8(vcombine_s8(vtbl1_s8(inv, vget_low_s8(pos1)),
                                                               vtbl1_s8(inv, vget_high_s8(pos1))))};
        int16x8_t shiftcount = vdupq_n_s16(shift - 12); // negative means right shift
        int16x8_t tblvec1[8];

        in += 8;
        tblvec1[0] = vextq_s16(table_prefix, tblvec[1], 7);
        invec[0] = vmulq_s16(invec[0], mult);
        tblvec1[1] = vextq_s16(table_prefix, tblvec[1], 6);
        invec[1] = vmulq_s16(invec[1], mult);
        tblvec1[2] = vextq_s16(table_prefix, tblvec[1], 5);
        tblvec1[3] = vextq_s16(table_prefix, tblvec[1], 4);
        invec[0] = vandq_s16(invec[0], mask);
        tblvec1[4] = vextq_s16(table_prefix, tblvec[1], 3);
        invec[1] = vandq_s16(invec[1], mask);
        tblvec1[5] = vextq_s16(table_prefix, tblvec[1], 2);
        tblvec1[6] = vextq_s16(table_prefix, tblvec[1], 1);
        invec[0] = vqshlq_s16(invec[0], shiftcount);
        invec[1] = vqshlq_s16(invec[1], shiftcount);
        tblvec1[7] = table_prefix;
        for (i = 0; i < 2; i++) {
            int32x4_t acc0;
            int32x4_t acc1;

            acc1 = vmull_lane_s16(vget_high_s16(tblvec[0]), vget_high_s16(result), 2);
            acc1 = vmlal_lane_s16(acc1, vget_high_s16(tblvec[1]), vget_high_s16(result), 3);
            acc0 = vmull_lane_s16(vget_low_s16(tblvec[0]), vget_high_s16(result), 2);
            acc0 = vmlal_lane_s16(acc0, vget_low_s16(tblvec[1]), vget_high_s16(result), 3);

            acc0 = vmlal_lane_s16(acc0, vget_low_s16(tblvec1[0]), vget_low_s16(invec[i]), 0);
            acc0 = vmlal_lane_s16(acc0, vget_low_s16(tblvec1[1]), vget_low_s16(invec[i]), 1);
            acc0 = vmlal_lane_s16(acc0, vget_low_s16(tblvec1[2]), vget_low_s16(invec[i]), 2);
            acc0 = vmlal_lane_s16(acc0, vget_low_s16(tblvec1[3]), vget_low_s16(invec[i]), 3);

            acc1 = vmlal_lane_s16(acc1, vget_high_s16(tblvec1[0]), vget_low_s16(invec[i]), 0);
            acc1 = vmlal_lane_s16(acc1, vget_high_s16(tblvec1[1]), vget_low_s16(invec[i]), 1);
            acc1 = vmlal_lane_s16(acc1, vget_high_s16(tblvec1[2]), vget_low_s16(invec[i]), 2);
            acc1 = vmlal_lane_s16(acc1, vget_high_s16(tblvec1[3]), vget_low_s16(invec[i]), 3);
            acc1 = vmlal_lane_s16(acc1, vget_high_s16(tblvec1[4]), vget_high_s16(invec[i]), 0);
            acc1 = vmlal_lane_s16(acc1, vget_high_s16(tblvec1[5]), vget_high_s16(invec[i]), 1);
            acc1 = vmlal_lane_s16(acc1, vget_high_s16(tblvec1[6]), vget_high_s16(invec[i]), 2);
            acc1 = vmlal_lane_s16(acc1, vget_high_s16(tblvec1[7]), vget_high_s16(invec[i]), 3);

            result = vcombine_s16(vqshrn_n_s32(acc0, 11), vqshrn_n_s32(acc1, 11));
            vst1q_s16(out, result);
            out += 8;
        }

        nbytes -= 16 * sizeof(int16_t);
    }
    memcpy(state, out - 16, 16 * sizeof(int16_t));
}

void aResampleImpl(uint8_t flags, uint16_t pitch, RESAMPLE_STATE state) {
    int16_t tmp[16];
    int16_t *in_initial = rspa.buf.as_s16 + rspa.in / sizeof(int16_t);
    int16_t *in = in_initial;
    int16_t *out = rspa.buf.as_s16 + rspa.out / sizeof(int16_t);
    int nbytes = ROUND_UP_16(rspa.nbytes);
    uint32_t pitch_accumulator;
    int i;

    if (flags & A_INIT) {
        memset(tmp, 0, 5 * sizeof(int16_t));
    } else {
        memcpy(tmp, state, 16 * sizeof(int16_t));
    }
    if (flags & 2) {
        memcpy(in - 8, tmp + 8, 8 * sizeof(int16_t));
        in -= tmp[5] / sizeof(int16_t);
    }
    in -= 4;
    pitch_accumulator = (uint16_t)tmp[4];
    memcpy(in, tmp, 4 * sizeof(int16_t));

    static const uint16_t multiples_data[8] = {0, 2, 4, 6, 8, 10, 12, 14};
    uint16x8_t multiples = vld1q_u16(multiples_data);
    uint32x4_t pitchvec_8_steps = vdupq_n_u32((pitch << 1) * 8);
    uint32x4_t pitchacclo_vec = vdupq_n_u32((uint16_t)pitch_accumulator);
    uint32x4_t acc_a = vmlal_n_u16(pitchacclo_vec, vget_low_u16(multiples), pitch);
    uint32x4_t acc_b = vmlal_n_u16(pitchacclo_vec, vget_high_u16(multiples), pitch);

    do {
        uint16x8x2_t unzipped = vuzpq_u16(vreinterpretq_u16_u32(acc_a), vreinterpretq_u16_u32(acc_b));
        uint16x8_t tbl_positions = vshrq_n_u16(unzipped.val[0], 10);
        uint16x8_t in_positions = unzipped.val[1];
        int16x8_t tbl_entries[4];
        int16x8_t samples[4];
        int16x8x2_t unzipped1;
        int16x8x2_t unzipped2;

        tbl_entries[0] = vcombine_s16(vld1_s16(resample_table[vgetq_lane_u16(tbl_positions, 0)]), vld1_s16(resample_table[vgetq_lane_u16(tbl_positions, 1)]));
        tbl_entries[1] = vcombine_s16(vld1_s16(resample_table[vgetq_lane_u16(tbl_positions, 2)]), vld1_s16(resample_table[vgetq_lane_u16(tbl_positions, 3)]));
        tbl_entries[2] = vcombine_s16(vld1_s16(resample_table[vgetq_lane_u16(tbl_positions, 4)]), vld1_s16(resample_table[vgetq_lane_u16(tbl_positions, 5)]));
        tbl_entries[3] = vcombine_s16(vld1_s16(resample_table[vgetq_lane_u16(tbl_positions, 6)]), vld1_s16(resample_table[vgetq_lane_u16(tbl_positions, 7)]));
        samples[0] = vcombine_s16(vld1_s16(&in[vgetq_lane_u16(in_positions, 0)]), vld1_s16(&in[vgetq_lane_u16(in_positions, 1)]));
        samples[1] = vcombine_s16(vld1_s16(&in[vgetq_lane_u16(in_positions, 2)]), vld1_s16(&in[vgetq_lane_u16(in_positions, 3)]));
        samples[2] = vcombine_s16(vld1_s16(&in[vgetq_lane_u16(in_positions, 4)]), vld1_s16(&in[vgetq_lane_u16(in_positions, 5)]));
        samples[3] = vcombine_s16(vld1_s16(&in[vgetq_lane_u16(in_positions, 6)]), vld1_s16(&in[vgetq_lane_u16(in_positions, 7)]));
        samples[0] = vqrdmulhq_s16(samples[0], tbl_entries[0]);
        samples[1] = vqrdmulhq_s16(samples[1], tbl_entries[1]);
        samples[2] = vqrdmulhq_s16(samples[2], tbl_entries[2]);
        samples[3] = vqrdmulhq_s16(samples[3], tbl_entries[3]);

        unzipped1 = vuzpq_s16(samples[0], samples[1]);
        unzipped2 = vuzpq_s16(samples[2], samples[3]);
        samples[0] = vqaddq_s16(unzipped1.val[0], unzipped1.val[1]);
        samples[1] = vqaddq_s16(unzipped2.val[0], unzipped2.val[1]);
        unzipped1 = vuzpq_s16(samples[0], samples[1]);
        samples[0] = vqaddq_s16(unzipped1.val[0], unzipped1.val[1]);

        vst1q_s16(out, samples[0]);

        acc_a = vaddq_u32(acc_a, pitchvec_8_steps);
        acc_b = vaddq_u32(acc_b, pitchvec_8_steps);
        out += 8;
        nbytes -= 8 * sizeof(int16_t);
    } while (nbytes > 0);
    in += vgetq_lane_u16(vreinterpretq_u16_u32(acc_a), 1);
    pitch_accumulator = vgetq_lane_u16(vreinterpretq_u16_u32(acc_a), 0);

    state[4] = (int16_t)pitch_accumulator;
    memcpy(state, in, 4 * sizeof(int16_t));
    i = (in - in_initial + 4) & 7;
    in -= i;
    if (i != 0) {
        i = -8 - i;
    }
    state[5] = i;
    memcpy(state + 8, in, 8 * sizeof(int16_t));
}


void aEnvMixerImpl(uint8_t flags, ENVMIX_STATE state) {
    int16_t *in = rspa.buf.as_s16 + rspa.in / sizeof(int16_t);
    int16_t *dry[2] = {rspa.buf.as_s16 + rspa.out / sizeof(int16_t), rspa.buf.as_s16 + rspa.dry_right / sizeof(int16_t)};
    int16_t *wet[2] = {rspa.buf.as_s16 + rspa.wet_left / sizeof(int16_t), rspa.buf.as_s16 + rspa.wet_right / sizeof(int16_t)};
    int nbytes = ROUND_UP_16(rspa.nbytes);

    float32x4_t vols[2][2];
    int16_t dry_factor;
    int16_t wet_factor;
    float32x4_t target[2];
    float rate[2];
    int16x8_t in_loaded;
    int16x8_t vol_s16;
    bool increasing[2];

    int c;

    if (flags & A_INIT) {
        float vol_init[2] = {rspa.vol[0], rspa.vol[1]};
        float rate_float[2] = {(float)rspa.rate[0] * (1.0f / 65536.0f), (float)rspa.rate[1] * (1.0f / 65536.0f)};
        float step_diff[2] = {vol_init[0] * (rate_float[0] - 1.0f), vol_init[1] * (rate_float[1] - 1.0f)};
        static const float step_dividers_data[2][4] = {{1.0f / 8.0f, 2.0f / 8.0f, 3.0f / 8.0f, 4.0f / 8.0f},
                                                      {5.0f / 8.0f, 6.0f / 8.0f, 7.0f / 8.0f, 8.0f / 8.0f}};
        float32x4_t step_dividers[2] = {vld1q_f32(step_dividers_data[0]), vld1q_f32(step_dividers_data[1])};

        for (c = 0; c < 2; c++) {
            vols[c][0] = vaddq_f32(vdupq_n_f32(vol_init[c]), vmulq_n_f32(step_dividers[0], step_diff[c]));
            vols[c][1] = vaddq_f32(vdupq_n_f32(vol_init[c]), vmulq_n_f32(step_dividers[1], step_diff[c]));
            increasing[c] = rate_float[c] >= 1.0f;
            target[c] = vdupq_n_f32(rspa.target[c]);
            rate[c] = rate_float[c];
        }

        dry_factor = rspa.vol_dry;
        wet_factor = rspa.vol_wet;

        memcpy(state + 32, &rate_float[0], 4);
        memcpy(state + 34, &rate_float[1], 4);
        state[36] = rspa.target[0];
        state[37] = rspa.target[1];
        state[38] = rspa.vol_dry;
        state[39] = rspa.vol_wet;
    } else {
        vols[0][0] = vreinterpretq_f32_s16(vld1q_s16(state));
        vols[0][1] = vreinterpretq_f32_s16(vld1q_s16(state + 8));
        vols[1][0] = vreinterpretq_f32_s16(vld1q_s16(state + 16));
        vols[1][1] = vreinterpretq_f32_s16(vld1q_s16(state + 24));
        memcpy(&rate[0], state + 32, 4);
        memcpy(&rate[1], state + 34, 4);
        increasing[0] = rate[0] >= 1.0f;
        increasing[1] = rate[1] >= 1.0f;
        target[0] = vdupq_n_f32(state[36]);
        target[1] = vdupq_n_f32(state[37]);
        dry_factor = state[38];
        wet_factor = state[39];
    }

    do {
        in_loaded = vld1q_s16(in);
        in += 8;
        for (c = 0; c < 2; c++) {
            if (increasing[c]) {
                vols[c][0] = vminq_f32(vols[c][0], target[c]);
                vols[c][1] = vminq_f32(vols[c][1], target[c]);
            } else {
                vols[c][0] = vmaxq_f32(vols[c][0], target[c]);
                vols[c][1] = vmaxq_f32(vols[c][1], target[c]);
            }

            vol_s16 = vcombine_s16(vqmovn_s32(vcvtq_s32_f32(vols[c][0])), vqmovn_s32(vcvtq_s32_f32(vols[c][1])));
            vst1q_s16(dry[c], vqaddq_s16(vld1q_s16(dry[c]), vqrdmulhq_s16(in_loaded, vqrdmulhq_n_s16(vol_s16, dry_factor))));
            dry[c] += 8;
            if (flags & A_AUX) {
                vst1q_s16(wet[c], vqaddq_s16(vld1q_s16(wet[c]), vqrdmulhq_s16(in_loaded, vqrdmulhq_n_s16(vol_s16, wet_factor))));
                wet[c] += 8;
            }
            vols[c][0] = vmulq_n_f32(vols[c][0], rate[c]);
            vols[c][1] = vmulq_n_f32(vols[c][1], rate[c]);
        }

        nbytes -= 8 * sizeof(int16_t);
    } while (nbytes > 0);

    vst1q_s16(state, vreinterpretq_s16_f32(vols[0][0]));
    vst1q_s16(state + 8, vreinterpretq_s16_f32(vols[0][1]));
    vst1q_s16(state + 16, vreinterpretq_s16_f32(vols[1][0]));
    vst1q_s16(state + 24, vreinterpretq_s16_f32(vols[1][1]));
}

void aMixImpl(int16_t gain, uint16_t in_addr, uint16_t out_addr) {
    int nbytes = ROUND_UP_32(rspa.nbytes);
    int16_t *in = rspa.buf.as_s16 + in_addr / sizeof(int16_t);
    int16_t *out = rspa.buf.as_s16 + out_addr / sizeof(int16_t);

    while (nbytes > 0) {
        int16x8_t out1, out2, in1, in2;
        out1 = vld1q_s16(out);
        out2 = vld1q_s16(out + 8);
        in1 = vld1q_s16(in);
        in2 = vld1q_s16(in + 8);

        out1 = vqaddq_s16(out1, vqrdmulhq_n_s16(in1, gain));
        out2 = vqaddq_s16(out2, vqrdmulhq_n_s16(in2, gain));

        vst1q_s16(out, out1);
        vst1q_s16(out + 8, out2);

        out += 16;
        in += 16;

        nbytes -= 16 * sizeof(int16_t);
    }
}

#endif
