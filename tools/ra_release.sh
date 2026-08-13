#!/bin/sh
#
# Build the ROM that gets handed to a person, and refuse to produce one that is not what it claims.
#
# This exists because of a specific failure, and the failure is worth writing down because nothing in
# the build would have caught it. The pre-delivery checklist in docs/retroachievements.md says a ROM
# must compile at RA_LAUNCHER_WIFI=0 and =2. Running both `make`s satisfies that -- and leaves
# retail/bin/nds-bootstrap.nds holding whichever one ran *last*. Copying it after a `=0` build hands
# over a launcher with no network: no RetroAchievements ladder, nothing staged for the cardengine, and
# cardenginei_arm9_ra falling back to its built-in self-test, which fires one unlock with the synthetic
# id 0xF0000000 seconds into the game.
#
# That was mistaken for an intermittent memory fault for several days. The arena, the pending block's
# address and the launcher's statics were all blamed in turn, on evidence that was really "did the =2
# build happen to run last this time". The logs agreed with each other and not with the console,
# because a boot with no network never rewrites ra_wifi_launcher.log -- so the log being read was the
# previous, working boot's.
#
# So: the order is fixed here rather than remembered, and the result is verified against the binary
# rather than against intent. `=0` is built first because it is only a compile check; `=2` is built
# last because it is the deliverable.
#
set -e

cd "$(dirname "$0")/.."

OUT=${1:-nds-bootstrap-release.nds}

echo "checking RA_LAUNCHER_WIFI=0 still compiles"
make RA_LAUNCHER_WIFI=0 >/dev/null 2>&1

echo "building the deliverable at RA_LAUNCHER_WIFI=2"
make RA_LAUNCHER_WIFI=2 >/dev/null 2>&1

# The proof, taken from the ELF the ROM was built from. A =2 launcher links dsiwifi into its ARM7 and
# a =0 launcher does not, so this separates them without trusting which make ran last.
WIFI=$(arm-none-eabi-nm retail/arm7/nds-bootstrap.elf 2>/dev/null | grep -ci 'dsiwifi\|wifi_' || true)
if [ "$WIFI" -lt 20 ]; then
	echo "REFUSING: the ARM7 has $WIFI wifi symbols, so this is not a network build" >&2
	exit 1
fi

echo "host suite"
if ! bash tools/ra_reader_test.sh 2>&1 | grep -q "PASSED"; then
	echo "REFUSING: the host suite did not pass" >&2
	exit 1
fi

echo "snapshot address and free bytes"
bash tools/ra_snapshot_addr.sh

cp retail/bin/nds-bootstrap.nds "$OUT"
echo
echo "$OUT"
echo "  commit  $(git rev-parse --short HEAD)$(git diff --quiet || echo ' (WORKING TREE DIRTY)')"
echo "  wifi    $WIFI symbols -- RA_LAUNCHER_WIFI=2"
echo "  md5     $(md5sum "$OUT" | cut -d' ' -f1)"
