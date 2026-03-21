#pragma once

#include "Limelight.h"
#include "Platform.h"
#ifdef __3DS__
#include <netinet/in.h>

#ifdef AF_INET6
#undef AF_INET6
#endif

extern in_port_t n3ds_udp_port;
#endif

#ifdef AF_INET6
#undef AF_INET6
#endif
#ifdef _PSP
#include <pspnet.h>
#include <pspnet_inet.h>
#include <pspnet_resolver.h>
#include <pspnet_apctl.h>
#include <psputility_netparam.h>
#include <pspthreadman.h>
#include <psprtc.h>
#ifdef AF_INET6
#undef AF_INET6
#endif
#include <poll.h>
#define ioctl sceNetInetIoctl

// Explicitly declare sceNetInetIoctl if it's missing from headers
extern int sceNetInetIoctl(int s, unsigned long cmd, void *arg);

#ifndef FIONBIO
#define FIONBIO 0x8004667e
#endif

#ifndef _SOCKADDR_STORAGE
#define _SOCKADDR_STORAGE
#ifndef HAS_SOCKADDR_STORAGE
struct sockaddr_storage {
    unsigned char ss_len;
    unsigned char ss_family;
    char __ss_padding[128 - 2];
};
#endif
#endif

#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/select.h>
#include <sys/time.h>
#include <netdb.h>

#ifndef AI_PASSIVE
struct addrinfo {
    int ai_flags;
    int ai_family;
    int ai_socktype;
    int ai_protocol;
    socklen_t ai_addrlen;
    char *ai_canonname;
    struct sockaddr *ai_addr;
    struct addrinfo *ai_next;
};
#define AI_PASSIVE 1
#define AI_CANONNAME 2
#define AI_NUMERICHOST 4
#endif

#ifndef AI_ADDRCONFIG
#define AI_ADDRCONFIG 32
#endif

// Map to sceNetInet versions if they exist, or just declare them
extern int getaddrinfo(const char *node, const char *service,
                       const struct addrinfo *hints,
                       struct addrinfo **res);
extern void freeaddrinfo(struct addrinfo *res);

#if !defined(POLLIN) && !defined(_PSP_POLL_H)
struct pollfd {
    int fd;
    short events;
    short revents;
};
#define POLLIN 0x0001
#define POLLOUT 0x0004
#define POLLERR 0x0008
#define POLLHUP 0x0010
#define POLLNVAL 0x0020
#define POLLRDNORM POLLIN
#define POLLWRNORM POLLOUT
#endif

#define select sceNetInetSelect
#define getsockopt sceNetInetGetsockopt
#define setsockopt sceNetInetSetsockopt
#define getsockname sceNetInetGetsockname
#define getpeername sceNetInetGetpeername
#define shutdown sceNetInetShutdown
#define socket sceNetInetSocket
#define connect sceNetInetConnect
#define bind sceNetInetBind
#define listen sceNetInetListen
#define accept sceNetInetAccept
#define send sceNetInetSend
#define sendto sceNetInetSendto
#define recv sceNetInetRecv
#define recvfrom sceNetInetRecvfrom
#endif

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <wlanapi.h>
#ifndef __MINGW32__
#include <timeapi.h>
#else
#include <mmsystem.h>
#endif
#define SetLastSocketError(x) WSASetLastError(x)
#define LastSocketError() WSAGetLastError()

#define SHUT_RDWR SD_BOTH

#ifdef EAGAIN
#undef EAGAIN
#endif
#define EAGAIN WSAEWOULDBLOCK

#ifdef EINTR
#undef EINTR
#endif
#define EINTR WSAEINTR

#ifdef __MINGW32__
#undef EWOULDBLOCK
#undef EINPROGRESS
#undef ETIMEDOUT
#undef ECONNREFUSED
#endif

#define EWOULDBLOCK WSAEWOULDBLOCK
#define EINPROGRESS WSAEINPROGRESS
#define ETIMEDOUT WSAETIMEDOUT
#define ECONNREFUSED WSAECONNREFUSED

typedef int SOCK_RET;
typedef int SOCKADDR_LEN;

#else

#include <sys/types.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <netinet/tcp.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <errno.h>
#include <signal.h>
#include <poll.h>

#define ioctlsocket ioctl
#ifdef _PSP
#define LastSocketError() sceNetInetGetErrno()
#define SetLastSocketError(x) (void)(x)
#else
#define LastSocketError() errno
#define SetLastSocketError(x) errno = x
#endif
#define INVALID_SOCKET -1
#define SOCKET_ERROR -1

typedef int SOCKET;
typedef ssize_t SOCK_RET;
typedef socklen_t SOCKADDR_LEN;
#endif

#ifdef _PSP
typedef struct sockaddr_in LC_SOCKADDR;
#define SET_FAMILY(addr, family) do { \
    (addr)->sin_len = sizeof(struct sockaddr_in); \
    (addr)->sin_family = (family); \
} while (0)
#define SET_PORT(addr, port) ((addr)->sin_port = htons(port))
#else
#ifdef AF_INET6
typedef struct sockaddr_in6 LC_SOCKADDR;
#define SET_FAMILY(addr, family) ((addr)->sin6_family = (family))
#define SET_PORT(addr, port) ((addr)->sin6_port = htons(port))
#else
typedef struct sockaddr_in LC_SOCKADDR;
#define SET_FAMILY(addr, family) ((addr)->sin_family = (family))
#define SET_PORT(addr, port) ((addr)->sin_port = htons(port))
#endif
#endif

#define LastSocketFail() ((LastSocketError() != 0) ? LastSocketError() : -1)

#ifdef AF_INET6
// IPv6 addresses have 2 extra characters for URL escaping
#define URLSAFESTRING_LEN (INET6_ADDRSTRLEN+2)
#else
#define URLSAFESTRING_LEN INET_ADDRSTRLEN
#endif
void addrToUrlSafeString(struct sockaddr_storage* addr, char* string, size_t stringLength);

#define SOCK_QOS_TYPE_BEST_EFFORT 0
#define SOCK_QOS_TYPE_AUDIO 1
#define SOCK_QOS_TYPE_VIDEO 2

SOCKET createSocket(int addressFamily, int socketType, int protocol, bool nonBlocking);
SOCKET connectTcpSocket(struct sockaddr_storage* dstaddr, SOCKADDR_LEN addrlen, unsigned short port, int timeoutSec);
int getLocalAddressByUdpConnect(const struct sockaddr_storage* targetAddr, SOCKADDR_LEN targetAddrLen,  unsigned short targetPort,
                                struct sockaddr_storage* localAddr, SOCKADDR_LEN* localAddrLen);
int sendMtuSafe(SOCKET s, char* buffer, int size);
SOCKET bindUdpSocket(int addressFamily, struct sockaddr_storage* localAddr, SOCKADDR_LEN addrLen, int bufferSize, int socketQosType);
int enableNoDelay(SOCKET s);
int setSocketNonBlocking(SOCKET s, bool enabled);
int recvUdpSocket(SOCKET s, char* buffer, int size, bool useSelect);
void shutdownTcpSocket(SOCKET s);
int setNonFatalRecvTimeoutMs(SOCKET s, int timeoutMs);
void closeSocket(SOCKET s);
bool isPrivateNetworkAddress(struct sockaddr_storage* address);
bool isNat64SynthesizedAddress(struct sockaddr_storage* address);
int pollSockets(struct pollfd* pollFds, int pollFdsCount, int timeoutMs);
bool isSocketReadable(SOCKET s);

#define TCP_PORT_MASK 0xFFFF
#define TCP_PORT_FLAG_ALWAYS_TEST 0x10000
int resolveHostName(const char* host, int family, int tcpTestPort, struct sockaddr_storage* addr, SOCKADDR_LEN* addrLen);

void enterLowLatencyMode(void);
void exitLowLatencyMode(void);

int initializePlatformSockets(void);
void cleanupPlatformSockets(void);
