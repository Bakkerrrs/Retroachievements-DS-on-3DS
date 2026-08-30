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
/*
    And the channel mode 2 uses to ask for the radio back. FIFO_USER_07 because 01 through 06 are
    all spoken for by the launcher's own handshake and by nds-bootstrap; a channel with two owners
    is the bug this project already paid for once with FIFO_DSWIFI.
*/
#define RA_WIFI_ARM7_STOP_CHANNEL  FIFO_USER_07

/*
    dsiwifi's own teardown, declared rather than included: wifi_card.h lives in its arm_iop/source
    directory and is not on the exported include path. dsiwifi's own test_app declares it exactly
    this way, which is the closest thing to a blessing available.

    It masks the SDIO card IRQ, disables IRQ_WIFI_SDIO_CARDIRQ on the AUX controller, disables the
    ARM7's TIMER3, and writes zero to the chip's F1_INT_STATUS_ENABLE and CCCR irq_enable. That is
    every path by which the chip could interrupt this CPU after the bootloader has replaced its
    code, which is the whole requirement.

    What it does *not* do is power the chip down -- dsiwifi has no path for that -- so the radio is
    left associated to the access point with its interrupts masked. Stated rather than glossed:
    nothing will poke this CPU, and nothing is reading or writing memory, but the chip is still on.
*/
extern void wifi_card_deinit(void);

static volatile bool raWifiStopAsked;

/*
    Asked from an interrupt, done from the idle loop. wifi_card_deinit() writes SDIO registers and
    waits for the controller to answer -- bounded, but not a wait to take inside a FIFO handler on
    a CPU whose code is about to be overwritten.
*/
static void raWifiStopHandler(u32 value, void* userdata) {
	(void)value;
	(void)userdata;
	raWifiStopAsked = true;
}

void raWifiInstall(void) {
	installWifiFIFO();

	fifoSetValue32Handler(RA_WIFI_ARM7_STOP_CHANNEL, raWifiStopHandler, 0);

	/*
	    Told rather than assumed. The ARM9 waits a bounded time for this and reports its
	    absence as "rebuild both CPUs", which is the mistake a bare timeout would otherwise
	    blame on the hardware.
	*/
	fifoSendValue32(RA_WIFI_ARM7_READY_CHANNEL, 1);
}

void raWifiPoll(void) {
	if (!raWifiStopAsked) {
		return;
	}
	raWifiStopAsked = false;

	wifi_card_deinit();

	/*
	    The acknowledgement is the point of the round trip. Without it the ARM9 would have to
	    guess, and the thing it would be guessing about is whether an ARM7 that is still taking
	    SDIO interrupts is about to have its code replaced.
	*/
	fifoSendValue32(RA_WIFI_ARM7_STOP_CHANNEL, 1);
}

#endif /* RA_LAUNCHER_WIFI */
