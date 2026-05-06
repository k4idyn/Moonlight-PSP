#include "net_send.h"

#include <pspthreadman.h>
#include <pspnet.h>
#include <pspnet_inet.h>
#include <sys/socket.h>
#include <errno.h>

int net_send_all_psp(int sock,
                     const char *data,
                     int data_len,
                     int retry_wouldblock,
                     unsigned int retry_delay_us,
                     int *out_errno,
                     int *out_sent)
{
    int sent = 0;

    if (out_errno) *out_errno = 0;
    if (out_sent) *out_sent = 0;

    while (sent < data_len) {
        int ret = (int)sceNetInetSend(sock, data + sent, data_len - sent, 0);
        if (ret > 0) {
            sent += ret;
            continue;
        }

        if (ret < 0) {
            int err = sceNetInetGetErrno();
            if (retry_wouldblock &&
                (err == EAGAIN || err == EWOULDBLOCK)) {
                if (retry_delay_us > 0) {
                    sceKernelDelayThread(retry_delay_us);
                }
                continue;
            }
            if (out_errno) *out_errno = err;
            if (out_sent) *out_sent = sent;
            return -1;
        }

        /* ret == 0 means peer closed before full send. */
        if (out_errno) *out_errno = 0;
        if (out_sent) *out_sent = sent;
        return -1;
    }

    if (out_sent) *out_sent = sent;
    return 0;
}
