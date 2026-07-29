	.set noreorder

#include "pspimport.s"

	IMPORT_START "sceMpeg",0x00090000
	IMPORT_FUNC "sceMpeg",0x11F95CF1,sceMpegGetAvcNalAu
