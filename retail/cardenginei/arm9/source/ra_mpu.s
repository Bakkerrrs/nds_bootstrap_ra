@---------------------------------------------------------------------------------
@ RetroAchievements support for nds-bootstrap -- MPU inspection.
@
@ The cardengine can reach the ROM cache with the CPU but faults one byte past it,
@ while DMA round-trips through that same address fine -- so the memory is real
@ and the MPU is what blocks it. Claiming a region the game does not use is the
@ clean way in, which first means knowing which are in use.
@
@ ARM code because MRC has no Thumb encoding, and the cardengine builds as Thumb.
@
@ This file is part of nds-bootstrap and is licensed under the GPL-3.0,
@ the same terms as the rest of the project.
@---------------------------------------------------------------------------------
#include <nds/asminc.h>

	.arm
@---------------------------------------------------------------------------------
BEGIN_ASM_FUNC ra_mpu_read_regions
@ void ra_mpu_read_regions(u32 out[8]);
@ Each word is a protection region register: base in bits 31..12, size in bits
@ 5..1, enable in bit 0. A word with bit 0 clear is a region going spare.
@---------------------------------------------------------------------------------
	mrc	p15, 0, r1, c6, c0, 0
	str	r1, [r0, #0]
	mrc	p15, 0, r1, c6, c1, 0
	str	r1, [r0, #4]
	mrc	p15, 0, r1, c6, c2, 0
	str	r1, [r0, #8]
	mrc	p15, 0, r1, c6, c3, 0
	str	r1, [r0, #12]
	mrc	p15, 0, r1, c6, c4, 0
	str	r1, [r0, #16]
	mrc	p15, 0, r1, c6, c5, 0
	str	r1, [r0, #20]
	mrc	p15, 0, r1, c6, c6, 0
	str	r1, [r0, #24]
	mrc	p15, 0, r1, c6, c7, 0
	str	r1, [r0, #28]
	bx	lr

@---------------------------------------------------------------------------------
BEGIN_ASM_FUNC ra_mpu_read_perms
@ void ra_mpu_read_perms(u32 out[4]);
@ [0] extended data access permission   (4 bits per region)
@ [1] extended instruction access permission
@ [2] data cacheable bits               (1 bit per region)
@ [3] write buffer control
@
@ The regions say which addresses are covered; these say what may be done with
@ them. A region that permits reads but not writes explains a store faulting where
@ loads succeed.
@---------------------------------------------------------------------------------
	mrc	p15, 0, r1, c5, c0, 2
	str	r1, [r0, #0]
	mrc	p15, 0, r1, c5, c0, 3
	str	r1, [r0, #4]
	mrc	p15, 0, r1, c2, c0, 0
	str	r1, [r0, #8]
	mrc	p15, 0, r1, c3, c0, 0
	str	r1, [r0, #12]
	bx	lr
