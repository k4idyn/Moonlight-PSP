#ifndef NET_SEND_H
#define NET_SEND_H

#ifdef __cplusplus
extern "C" {
#endif

/*
 * net_send_all_psp - Send the full buffer over a PSP TCP socket.
 *
 * Returns 0 on success.
 * Returns -1 on failure and fills out_errno/out_sent when provided.
 */
int net_send_all_psp(int sock,
                     const char *data,
                     int data_len,
                     int retry_wouldblock,
                     unsigned int retry_delay_us,
                     int *out_errno,
                     int *out_sent);

#ifdef __cplusplus
}
#endif

#endif /* NET_SEND_H */
