#include "avc_frame_slots.h"
#include <string.h>
void avc_frame_slots_init(AvcFrameSlots *s) {
    memset(s,0,sizeof(*s)); s->queued=-1;
}
int avc_frame_slots_reserve(AvcFrameSlots *s,int slots[3]) {
    int found[3],i,n=0;
    if(!s || !slots) return -1;
    for(i=0;i<AVC_FRAME_SLOTS && n<3;i++)
        if(s->state[i]==AVC_SLOT_FREE) found[n++]=i;
    if(n!=3) return -1;
    for(i=0;i<3;i++) { slots[i]=found[i]; s->state[found[i]]=AVC_SLOT_WRITING; }
    return 0;
}
int avc_frame_slots_publish(AvcFrameSlots *s,const int slots[3],int count,
                            const unsigned int *ids,
                            const unsigned int *decoded_at_us) {
    int i,j;
    if(!s || !slots || count<0 || count>3 ||
       (count && (!ids || !decoded_at_us))) return -1;
    for(i=0;i<3;i++) {
        if(slots[i]<0 || slots[i]>=AVC_FRAME_SLOTS ||
           s->state[slots[i]]!=AVC_SLOT_WRITING) return -1;
        for(j=0;j<i;j++) if(slots[i]==slots[j]) return -1;
    }
    for(i=0;i<3;i++) s->state[slots[i]]=AVC_SLOT_FREE;
    if(count) {
        if(s->queued>=0) s->state[s->queued]=AVC_SLOT_FREE;
        s->queued=slots[count-1];
        s->state[s->queued]=AVC_SLOT_QUEUED;
        s->frame_id[s->queued]=ids[count-1];
        s->decoded_at_us[s->queued]=decoded_at_us[count-1];
    }
    return 0;
}
int avc_frame_slots_take(AvcFrameSlots *s,unsigned int *frame_id,
                         unsigned int *decoded_at_us) {
    int slot;
    if(!s || !frame_id || !decoded_at_us || s->queued<0) return -1;
    slot=s->queued; s->queued=-1; s->state[slot]=AVC_SLOT_HELD;
    *frame_id=s->frame_id[slot];
    *decoded_at_us=s->decoded_at_us[slot];
    return slot;
}
int avc_frame_slots_release(AvcFrameSlots *s,int slot) {
    if(!s || slot<0 || slot>=AVC_FRAME_SLOTS || s->state[slot]!=AVC_SLOT_HELD) return -1;
    s->state[slot]=AVC_SLOT_FREE; return 0;
}
