#ifndef MOONLIGHT_AVC_FRAME_SLOTS_H
#define MOONLIGHT_AVC_FRAME_SLOTS_H
/* Metadata only. Caller must hold one shared lock around every operation.
 * Pixel allocation and cache handling belong to the hardware adapter.
 * Six slots allow three decode outputs, one queued and two presenter-held
 * frames (pacing can retain a frame separately from an active GU read).
 * No slot may be reused until its owner explicitly releases it. */
#define AVC_FRAME_SLOTS 6
enum { AVC_SLOT_FREE, AVC_SLOT_WRITING, AVC_SLOT_QUEUED, AVC_SLOT_HELD };
typedef struct {
    unsigned char state[AVC_FRAME_SLOTS];
    unsigned int frame_id[AVC_FRAME_SLOTS];
    unsigned int decoded_at_us[AVC_FRAME_SLOTS];
    int queued;
} AvcFrameSlots;
void avc_frame_slots_init(AvcFrameSlots *s);
/* Atomic reservation of three slots; failure leaves state unchanged. */
int avc_frame_slots_reserve(AvcFrameSlots *s,int slots[3]);
/* Publishes only the newest of count chronological pictures. Earlier pictures
 * are intentionally skipped for latency, never reclassified as decode errors.
 * count=0 cancels the reservation. ids must describe outputs, not current AU. */
int avc_frame_slots_publish(AvcFrameSlots *s,const int slots[3],int count,
                            const unsigned int *ids,
                            const unsigned int *decoded_at_us);
int avc_frame_slots_take(AvcFrameSlots *s,unsigned int *frame_id,
                         unsigned int *decoded_at_us);
int avc_frame_slots_release(AvcFrameSlots *s,int slot);
#endif
