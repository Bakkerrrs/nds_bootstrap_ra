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
typedef int8_t   s8;
typedef int16_t  s16;
typedef int32_t  s32;
typedef volatile uint8_t  vu8;
typedef volatile uint16_t vu16;
typedef volatile uint32_t vu32;
#endif
EOF

# rcheevos is compiled in rather than stubbed out, for the same reason cardengine.c is:
# the expensive failures here are a definition that does not parse and an address that
# translates wrongly, and both are pure logic that a host can check. Catching either one
# here costs seconds; catching it on hardware costs a flash cycle and a photograph of a
# hex viewer.
#
# The file list mirrors the whitelist in retail/cardenginei/arm9_ra/Makefile -- the
# runtime, not the whole library. -lm is for fmod(), which rcheevos' modulus operator
# references whether or not any definition uses it.
RC=retail/cardenginei/arm9_ra/rcheevos
if [ ! -f "$RC/include/rc_runtime.h" ]; then
	echo "$RC is empty -- run: git submodule update --init" >&2
	exit 2
fi

rc_runtime_sources=$(ls "$RC"/src/rcheevos/*.c | grep -v 'rc_validate\.c$')

$CC -std=gnu99 -Wall -Wno-unused-function -Wno-pointer-to-int-cast -Wno-int-to-pointer-cast -O1 \
	-I"$out/include" -Iretail/common/include \
	-I"$RC/include" -I"$RC/src" \
	-no-pie -Wl,-Ttext-segment=0x02100000 \
	tools/ra_reader_test.c \
	$rc_runtime_sources \
	"$RC"/src/rc_util.c "$RC"/src/rc_compat.c "$RC"/src/rc_version.c \
	"$RC"/src/rhash/md5.c \
	-lm -o "$out/ra_reader_test"

set +e
"$out/ra_reader_test"
status=$?
set -e

# The other half of pinning step two's log classifier.
#
# ra_wifi_verdict.c decides how the Atheros chip arrived by matching dsiwifi's printf text,
# because the driver exposes none of those facts any other way. The C test above proves it
# reads the log a real console produced; this proves those strings are still the ones the
# submodule prints. Between them, bumping libs/dsiwifi fails here in seconds instead of
# reporting the wrong world after a play session.
#
# Skipped rather than failed when the submodule is absent: dsiwifi is only needed for the
# step-two diagnostic build, so a clone without it is a normal state to be in, unlike a
# clone without rcheevos.
DSIWIFI=libs/dsiwifi
if [ -f "$DSIWIFI/arm_iop/source/wifi_card.twl.c" ]; then
	echo
	echo "dsiwifi still prints what the classifier looks for"
	for say in \
		'Mfg %08lx' \
		'needs firmware upload' \
		'BMI version:' \
		'Launching!' \
		'ready, handshaking' \
		'fully initialized!' \
		'bad mbox alloc'
	do
		if grep -qF -- "$say" "$DSIWIFI/arm_iop/source/wifi_card.twl.c"; then
			echo "  ok    $say"
		else
			echo "  FAIL  $say -- libs/dsiwifi does not print this any more"
			status=1
		fi
	done
else
	echo
	echo "libs/dsiwifi absent -- skipping the classifier's string pins"
	echo "  (git submodule update --init, if you are building RA_LAUNCHER_WIFI=1)"
fi

exit $status
