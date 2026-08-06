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
