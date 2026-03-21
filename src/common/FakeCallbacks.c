#include "Limelight-internal.h"

static int fakeDrSetup(int videoFormat, int width, int height, int redrawRate, void* context, int drFlags) {
    (void)videoFormat; (void)width; (void)height; (void)redrawRate; (void)context; (void)drFlags;
    return 0;
}
static void fakeDrStart(void) {}
static void fakeDrStop(void) {}
static void fakeDrCleanup(void) {}
static int fakeDrSubmitDecodeUnit(PDECODE_UNIT decodeUnit) {
    (void)decodeUnit;
    return DR_OK;
}

static DECODER_RENDERER_CALLBACKS fakeDrCallbacks = {
    .setup = fakeDrSetup,
    .start = fakeDrStart,
    .stop = fakeDrStop,
    .cleanup = fakeDrCleanup,
    .submitDecodeUnit = fakeDrSubmitDecodeUnit,
};

static int fakeArInit(int audioConfiguration, POPUS_MULTISTREAM_CONFIGURATION opusConfig, void* context, int arFlags) {
    (void)audioConfiguration; (void)opusConfig; (void)context; (void)arFlags;
    return 0;
}
static void fakeArStart(void) {}
static void fakeArStop(void) {}
static void fakeArCleanup(void) {}
static void fakeArDecodeAndPlaySample(char* sampleData, int sampleLength) {
    (void)sampleData; (void)sampleLength;
}

AUDIO_RENDERER_CALLBACKS fakeArCallbacks = {
    .init = fakeArInit,
    .start = fakeArStart,
    .stop = fakeArStop,
    .cleanup = fakeArCleanup,
    .decodeAndPlaySample = fakeArDecodeAndPlaySample,
};

static void fakeClStageStarting(int stage) { (void)stage; }
static void fakeClStageComplete(int stage) { (void)stage; }
static void fakeClStageFailed(int stage, int errorCode) { (void)stage; (void)errorCode; }
static void fakeClConnectionStarted(void) {}
static void fakeClConnectionTerminated(int errorCode) { (void)errorCode; }
static void fakeClLogMessage(const char* format, ...) { (void)format; }
static void fakeClRumble(unsigned short controllerNumber, unsigned short lowFreqMotor, unsigned short highFreqMotor) {
    (void)controllerNumber; (void)lowFreqMotor; (void)highFreqMotor;
}
static void fakeClConnectionStatusUpdate(int connectionStatus) { (void)connectionStatus; }
static void fakeClSetHdrMode(bool enabled) { (void)enabled; }
static void fakeClRumbleTriggers(uint16_t controllerNumber, uint16_t leftTriggerMotor, uint16_t rightTriggerMotor) {
    (void)controllerNumber; (void)leftTriggerMotor; (void)rightTriggerMotor;
}
static void fakeClSetMotionEventState(uint16_t controllerNumber, uint8_t motionType, uint16_t reportRateHz) {
    (void)controllerNumber; (void)motionType; (void)reportRateHz;
}
static void fakeClSetAdaptiveTriggers(uint16_t controllerNumber, uint8_t eventFlags, uint8_t typeLeft, uint8_t typeRight, uint8_t *left, uint8_t *right) {
    (void)controllerNumber; (void)eventFlags; (void)typeLeft; (void)typeRight; (void)left; (void)right;
};
static void fakeClSetControllerLED(uint16_t controllerNumber, uint8_t r, uint8_t g, uint8_t b) {
    (void)controllerNumber; (void)r; (void)g; (void)b;
}

static CONNECTION_LISTENER_CALLBACKS fakeClCallbacks = {
    .stageStarting = fakeClStageStarting,
    .stageComplete = fakeClStageComplete,
    .stageFailed = fakeClStageFailed,
    .connectionStarted = fakeClConnectionStarted,
    .connectionTerminated = fakeClConnectionTerminated,
    .logMessage = fakeClLogMessage,
    .rumble = fakeClRumble,
    .connectionStatusUpdate = fakeClConnectionStatusUpdate,
    .setHdrMode = fakeClSetHdrMode,
    .rumbleTriggers = fakeClRumbleTriggers,
    .setMotionEventState = fakeClSetMotionEventState,
    .setControllerLED = fakeClSetControllerLED,
    .setAdaptiveTriggers = fakeClSetAdaptiveTriggers,
};

void fixupMissingCallbacks(PDECODER_RENDERER_CALLBACKS* drCallbacks, PAUDIO_RENDERER_CALLBACKS* arCallbacks,
    PCONNECTION_LISTENER_CALLBACKS* clCallbacks)
{
    if (*drCallbacks == NULL) {
        *drCallbacks = &fakeDrCallbacks;
    }
    else {
        if ((*drCallbacks)->setup == NULL) {
            (*drCallbacks)->setup = fakeDrSetup;
        }
        if ((*drCallbacks)->start == NULL) {
            (*drCallbacks)->start = fakeDrStart;
        }
        if ((*drCallbacks)->stop == NULL) {
            (*drCallbacks)->stop = fakeDrStop;
        }
        if ((*drCallbacks)->cleanup == NULL) {
            (*drCallbacks)->cleanup = fakeDrCleanup;
        }
        if ((*drCallbacks)->submitDecodeUnit == NULL) {
            (*drCallbacks)->submitDecodeUnit = fakeDrSubmitDecodeUnit;
        }
    }

    if (*arCallbacks == NULL) {
        *arCallbacks = &fakeArCallbacks;
    }
    else {
        if ((*arCallbacks)->init == NULL) {
            (*arCallbacks)->init = fakeArInit;
        }
        if ((*arCallbacks)->start == NULL) {
            (*arCallbacks)->start = fakeArStart;
        }
        if ((*arCallbacks)->stop == NULL) {
            (*arCallbacks)->stop = fakeArStop;
        }
        if ((*arCallbacks)->cleanup == NULL) {
            (*arCallbacks)->cleanup = fakeArCleanup;
        }
        if ((*arCallbacks)->decodeAndPlaySample == NULL) {
            (*arCallbacks)->decodeAndPlaySample = fakeArDecodeAndPlaySample;
        }
    }

    if (*clCallbacks == NULL) {
        *clCallbacks = &fakeClCallbacks;
    }
    else {
        if ((*clCallbacks)->stageStarting == NULL) {
            (*clCallbacks)->stageStarting = fakeClStageStarting;
        }
        if ((*clCallbacks)->stageComplete == NULL) {
            (*clCallbacks)->stageComplete = fakeClStageComplete;
        }
        if ((*clCallbacks)->stageFailed == NULL) {
            (*clCallbacks)->stageFailed = fakeClStageFailed;
        }
        if ((*clCallbacks)->connectionStarted == NULL) {
            (*clCallbacks)->connectionStarted = fakeClConnectionStarted;
        }
        if ((*clCallbacks)->connectionTerminated == NULL) {
            (*clCallbacks)->connectionTerminated = fakeClConnectionTerminated;
        }
        if ((*clCallbacks)->logMessage == NULL) {
            (*clCallbacks)->logMessage = fakeClLogMessage;
        }
        if ((*clCallbacks)->rumble == NULL) {
            (*clCallbacks)->rumble = fakeClRumble;
        }
        if ((*clCallbacks)->connectionStatusUpdate == NULL) {
            (*clCallbacks)->connectionStatusUpdate = fakeClConnectionStatusUpdate;
        }
        if ((*clCallbacks)->setHdrMode == NULL) {
            (*clCallbacks)->setHdrMode = fakeClSetHdrMode;
        }
        if ((*clCallbacks)->rumbleTriggers == NULL) {
            (*clCallbacks)->rumbleTriggers = fakeClRumbleTriggers;
        }
        if ((*clCallbacks)->setMotionEventState == NULL) {
            (*clCallbacks)->setMotionEventState = fakeClSetMotionEventState;
        }
        if ((*clCallbacks)->setControllerLED == NULL) {
            (*clCallbacks)->setControllerLED = fakeClSetControllerLED;
        }
        if ((*clCallbacks)->setAdaptiveTriggers == NULL) {
            (*clCallbacks)->setAdaptiveTriggers = fakeClSetAdaptiveTriggers;
        }
    }
}
