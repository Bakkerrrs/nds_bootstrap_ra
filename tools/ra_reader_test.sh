#!/bin/sh
# Build and run the host-side test for the RA reader's watchlist and chain walker.
#
# Needs only a host compiler -- no devkitARM, no hardware. See the comment at the
# top of ra_reader_test.c for what it covers and why it is worth having.
#
# The two link options are what make it a real test:
#
#   -no-pie -Wl,-Ttext-segment=0x02100000
#       puts the test's globals, including the reader's own snapshot buffer, inside
#       the 0x02000000-0x03000000 range the reader validates addresses against. The
#       chain self-tests resolve for the same reason they do on hardware rather than
#       because a check was relaxed.
#
# The I/O page at 0x04000000 is mapped by the test itself.
#
# The two -Wno-*-cast flags are for the u32/pointer round trips the reader does on
# purpose: on the DS a u32 *is* an address. They are lossless here only because of the
# link address above, which is why that is not optional.

set -e
cd "$(dirname "$0")/.."

CC=${CC:-cc}
out=$(mktemp -d)
trap 'rm -rf "$out"' EXIT

mkdir -p "$out/include/nds"
cat > "$out/include/nds/ndstypes.h" <<'EOF'
/* Just enough of libnds' ndstypes.h for the reader to compile on the host. */
#ifndef NDSTYPES_H
#define NDSTYPES_H
#include <stdint.h>
#include <stdbool.h>
typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef volatile uint8_t  vu8;
typedef volatile uint16_t vu16;
typedef volatile uint32_t vu32;
#endif
EOF

$CC -std=gnu99 -Wall -Wno-unused-function -Wno-pointer-to-int-cast -Wno-int-to-pointer-cast -O1 \
	-I"$out/include" -Iretail/common/include \
	-no-pie -Wl,-Ttext-segment=0x02100000 \
	tools/ra_reader_test.c -o "$out/ra_reader_test"

"$out/ra_reader_test"
