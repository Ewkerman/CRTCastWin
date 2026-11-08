// Windows-only housekeeping for groovymister's Registered I/O (RIO) sender, without changing
// groovymister.cpp.
//
// groovymister sends every UDP packet with RIOSend(RIO_MSG_DONT_NOTIFY) but never takes the send
// completions off its send completion queue (846 entries). One raw 480i field is 200-300 packets, so
// the queue is full after a few fields, and an overflowing RIO completion queue is undefined
// behaviour (Windows may fail-fast the process inside mswsock.dll: exception 0xC0000409).
// We empty that queue from the sender thread after each send, which is what RIO expects.
//
// The members are private, so this one file opens them up. Only data members are touched (no private
// functions are called), so nothing depends on how private names are decorated.
// Everything groovymister.h includes, first, so the access trick below only reaches GroovyMister.
#include <winsock2.h>
#include <ws2tcpip.h>
#include <mswsock.h>
#include <inttypes.h>
#include <cstdio>
#include <cstring>
#include "rio.h"

#define private public
#include "groovymister.h"
#undef private

#include "groovy_rio.h"

// Same layout as in GroovyBridge.cpp.
struct GMHandle {
    GroovyMister gm;
};

long gm_rio_drain_sends(GMHandle *h) {
#if !CRTCAST_GROOVY_RIO
    (void) h;
    return 0;   // plain-socket sender: no RIO queues (and they are never initialized)
#endif
    if (!h || !h->gm.m_isConnected || h->gm.m_sendQueue == nullptr) return 0;
    RIORESULT results[256];
    long total = 0;
    for (;;) {
        ULONG n = h->gm.m_rio.RIODequeueCompletion(h->gm.m_sendQueue, results, 256);
        if (n == RIO_CORRUPT_CQ) return -1;
        if (n == 0) break;
        total += long(n);
    }
    return total;
}
