/*
    The ARM7 half of step two: hand nds-bootstrap's ARM7 to dsiwifi.

    One call, and that is the point. `installWifiFIFO()` allocates two 1.5K mailbox buffers,
    reads the firmware's WiFi slots, initialises NDMA and the SDIO controller at 0x04004A00,
    and installs a handler on FIFO_DSWIFI. Nothing touches the chip until the ARM9 asks --
    dsiwifi's own logging is off until then too -- so this costs a booting console nothing it
    would notice, and the whole bring-up stays a thing the ARM9 starts deliberately.

    Where it goes in main() matters more than what it does. It is installed *after* the
    launcher's own FIFO handshake has completed, so that handshake is bit-for-bit what it is
    today: SCFGFifoCheck() has run, FIFO_USER_05 has been answered, and the ARM9 is free to
    proceed. Installing earlier would have let the probe's own IPC message satisfy the
    swiIntrWait() the handshake waits on, and the ARM7 would then have run SCFGFifoCheck()
    before the ARM9 had sent FIFO_USER_06 -- silently dropping the CPU-clock request. The
    probe build never boots a game, so that would not have been visible; it would just have
    been wrong.

    Nothing here is conditional on the console at run time. installWifiFIFO() writes only
    the WiFi SDIO block and NDMA, both of which are extended TWL I/O: on a DS, or in DS mode
    with SCFG closed, those addresses are not there and the reads come back as they do for
    anything unmapped. The build is a diagnostic aimed at a 3DS in DSi mode -- see
    RA_LAUNCHER_WIFI -- and gating it on isDSiMode() would only hide the case where SCFG is
    closed, which is precisely the case the log exists to report.

    This file is part of nds-bootstrap and is licensed under the GPL-3.0,
    the same terms as the rest of the project.
*/

#include "ra_wifi.h"

#if RA_LAUNCHER_WIFI

#include <nds/fifocommon.h>

#include "dsiwifi7.h"

/* Must match the ARM9's RA_WIFI_ARM7_READY_CHANNEL. */
#define RA_WIFI_ARM7_READY_CHANNEL FIFO_USER_04

void raWifiInstall(void) {
	installWifiFIFO();

	/*
	    Told rather than assumed. The ARM9 waits a bounded time for this and reports its
	    absence as "rebuild both CPUs", which is the mistake a bare timeout would otherwise
	    blame on the hardware.
	*/
	fifoSendValue32(RA_WIFI_ARM7_READY_CHANNEL, 1);
}

#endif /* RA_LAUNCHER_WIFI */
