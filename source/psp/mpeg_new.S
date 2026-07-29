	.set noreorder

#include "pspstub.s"

	/* Firmware 6.xx MPEG entry points.  Old PSPSDK NIDs return success from
	   create but leave direct AVC decoding unusable on current ARK firmware. */
	STUB_START "sceMpeg",0x40090000,0x00020005
	STUB_FUNC 0x42C679F6,sceMpegQueryMemSizeUtubbu
	STUB_FUNC 0x75E21135,sceMpegCreateUtubbu
	STUB_END
