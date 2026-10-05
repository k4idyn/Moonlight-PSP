	.set noreorder
#include "pspstub.s"
	/* Keep the PRX function count aligned with the selected import variant. */
#if defined(PSP_AVC_LOW_DDR) && PSP_AVC_LOW_DDR
	STUB_START "AvcBootBridge",0x40010000,0x00020005
	STUB_FUNC 0x3F2EC244,AvcBootBridgeGetAvcLowDdr
#else
	STUB_START "AvcBootBridge",0x40010000,0x00010005
#endif
	STUB_FUNC 0x2FF97305,AvcBootBridgeBootAvcMode
	STUB_END
