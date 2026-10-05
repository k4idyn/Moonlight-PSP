	.set noreorder

#include "pspstub.s"

	STUB_START "MediaEngine",0x40090000,0x00060005
	STUB_FUNC  0x284FAFFC,InitME
	STUB_FUNC  0xE6B12941,KillME
	STUB_FUNC  0xBF33B531,DisableMsLED
	STUB_FUNC  0x4D67F245,RequestModuleUnload
	STUB_FUNC  0x2FF97305,BootAvcMode
	STUB_FUNC  0xE0840133,AvcFixupModuleImports
	STUB_END
