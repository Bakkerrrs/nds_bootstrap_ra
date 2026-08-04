#!/bin/sh
# Print the address of the RA snapshot buffer in each cardengine variant that
# contains the reader.
#
# The buffer lives in the cardengine's .bss, so its address moves whenever the
# code around it changes. Re-run this after every build and use the address for
# the variant your game actually loads -- for a plain retail DS game on a DSi or
# 3DS that is cardenginei_arm9.
#
# Then open the in-game menu's RAM viewer, navigate to that address, and look for
# the ASCII bytes "RA0S" (52 41 30 53) followed by a frame counter that climbs
# once per frame.

set -e
cd "$(dirname "$0")/.."

found=0
for map in $(find retail hb -name '*.map' 2>/dev/null | sort); do
	# Read the symbol itself: ra_reader.o also holds other statics in .bss, so the
	# start of its .bss contribution is not necessarily the snapshot.
	addr=$(awk '$2 == "raSnapshotBuffer" { print $1 }' "$map" | head -n1)
	[ -n "$addr" ] || continue
	size=$(awk '/^ \.bss +0x[0-9a-f]+ +0x[0-9a-f]+ ra_reader\.o$/ { print $3 }' "$map")
	printf '%-34s snapshot=%s size=%s\n' "$(basename "$map" .map)" "$addr" "$size"
	found=$((found + 1))
done

if [ "$found" -eq 0 ]; then
	echo "No .map files with ra_reader.o found -- build first, and check that" >&2
	echo "RA_READER_ENABLED is 1 in retail/common/include/ra.h." >&2
	exit 1
fi
