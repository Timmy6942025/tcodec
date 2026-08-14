#!/usr/bin/env bash
# Scalar-vs-NEON parity check (run from repo root).
# Builds the codec twice (NEON on/off), then verifies:
#   1. All four residual transforms produce bit-identical output
#   2. End-to-end bitstreams (CQP, entropy on/off) are byte-identical
set -euo pipefail
cd "$(dirname "$0")/.."

CC="${CC:-cc}"
CF="-O2 -march=armv8-a -Iinclude -std=c11 -D_GNU_SOURCE"

# 1) scalar-transform parity program
cat > /tmp/tc_parity.c << 'PEOF'
#include <stdio.h>
#include "tcodec_common.h"
int main(void) {
    tc_coeff_t in[64], fwd[64], rec[64];
    unsigned long long rs=123456789;
    for (int rep=0; rep<256; rep++) {
        for (int i=0;i<64;i++){ rs=rs*6364136223846793005ULL+1442695040888963407ULL;
                                in[i]=(tc_coeff_t)((int)((rs>>33)%4001)-2000); }
        tc_fwht4x4(in,4,fwd);    tc_iwht4x4(fwd,rec,4);    fwrite(rec,sizeof(rec[0]),16,stdout);
        tc_fdct4x4_res(in,4,fwd); tc_idct4x4_res(fwd,rec,4); fwrite(rec,sizeof(rec[0]),16,stdout);
        tc_fwht8x8(in,8,fwd);    tc_iwht8x8(fwd,rec,8);    fwrite(rec,sizeof(rec[0]),64,stdout);
        tc_fdct8x8_res(in,8,fwd); tc_idct8x8_res(fwd,rec,8); fwrite(rec,sizeof(rec[0]),64,stdout);
    }
    return 0;
}
PEOF

SCALAR_DIR=/tmp/tc_parity_scalar_lib
rm -rf "$SCALAR_DIR"; mkdir -p "$SCALAR_DIR"
for f in src/*.c; do
  $CC $CF -U__aarch64__ -U__ARM_NEON -U__ARM_NEON__ -c "$f" -o "$SCALAR_DIR/$(basename "${f%.c}").o" 2>/dev/null
done
ar rcs "$SCALAR_DIR/libtcodec_scalar.a" "$SCALAR_DIR"/*.o

$CC $CF -o /tmp/tc_parity_neon  /tmp/tc_parity.c build/libtcodec.a -lpthread -lm
$CC $CF -U__aarch64__ -U__ARM_NEON -U__ARM_NEON__ -o /tmp/tc_parity_scalar_bin /tmp/tc_parity.c "$SCALAR_DIR/libtcodec_scalar.a" -lpthread -lm
/tmp/tc_parity_neon  > /tmp/parity_neon.bin
/tmp/tc_parity_scalar_bin > /tmp/parity_scalar.bin
cmp -s /tmp/parity_neon.bin /tmp/parity_scalar.bin || { echo "PARITY FAIL: transforms differ"; exit 1; }
echo "OK: transform scalar/NEON bit-identical"

# 1a) pixel-mode IDCT parity (the v2 decoder does not normally use this
# entry point, but it is part of the public transform contract).
cat > /tmp/tc_pixel_idct_parity.c << 'PEOF'
#include <stdio.h>
#include <stdint.h>
#include "tcodec_common.h"
int main(void) {
    tc_coeff_t a4[16], a8[64];
    tc_pixel_t o4[64], o8[128];
    uint32_t seed = 1;
    for (int rep = 0; rep < 1000; rep++) {
        for (int i = 0; i < 16; i++) { seed = seed * 1664525u + 1013904223u; a4[i] = (tc_coeff_t)(seed >> 16); }
        for (int i = 0; i < 64; i++) { seed = seed * 1664525u + 1013904223u; a8[i] = (tc_coeff_t)(seed >> 16); }
        memset(o4, 0, sizeof(o4)); memset(o8, 0, sizeof(o8));
        tc_idct4x4(a4, o4, 8); tc_idct8x8(a8, o8, 16);
        fwrite(o4, 1, sizeof(o4), stdout); fwrite(o8, 1, sizeof(o8), stdout);
    }
    return 0;
}
PEOF
$CC $CF -o /tmp/tc_pixel_idct_neon /tmp/tc_pixel_idct_parity.c build/libtcodec.a -lpthread -lm
$CC $CF -U__aarch64__ -U__ARM_NEON -U__ARM_NEON__ -o /tmp/tc_pixel_idct_scalar /tmp/tc_pixel_idct_parity.c "$SCALAR_DIR/libtcodec_scalar.a" -lpthread -lm
/tmp/tc_pixel_idct_neon > /tmp/pixel_idct_neon.bin
/tmp/tc_pixel_idct_scalar > /tmp/pixel_idct_scalar.bin
cmp -s /tmp/pixel_idct_neon.bin /tmp/pixel_idct_scalar.bin || { echo "PARITY FAIL: pixel IDCT differs"; exit 1; }
echo "OK: pixel IDCT scalar/NEON bit-identical"

# 1b) deblock parity, including mixed weak/strong edges and non-trivial strides
cat > /tmp/tc_deblock_parity.c << 'PEOF'
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "tcodec_common.h"
int main(void) {
    enum { W = 80, H = 80, S = 96, CW = 40, CH = 40, CS = 48 };
    static uint8_t y[S * H], cb[CS * CH], cr[CS * CH];
    unsigned seed = 0x13579bdfu;
    for (int rep = 0; rep < 128; rep++) {
        for (int i = 0; i < S * H; i++) {
            seed = seed * 1664525u + 1013904223u;
            y[i] = (uint8_t)(seed >> 24);
        }
        for (int i = 0; i < CS * CH; i++) {
            seed = seed * 1664525u + 1013904223u;
            cb[i] = (uint8_t)(seed >> 24);
            seed = seed * 1664525u + 1013904223u;
            cr[i] = (uint8_t)(seed >> 24);
        }
        tc_deblock_ctu(y, S, cb, CS, cr, CS, 8, 8, 8 + (rep % 56));
        fwrite(y, 1, sizeof(y), stdout);
        fwrite(cb, 1, sizeof(cb), stdout);
        fwrite(cr, 1, sizeof(cr), stdout);
    }
    return 0;
}
PEOF
$CC $CF -o /tmp/tc_deblock_neon /tmp/tc_deblock_parity.c build/libtcodec.a -lpthread -lm
$CC $CF -U__aarch64__ -U__ARM_NEON -U__ARM_NEON__ -o /tmp/tc_deblock_scalar /tmp/tc_deblock_parity.c "$SCALAR_DIR/libtcodec_scalar.a" -lpthread -lm
/tmp/tc_deblock_neon > /tmp/deblock_neon.bin
/tmp/tc_deblock_scalar > /tmp/deblock_scalar.bin
cmp -s /tmp/deblock_neon.bin /tmp/deblock_scalar.bin || { echo "PARITY FAIL: deblock differs"; exit 1; }
echo "OK: deblock scalar/NEON bit-identical"

# 1c) Motion compensation parity for all safe production block sizes and
# fractional modes used by the NEON dispatch (luma/chroma).
cat > /tmp/tc_motion_parity.c << 'PEOF'
#include <stdio.h>
#include <stdint.h>
#include "tcodec_common.h"
int main(void) {
    static tc_pixel_t y[160 * 160], c[80 * 80], out[64 * 64];
    uint32_t seed = 7;
    for (size_t i = 0; i < sizeof(y); i++) { seed = seed * 1664525u + 1013904223u; y[i] = (uint8_t)(seed >> 24); }
    for (size_t i = 0; i < sizeof(c); i++) { seed = seed * 1664525u + 1013904223u; c[i] = (uint8_t)(seed >> 24); }
    for (int n = 8; n <= 64; n *= 2) for (int rep = 0; rep < 200; rep++) {
        tc_mv_s mv = { (12 + rep % 20) * 4 + rep % 4, (12 + (rep * 3) % 20) * 4 + rep % 4 };
        tc_inter_predict(y, 160, 160, 160, mv, out, n, n);
        fwrite(out, 1, (size_t)n * n, stdout);
    }
    for (int n = 4; n <= 32; n *= 2) for (int rep = 0; rep < 200; rep++) {
        tc_mv_s mv = { (12 + rep % 20) * 4 + rep % 8, (12 + (rep * 3) % 20) * 4 + rep % 8 };
        tc_inter_predict_chroma(c, 80, 80, 80, mv, out, n, n);
        fwrite(out, 1, (size_t)n * n, stdout);
    }
    return 0;
}
PEOF
$CC $CF -o /tmp/tc_motion_neon /tmp/tc_motion_parity.c build/libtcodec.a -lpthread -lm
$CC $CF -U__aarch64__ -U__ARM_NEON -U__ARM_NEON__ -o /tmp/tc_motion_scalar /tmp/tc_motion_parity.c "$SCALAR_DIR/libtcodec_scalar.a" -lpthread -lm
/tmp/tc_motion_neon > /tmp/motion_neon.bin
/tmp/tc_motion_scalar > /tmp/motion_scalar.bin
cmp -s /tmp/motion_neon.bin /tmp/motion_scalar.bin || { echo "PARITY FAIL: motion differs"; exit 1; }
echo "OK: motion scalar/NEON bit-identical"

# 2) end-to-end encode parity
python3 - << 'PEOF'
w,h,n=128,240,6
f=open('/tmp/parity_src.yuv','wb')
for fr in range(n):
    for y in range(h):
        f.write(bytes([(x*255//w + y*255//h + fr*17) % 256 for x in range(w)]))
    for y in range(h//2):
        f.write(bytes([128])*(w//2))
    for y in range(h//2):
        f.write(bytes([128])*(w//2))
f.close()
PEOF

$CC $CF -U__aarch64__ -U__ARM_NEON -U__ARM_NEON__ -o /tmp/tcenc_scalar tools/tcenc.c "$SCALAR_DIR/libtcodec_scalar.a" -lpthread -lm
for mode in "" "--entropy"; do
  ./build/tcenc -w 128 -h 224 -q 30 -n 6 $mode -o /tmp/parity_neon.tcv /tmp/parity_src.yuv >/dev/null 2>&1
  /tmp/tcenc_scalar -w 128 -h 224 -q 30 -n 6 $mode -o /tmp/parity_scalar.tcv /tmp/parity_src.yuv >/dev/null 2>&1
  cmp -s /tmp/parity_neon.tcv /tmp/parity_scalar.tcv || { echo "PARITY FAIL: mode='$mode' bitstreams differ"; exit 1; }
  echo "OK: e2e bitstream identical (mode='$mode')"
done
echo "SCALAR/NEON PARITY: ALL OK"
