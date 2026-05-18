/* rs.h - Reed-Solomon FEC codec for RTP packet loss recovery (128 shard max) */
#ifndef __RS_H_
#define __RS_H_

/* PSP: must be small — reed_solomon_decode uses DATA_SHARDS_MAX^2 bytes on STACK.
 * At 255 that's 65KB → stack overflow on PSP (32-64KB thread stacks).
 * 128 matches FEC_MAX_PACKETS and gives ~16KB stack usage. */
#define DATA_SHARDS_MAX 128

typedef struct _reed_solomon {
    int data_shards;
    int parity_shards;
    int shards;
    unsigned char* m;
    unsigned char* parity;
} reed_solomon;

/**
 * MUST initial one time
 * */
void reed_solomon_init(void);

/* The PSP build keeps the RS tables/context in static storage. Hold this
 * lock across reed_solomon_new() + encode/reconstruct when multiple stream
 * threads may use RS at the same time. */
void reed_solomon_global_lock(void);
void reed_solomon_global_unlock(void);

reed_solomon* reed_solomon_new(int data_shards, int parity_shards);
void reed_solomon_release(reed_solomon* rs);

/**
 * encode a big size of buffer
 * input:
 * rs
 * nr_shards: assert(0 == nr_shards % rs->data_shards)
 * shards[nr_shards][block_size]
 * */
int reed_solomon_encode(reed_solomon* rs, unsigned char** shards, int nr_shards, int block_size);

/**
 * reconstruct a big size of buffer
 * input:
 * rs
 * nr_shards: assert(0 == nr_shards % rs->data_shards)
 * shards[nr_shards][block_size]
 * marks[nr_shards] marks as errors
 * */
int reed_solomon_reconstruct(reed_solomon* rs, unsigned char** shards, unsigned char* marks, int nr_shards, int block_size);
#endif
