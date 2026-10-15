// GameCube multiplayer backend: UDP over the Broadband Adapter (libogc lwIP).
//
// Same model as the desktop GameNetworkingSockets backend (stdComm_GNS.cpp), which
// the engine side is written against:
//  - Star topology. The host is player id 1; every message goes through it, and it
//    forwards messages addressed to another client.
//  - A joining client says HELLO; the host answers WELCOME with the client's id and
//    the session settings.
//  - DirectPlay_Receive returns 0 (message), 2 (player id lost) or -1 (nothing).
//
// Why UDP and not TCP: the game was written for DirectPlay datagrams and does its
// own reliability (sithComm: important messages carry an id, are acknowledged and
// resent, duplicates dropped). And Dolphin's emulated adapter can't accept incoming
// TCP connections, so with TCP a Dolphin player could never host.
// What this layer adds: the join handshake, keep-alive pings (a player silent for
// GCCOMM_PEER_TIMEOUT_MS / GCCOMM_TIMEOUT_MS is gone) and splitting messages
// larger than one datagram.
//
// Games are found on the local network without typing addresses: hosts broadcast a
// beacon every second and answer "any games?" queries directly.
//
// Wire format: a 20-byte header with big-endian fields (written explicitly, so
// another platform can join later), then the message bytes. The game message itself
// is the engine's native layout (GameCube to GameCube only, for now).

#include "stdComm_gc.h"

#include "Win95/stdComm.h"
#include "Dss/sithMulti.h"
#include "Gui/jkGUINetHost.h"
#include "Main/jkMain.h"
#include "General/stdString.h"
#include "stdPlatform.h"
#include "jk.h"

#include "gcNet.h"
#include "gcGame.h"

#include "gcOgc.h" // libogc without its BOOL
#include <network.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>

#define GCCOMM_GAME_PORT     27020
#define GCCOMM_DISC_PORT     27021
#define GCCOMM_MAX_CLIENTS   10          // at most 10 players (host + 9, or a dedicated host + 10)
#define GCCOMM_MAGIC         0x4A4B4743u // "JKGC"
#define GCCOMM_QUERY         0x4A4B4751u // "JKGQ": "any games?" (answered with a beacon)
#define GCCOMM_VERSION       2
#define GCCOMM_MAX_MSG       4096
#define GCCOMM_HDR           20
#define GCCOMM_FRAG          1200        // payload bytes per datagram (under any MTU)
#define GCCOMM_MAX_FRAGS     ((GCCOMM_MAX_MSG + GCCOMM_FRAG - 1) / GCCOMM_FRAG)
#define GCCOMM_BEACON_MS     1000
#define GCCOMM_PING_MS       1000
#define GCCOMM_TIMEOUT_MS    30000       // (client) silence after which the host is gone (it may be loading a level)
#define GCCOMM_PEER_TIMEOUT_MS 60000     // (host) silence after which a player is gone: a joining
                                         // player says nothing while it loads the level
#define GCCOMM_POLL_GAP_MS   2000        // not polled for this long: we were busy (loading), not them
static uint32_t gcComm_lastPollMs = 0;   // see gcComm_PollGap
#define GCCOMM_SESSION_TTL   4000        // a game stops being listed after this long without a beacon
#define GCCOMM_CONNECT_MS    4000
#define GCCOMM_HELLO_MS      300

// libbba is built against lwIP's socket headers, whose values differ from the
// newlib ones <network.h> pulls in.
#define GCCOMM_SOL_SOCKET    0xfff
#define GCCOMM_FIONBIO       0x8004667Eu // _IOW('f', 126, unsigned long)

enum {
    GCPKT_DATA    = 0, // (a piece of) a game message
    GCPKT_HELLO   = 1, // client -> host: let me in
    GCPKT_WELCOME = 2, // host -> client: your id (idTo) + the session settings
    GCPKT_PING    = 3, // keep-alive
    GCPKT_BYE     = 4, // leaving
};

#pragma pack(push, 4)
typedef struct gcCommBeacon
{
    uint32_t magic;
    uint32_t version;
    uint32_t gameTag;    // JK or MotS: the two never see each other's games
    uint32_t gamePort;
    jkMultiEntry entry;
} gcCommBeacon;
#pragma pack(pop)

// One remote end: a client (on the host) or the host (on a client).
typedef struct gcCommPeer
{
    int       bUsed;
    uint32_t  ip;        // network order
    uint16_t  port;      // host order
    uint32_t  id;
    uint32_t  lastHeardMs;
    uint32_t  lastPingMs;
    // Reassembly of the message being received in pieces.
    uint32_t  rxSeq;
    uint32_t  rxMask;
    uint32_t  rxCount;
    uint32_t  rxLen;
    uint8_t*  rxBuf;     // GCCOMM_MAX_MSG, allocated on first use
} gcCommPeer;

static s32        gcComm_sock = -1;          // game traffic (host and client)
static uint32_t   gcComm_txSeq = 1;
static uint8_t    gcComm_pkt[GCCOMM_HDR + GCCOMM_FRAG];

// Host
static int        gcComm_bHosting = 0;
static gcCommPeer gcComm_aPeers[GCCOMM_MAX_CLIENTS];
static uint32_t   gcComm_usedIds = 0;        // bit (id-1); bit 0 = the host
static int        gcComm_aLost[GCCOMM_MAX_CLIENTS + 1];
static int        gcComm_numLost = 0;
static jkMultiEntry gcComm_hostEntry;
static s32        gcComm_beaconSock = -1;
static uint32_t   gcComm_lastBeaconMs = 0;

// Client
static gcCommPeer gcComm_server;
static uint32_t   gcComm_selfId = 0;
static int        gcComm_bHostLost = 0;

// Session discovery (join screen)
static s32        gcComm_listenUdp = -1;
static uint32_t   gcComm_aSessionIp[32];
static uint32_t   gcComm_aSessionPort[32];
static uint32_t   gcComm_aSessionSeen[32];
static jkMultiEntry gcComm_aSessions[32];
static int        gcComm_numSessions = 0;
static uint32_t   gcComm_lastQueryMs = 0;

#define gcComm_Printf(fmt, ...) stdPlatform_Printf("gcComm: " fmt, ##__VA_ARGS__)

// Also on screen, in the game's message lines (the console's own log can't be
// read): who joined, who left and why, a lost host.
#include "Devices/sithConsole.h"
#include <stdarg.h>
#include <stdio.h>
static void gcComm_Show(const char* fmt, ...)
{
    char buf[128];
    va_list va;
    va_start(va, fmt);
    vsnprintf(buf, sizeof(buf), fmt, va);
    va_end(va);
    stdPlatform_Printf("gcComm: %s\n", buf);
    if (sithWorld_g_pCurrentWorld)
        sithConsole_PrintString(buf);
}

// Milliseconds from `then` to `now`, signed: a time stamped later in the same poll
// (a player who just said HELLO) is "in the future" of that poll's `now`, and an
// unsigned difference made it look like ages ago -- the host dropped every
// joining player as timed out right after welcoming them.
// A stamp of 0 means "never" (the clock itself is not 0-based on the GameCube: a
// signed difference from 0 can come out negative, and then beacons and pings
// were never sent).
static inline int32_t gcComm_Since(uint32_t now, uint32_t then)
{
    if (!then) return 0x7FFFFFFF;
    return (int32_t)(now - then);
}

// Every net_* call here holds this lock (recursive, so helpers can nest): libogc's
// network calls must not run from two threads at once.
static mutex_t gcComm_netMutex = LWP_MUTEX_NULL;
static void gcComm_NetLock(void)
{
    if (gcComm_netMutex == LWP_MUTEX_NULL)
        LWP_MutexInit(&gcComm_netMutex, true);
    LWP_MutexLock(gcComm_netMutex);
}
static void gcComm_NetUnlock(void)
{
    LWP_MutexUnlock(gcComm_netMutex);
}

static inline void gcComm_Put32(uint8_t* p, uint32_t v)
{
    p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v;
}

static inline uint32_t gcComm_Get32(const uint8_t* p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

static uint32_t gcComm_GameTag(void)
{
    return gcGame_bMots ? 0x4A4B4D00u /* "JKM" */ : 0x4A4B3100u /* "JK1" */;
}

// --- Sockets ---------------------------------------------------------------------

// libogc's lwIP doesn't give a closed socket's resources back: net_close returns
// 0 but the next net_socket fails with -ENOBUFS, so a second hosted game (or a
// join after hosting) failed with a network error. The two ports the game uses
// get one socket each, opened the first time and kept for good; "closing" one
// just throws away whatever is waiting in it.
#define GCCOMM_NUM_PORTS 2
static s32 gcComm_aPortSock[GCCOMM_NUM_PORTS] = { -1, -1 };
static uint16_t gcComm_aPort[GCCOMM_NUM_PORTS] = { 0, 0 };

static s32 gcComm_OpenUdp(uint16_t port, int bBroadcast)
{
    gcComm_NetLock();
    int slot = -1;
    for (int i = 0; i < GCCOMM_NUM_PORTS; i++) {
        if (gcComm_aPortSock[i] >= 0 && gcComm_aPort[i] == port) { slot = i; break; }
        if (slot < 0 && gcComm_aPortSock[i] < 0) slot = i;
    }
    if (slot < 0) { gcComm_NetUnlock(); return -1; }
    if (gcComm_aPortSock[slot] < 0) {
        s32 s = net_socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
        if (s < 0) {
            gcComm_Printf("can't open a UDP socket (%d)\n", (int)s);
            gcComm_NetUnlock();
            return -1;
        }
        struct sockaddr_in addr;
        memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        addr.sin_addr.s_addr = INADDR_ANY;
        if (net_bind(s, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
            gcComm_Printf("can't use UDP port %u\n", port);
            net_close(s);
            gcComm_NetUnlock();
            return -1;
        }
        u32 one = 1;
        net_ioctl(s, GCCOMM_FIONBIO, &one); // reads return -EWOULDBLOCK instead of waiting
        gcComm_aPortSock[slot] = s;
        gcComm_aPort[slot] = port;
    }
    s32 s = gcComm_aPortSock[slot];
    u32 bcast = bBroadcast ? 1 : 0;
    net_setsockopt(s, GCCOMM_SOL_SOCKET, SO_BROADCAST, &bcast, sizeof(bcast));
    gcComm_NetUnlock();
    return s;
}

// "Close": drop what's queued (stale packets of the last game), keep the socket.
static void gcComm_CloseUdp(s32 s)
{
    if (s < 0) return;
    gcComm_NetLock();
    uint8_t junk[64];
    for (int i = 0; i < 256; i++) {
        struct sockaddr_in from;
        socklen_t flen = sizeof(from);
        if (net_recvfrom(s, junk, sizeof(junk), 0, (struct sockaddr*)&from, &flen) < 0) break;
    }
    gcComm_NetUnlock();
}

// Send failures (lwIP out of buffers, the adapter...): shown on screen the first
// time and every 100 after, since they silently cut a player off.
static uint32_t gcComm_statSendErrors = 0;

static void gcComm_SendTo(s32 s, uint32_t ip, uint16_t port, const void* data, uint32_t len)
{
    struct sockaddr_in to;
    memset(&to, 0, sizeof(to));
    to.sin_family = AF_INET;
    to.sin_port = htons(port);
    to.sin_addr.s_addr = ip;
    gcComm_NetLock();
    s32 rc = net_sendto(s, (void*)data, len, 0, (struct sockaddr*)&to, sizeof(to));
    gcComm_NetUnlock();
    if (rc < 0) {
        if (!(gcComm_statSendErrors++ % 100))
            gcComm_Show("Network: send error %d (%u so far)", (int)rc, gcComm_statSendErrors);
    }
}

// Sends one message to a peer, in as many datagrams as it takes.
static void gcComm_SendMsg(gcCommPeer* p, uint32_t type, uint32_t idFrom, uint32_t idTo, const void* data, uint32_t len)
{
    if (gcComm_sock < 0 || !p->bUsed) return;
    if (len > GCCOMM_MAX_MSG) len = GCCOMM_MAX_MSG;
    uint32_t count = len ? (len + GCCOMM_FRAG - 1) / GCCOMM_FRAG : 1;
    uint32_t seq = gcComm_txSeq++;
    for (uint32_t i = 0; i < count; i++) {
        uint32_t off = i * GCCOMM_FRAG;
        uint32_t n = len - off < GCCOMM_FRAG ? len - off : GCCOMM_FRAG;
        gcComm_Put32(gcComm_pkt + 0, GCCOMM_MAGIC);
        gcComm_Put32(gcComm_pkt + 4, (type << 24) | (i << 16) | (count << 8));
        gcComm_Put32(gcComm_pkt + 8, seq);
        gcComm_Put32(gcComm_pkt + 12, idFrom);
        gcComm_Put32(gcComm_pkt + 16, idTo);
        if (n) memcpy(gcComm_pkt + GCCOMM_HDR, (const uint8_t*)data + off, n);
        gcComm_SendTo(gcComm_sock, p->ip, p->port, gcComm_pkt, GCCOMM_HDR + n);
    }
}

// A received datagram, split up.
typedef struct gcCommPkt
{
    uint32_t type, frag, count, seq, from, to;
    const uint8_t* data;
    uint32_t len;
    uint32_t ip;
    uint16_t port;
} gcCommPkt;

static uint8_t gcComm_rxPkt[GCCOMM_HDR + GCCOMM_FRAG + 64];
static s32        gcComm_OpenUdp(uint16_t port, int bBroadcast);
static void       gcComm_CloseUdp(s32 s);

// Next datagram on the game socket. Returns 0 when there is none.
// Receive statistics, logged when the host is lost (network trouble shooting).
static uint32_t gcComm_statRx = 0, gcComm_statBad = 0, gcComm_statOther = 0;
static uint32_t gcComm_statOtherIp = 0, gcComm_statOtherPort = 0, gcComm_statLastErr = 0;

// Receive queue. The game reads the network once a frame, and not at all while it
// loads a level -- which is exactly when a host sends a joining player the whole
// world (and resends what isn't acknowledged). Left in lwIP, those datagrams used
// up its buffers: the joining player could then neither send (-ENOBUFS) nor
// receive, and lost the host. During a level load stdComm_Gc_Pump (called from
// the loading screen's progress) empties the game socket into this queue, which
// gcComm_NextPkt reads first. (A separate receive thread made it worse: libogc's
// network calls don't mix well across threads.)
#define GCCOMM_RXQ_SLOTS 96
#define GCCOMM_RXQ_SLOT  (GCCOMM_HDR + GCCOMM_FRAG + 64)
typedef struct gcCommRxSlot { int32_t len; uint32_t ip; uint16_t port; } gcCommRxSlot;
static uint8_t       gcComm_aRxQData[GCCOMM_RXQ_SLOTS][GCCOMM_RXQ_SLOT];
static gcCommRxSlot  gcComm_aRxQ[GCCOMM_RXQ_SLOTS];
static volatile uint32_t gcComm_rxqHead = 0, gcComm_rxqTail = 0; // head: next write, tail: next read
static uint32_t      gcComm_statRxqDropped = 0;
static uint8_t       gcComm_rxScratch[GCCOMM_RXQ_SLOT];

static void gcComm_RxLock(void)   { gcComm_NetLock(); }
static void gcComm_RxUnlock(void) { gcComm_NetUnlock(); }

// Added: the sockets are never really closed (see gcComm_OpenUdp), so one not in
// use still receives: the discovery port gets the host's beacons twice a second
// all through the game. Unread, each one holds an lwIP buffer, and within seconds
// there were none left -- the joining player could neither receive nor send
// (-ENOBUFS) and both ends timed out ("contacting host", 8 packets). Whatever
// arrives on a socket nobody is using is thrown away here (lock held).
static uint32_t gcComm_statIdleDropped = 0;
static void gcComm_DrainIdleLocked(void)
{
    uint8_t junk[64];
    for (int i = 0; i < GCCOMM_NUM_PORTS; i++) {
        s32 s = gcComm_aPortSock[i];
        if (s < 0 || s == gcComm_sock || s == gcComm_listenUdp || s == gcComm_beaconSock) continue;
        for (int k = 0; k < 64; k++) {
            struct sockaddr_in from;
            socklen_t flen = sizeof(from);
            if (net_recvfrom(s, junk, sizeof(junk), 0, (struct sockaddr*)&from, &flen) < 0) break;
            gcComm_statIdleDropped++;
        }
    }
}

// Moves whatever is waiting on the game socket into the queue (lock held).
static void gcComm_RxDrainLocked(void)
{
    gcComm_DrainIdleLocked();
    while (gcComm_sock >= 0) {
        struct sockaddr_in from;
        socklen_t flen = sizeof(from);
        int bFull = gcComm_rxqHead - gcComm_rxqTail >= GCCOMM_RXQ_SLOTS;
        uint32_t slot = gcComm_rxqHead % GCCOMM_RXQ_SLOTS;
        uint8_t* dst = bFull ? gcComm_rxScratch : gcComm_aRxQData[slot];
        s32 n = net_recvfrom(gcComm_sock, dst, GCCOMM_RXQ_SLOT, 0, (struct sockaddr*)&from, &flen);
        if (n < 0) { gcComm_statLastErr = (uint32_t)-n; break; } // nothing more (-EWOULDBLOCK)
        if (bFull) { gcComm_statRxqDropped++; continue; } // the game resends what matters
        gcComm_aRxQ[slot].len = n;
        gcComm_aRxQ[slot].ip = from.sin_addr.s_addr;
        gcComm_aRxQ[slot].port = ntohs(from.sin_port);
        gcComm_rxqHead++;
    }
}

// Loading screen progress: keep lwIP's buffers free while the game is busy.
void stdComm_Gc_Pump(void)
{
    if (gcComm_sock < 0) return;
    gcComm_RxLock();
    gcComm_RxDrainLocked();
    gcComm_RxUnlock();
}

// The game socket changes (opened, closed): the queue belongs to the old one.
static void gcComm_SetGameSock(s32 s)
{
    gcComm_RxLock();
    gcComm_sock = s;
    gcComm_rxqHead = gcComm_rxqTail = 0;
    gcComm_RxUnlock();
}

// Leaving a game (host or player, by any way out): nothing of it may be left for
// the next one -- the socket is emptied, the queue and the counters start over.
static void gcComm_CloseGameSock(void)
{
    gcComm_RxLock();
    gcComm_CloseUdp(gcComm_sock);
    gcComm_sock = -1;
    gcComm_rxqHead = gcComm_rxqTail = 0;
    gcComm_RxUnlock();
    gcComm_statRx = gcComm_statBad = gcComm_statOther = gcComm_statRxqDropped = gcComm_statIdleDropped = 0;
    gcComm_statSendErrors = 0;
    gcComm_lastPollMs = 0;
}

static int gcComm_NextPkt(gcCommPkt* pk)
{
    while (1) {
        struct sockaddr_in from;
        gcComm_RxLock();
        gcComm_RxDrainLocked();
        if (gcComm_rxqTail == gcComm_rxqHead) { gcComm_RxUnlock(); return 0; }
        uint32_t slot = gcComm_rxqTail % GCCOMM_RXQ_SLOTS;
        s32 n = gcComm_aRxQ[slot].len;
        memcpy(gcComm_rxPkt, gcComm_aRxQData[slot], n);
        from.sin_addr.s_addr = gcComm_aRxQ[slot].ip;
        from.sin_port = htons(gcComm_aRxQ[slot].port);
        gcComm_rxqTail++;
        gcComm_RxUnlock();
        gcComm_statRx++;
        if (n < GCCOMM_HDR || gcComm_Get32(gcComm_rxPkt) != GCCOMM_MAGIC) { gcComm_statBad++; continue; }
        uint32_t tfc = gcComm_Get32(gcComm_rxPkt + 4);
        pk->type = tfc >> 24;
        pk->frag = (tfc >> 16) & 0xFF;
        pk->count = (tfc >> 8) & 0xFF;
        pk->seq = gcComm_Get32(gcComm_rxPkt + 8);
        pk->from = gcComm_Get32(gcComm_rxPkt + 12);
        pk->to = gcComm_Get32(gcComm_rxPkt + 16);
        pk->data = gcComm_rxPkt + GCCOMM_HDR;
        pk->len = n - GCCOMM_HDR;
        pk->ip = from.sin_addr.s_addr;
        pk->port = ntohs(from.sin_port);
        if (!pk->count || pk->count > GCCOMM_MAX_FRAGS || pk->frag >= pk->count) continue;
        return 1;
    }
    return 0;
}

// Adds a DATA piece to the peer's message. Returns the complete message (and its
// length) once all pieces are in, else NULL. A message whose pieces don't all
// arrive is dropped (the game resends what matters).
static const uint8_t* gcComm_Reassemble(gcCommPeer* p, const gcCommPkt* pk, uint32_t* pLen)
{
    if (pk->count == 1) {
        *pLen = pk->len;
        return pk->data;
    }
    if (!p->rxBuf) {
        p->rxBuf = (uint8_t*)malloc(GCCOMM_MAX_MSG);
        if (!p->rxBuf) return NULL;
    }
    if (pk->seq != p->rxSeq || !p->rxCount) {
        p->rxSeq = pk->seq;
        p->rxCount = pk->count;
        p->rxMask = 0;
        p->rxLen = 0;
    }
    uint32_t off = pk->frag * GCCOMM_FRAG;
    if (pk->len > GCCOMM_FRAG || off + pk->len > GCCOMM_MAX_MSG) return NULL;
    memcpy(p->rxBuf + off, pk->data, pk->len);
    p->rxMask |= 1u << pk->frag;
    if (pk->frag == pk->count - 1) p->rxLen = off + pk->len;
    if (p->rxMask != (1u << p->rxCount) - 1) return NULL;
    p->rxCount = 0;
    *pLen = p->rxLen;
    return p->rxBuf;
}

static void gcComm_PeerReset(gcCommPeer* p)
{
    free(p->rxBuf);
    memset(p, 0, sizeof(*p));
}

// --- Host --------------------------------------------------------------------------

static int gcComm_HostPlayersInGame(void)
{
    int amt = 0;
    for (int i = 0; i < jkPlayer_maxPlayers; i++) {
        if (!i && jkGuiNetHost_bIsDedicated) continue;
        if ((jkPlayer_playerInfos[i].flags & 2) != 0 && !jkPlayer_playerInfos[i].playerNetId)
            continue;
        amt++;
    }
    return amt;
}

static gcCommPeer* gcComm_HostPeerById(uint32_t id)
{
    for (int i = 0; i < GCCOMM_MAX_CLIENTS; i++)
        if (gcComm_aPeers[i].bUsed && gcComm_aPeers[i].id == id)
            return &gcComm_aPeers[i];
    return NULL;
}

static gcCommPeer* gcComm_HostPeerByAddr(uint32_t ip, uint16_t port)
{
    for (int i = 0; i < GCCOMM_MAX_CLIENTS; i++)
        if (gcComm_aPeers[i].bUsed && gcComm_aPeers[i].ip == ip && gcComm_aPeers[i].port == port)
            return &gcComm_aPeers[i];
    return NULL;
}

static void gcComm_HostDropPeer(gcCommPeer* p, const char* why)
{
    uint32_t id = p->id;
    gcComm_Show("Network: player %u left (%s)", id, why);
    // Report it to the engine only if it had joined the game (as the GNS backend).
    for (int i = 0; i < jkPlayer_maxPlayers; i++) {
        if (!i && jkGuiNetHost_bIsDedicated) continue;
        if ((jkPlayer_playerInfos[i].flags & 2) != 0 && jkPlayer_playerInfos[i].playerNetId == (int)id) {
            if (gcComm_numLost < GCCOMM_MAX_CLIENTS + 1) gcComm_aLost[gcComm_numLost++] = id;
            break;
        }
    }
    gcComm_usedIds &= ~(1u << (id - 1));
    gcComm_PeerReset(p);
}

static void gcComm_HostWelcome(gcCommPeer* p)
{
    jkPlayer_maxPlayers = gcComm_hostEntry.maxPlayers;
    jkMultiEntry e = gcComm_hostEntry;
    e.multiModeFlags = sithMulti_multiModeFlags;
    e.numPlayers = gcComm_HostPlayersInGame();
    gcComm_SendMsg(p, GCPKT_WELCOME, 1, p->id, &e, sizeof(e));
}

static void gcComm_HostHello(const gcCommPkt* pk)
{
    gcCommPeer* p = gcComm_HostPeerByAddr(pk->ip, pk->port);
    if (!p) {
        for (int i = 0; i < GCCOMM_MAX_CLIENTS; i++)
            if (!gcComm_aPeers[i].bUsed) { p = &gcComm_aPeers[i]; break; }
        uint32_t id = 0;
        for (int i = 1; i < 32; i++)
            if (!(gcComm_usedIds & (1u << i))) { id = i + 1; break; }
        if (!p || !id) {
            gcComm_Show("Network: game full, a join was ignored");
            return;
        }
        memset(p, 0, sizeof(*p));
        p->bUsed = 1;
        p->ip = pk->ip;
        p->port = pk->port;
        p->id = id;
        gcComm_usedIds |= 1u << (id - 1);
        struct in_addr a; a.s_addr = pk->ip;
        gcComm_Show("Network: player %u joining from %s:%u", id, inet_ntoa(a), pk->port);
    }
    p->lastHeardMs = stdPlatform_GetTimeMsec();
    if (!p->lastPingMs) p->lastPingMs = p->lastHeardMs;
    gcComm_HostWelcome(p); // again for every HELLO: the previous answer may be lost
}

static void gcComm_MakeBeacon(gcCommBeacon* pB)
{
    memset(pB, 0, sizeof(*pB));
    gcComm_Put32((uint8_t*)&pB->magic, GCCOMM_MAGIC);
    gcComm_Put32((uint8_t*)&pB->version, GCCOMM_VERSION);
    gcComm_Put32((uint8_t*)&pB->gameTag, gcComm_GameTag());
    gcComm_Put32((uint8_t*)&pB->gamePort, GCCOMM_GAME_PORT);
    pB->entry = gcComm_hostEntry;
    pB->entry.multiModeFlags = sithMulti_multiModeFlags;
    pB->entry.numPlayers = gcComm_HostPlayersInGame();
    _strncpy(pB->entry.mapJklFname, jkMain_aLevelJklFname, 0x1F);
}

// Beacons to the whole network every second, and a direct answer to every query
// (networks or emulators that drop broadcasts still find the game by asking).
static void gcComm_HostBeacon(uint32_t now)
{
    if (gcComm_beaconSock < 0) return;

    gcCommBeacon b;
    uint8_t q[16];
    while (1) {
        struct sockaddr_in from;
        socklen_t flen = sizeof(from);
        gcComm_NetLock();
        s32 n = net_recvfrom(gcComm_beaconSock, q, sizeof(q), 0, (struct sockaddr*)&from, &flen);
        gcComm_NetUnlock();
        if (n < 0) break;
        if (n != 8 || gcComm_Get32(q) != GCCOMM_MAGIC || gcComm_Get32(q + 4) != GCCOMM_QUERY) continue;
        gcComm_MakeBeacon(&b);
        gcComm_SendTo(gcComm_beaconSock, from.sin_addr.s_addr, GCCOMM_DISC_PORT, &b, sizeof(b));
    }

    if (gcComm_Since(now, gcComm_lastBeaconMs) < GCCOMM_BEACON_MS) return;
    gcComm_lastBeaconMs = now;
    gcComm_MakeBeacon(&b);
    // Both the limited broadcast and the subnet's: some networks only pass one.
    gcComm_SendTo(gcComm_beaconSock, 0xFFFFFFFF, GCCOMM_DISC_PORT, &b, sizeof(b));
    uint32_t subnet = gcNet_GetBroadcast();
    if (subnet && subnet != 0xFFFFFFFF)
        gcComm_SendTo(gcComm_beaconSock, subnet, GCCOMM_DISC_PORT, &b, sizeof(b));
}

static void gcComm_HostKeepAlive(uint32_t now)
{
    for (int i = 0; i < GCCOMM_MAX_CLIENTS; i++) {
        gcCommPeer* p = &gcComm_aPeers[i];
        if (!p->bUsed) continue;
        // A player who just joined loads the level after the engine's join
        // request and says nothing meanwhile (well over 10 s on a console): a
        // 10 s limit dropped them right there. Players who quit say BYE.
        if (gcComm_Since(now, p->lastHeardMs) > GCCOMM_PEER_TIMEOUT_MS) {
            gcComm_HostDropPeer(p, "timed out");
            continue;
        }
        if (gcComm_Since(now, p->lastPingMs) >= GCCOMM_PING_MS) {
            p->lastPingMs = now;
            gcComm_SendMsg(p, GCPKT_PING, 1, p->id, NULL, 0);
        }
    }
}

static int gcComm_HostOpen(void)
{
    gcComm_lastPollMs = 0;
    for (int i = 0; i < GCCOMM_MAX_CLIENTS; i++) gcComm_PeerReset(&gcComm_aPeers[i]);
    gcComm_usedIds = 1; // id 1 = the host
    gcComm_numLost = 0;

    gcComm_SetGameSock(gcComm_OpenUdp(GCCOMM_GAME_PORT, 1));
    if (gcComm_sock < 0) return 0;
    // Dolphin's emulated adapter only opens a port on the PC side once the game
    // has sent something from it: send one datagram (ignored by everyone) so
    // joining players get through. Harmless on real hardware.
    {
        uint8_t hi[8];
        gcComm_Put32(hi, GCCOMM_MAGIC);
        gcComm_Put32(hi + 4, 0);
        uint32_t subnet = gcNet_GetBroadcast();
        gcComm_SendTo(gcComm_sock, subnet ? subnet : 0xFFFFFFFF, GCCOMM_DISC_PORT, hi, sizeof(hi));
    }
    gcComm_beaconSock = gcComm_OpenUdp(GCCOMM_DISC_PORT, 1);
    gcComm_lastBeaconMs = 0;
    gcComm_bHosting = 1;
    gcComm_Printf("hosting on %s:%d\n", gcNet_GetIp(), GCCOMM_GAME_PORT);
    return 1;
}

static void gcComm_HostClose(void)
{
    for (int i = 0; i < GCCOMM_MAX_CLIENTS; i++) {
        if (gcComm_aPeers[i].bUsed)
            gcComm_SendMsg(&gcComm_aPeers[i], GCPKT_BYE, 1, gcComm_aPeers[i].id, NULL, 0);
        gcComm_PeerReset(&gcComm_aPeers[i]);
    }
    gcComm_CloseGameSock();
    gcComm_CloseUdp(gcComm_beaconSock);
    gcComm_beaconSock = -1;
    gcComm_bHosting = 0;
    gcComm_usedIds = 0;
    gcComm_numLost = 0;
}

static int gcComm_HostSend(uint32_t idFrom, uint32_t idTo, const void* data, uint32_t len)
{
    gcCommPeer* p = gcComm_HostPeerById(idTo);
    if (p) gcComm_SendMsg(p, GCPKT_DATA, idFrom, idTo, data, len);
    return 1; // (a player no longer here is reported as lost)
}

// Whether the network went unpolled for a while (a level load, a long menu):
// the silence was ours, so it must not count against the other end.
static int gcComm_PollGap(uint32_t now)
{
    int bGap = gcComm_lastPollMs && gcComm_Since(now, gcComm_lastPollMs) > GCCOMM_POLL_GAP_MS;
    gcComm_lastPollMs = now;
    return bGap;
}

static int gcComm_HostReceive(int* pIdOut, void* pMsg, int* pLen)
{
    int maxLen = *pLen;
    *pIdOut = 0;
    *pLen = 0;

    uint32_t now = stdPlatform_GetTimeMsec();
    if (gcComm_PollGap(now)) {
        for (int i = 0; i < GCCOMM_MAX_CLIENTS; i++)
            if (gcComm_aPeers[i].bUsed) gcComm_aPeers[i].lastHeardMs = now;
    }
    gcComm_HostBeacon(now);

    gcCommPkt pk;
    while (gcComm_NextPkt(&pk)) {
        if (pk.type == GCPKT_HELLO) {
            gcComm_HostHello(&pk);
            continue;
        }
        gcCommPeer* p = gcComm_HostPeerByAddr(pk.ip, pk.port);
        if (!p) continue; // not one of ours
        p->lastHeardMs = now;
        if (pk.type == GCPKT_BYE) {
            gcComm_HostDropPeer(p, "quit");
            continue;
        }
        if (pk.type != GCPKT_DATA) continue;

        uint32_t len;
        const uint8_t* msg = gcComm_Reassemble(p, &pk, &len);
        if (!msg) continue;
        if (pk.to && pk.to != 1) {
            // For another client: pass it on.
            gcComm_HostSend(p->id, pk.to, msg, len);
            continue;
        }
        if ((int)len > maxLen) len = maxLen;
        memcpy(pMsg, msg, len);
        *pIdOut = p->id; // the address says who it is
        *pLen = len;
        return 0;
    }

    gcComm_HostKeepAlive(now);
    if (gcComm_numLost) {
        *pIdOut = gcComm_aLost[0];
        memmove(gcComm_aLost, gcComm_aLost + 1, (--gcComm_numLost) * sizeof(int));
        return 2;
    }
    return -1;
}

// --- Client ------------------------------------------------------------------------

static int gcComm_ClientOpen(uint32_t ip, uint16_t port)
{
    gcComm_selfId = 0;
    gcComm_bHostLost = 0;
    gcComm_PeerReset(&gcComm_server);
    gcComm_SetGameSock(gcComm_OpenUdp(GCCOMM_GAME_PORT, 0));
    if (gcComm_sock < 0) return 0;
    gcComm_server.bUsed = 1;
    gcComm_server.ip = ip;
    gcComm_server.port = port;
    gcComm_server.id = 1;

    // Knock until the host lets us in. The answer is taken from wherever it
    // comes: behind Dolphin's emulated adapter (NAT) a reply can show up with
    // another source address or port than the beacon said. From then on the
    // game talks to the address that answered.
    uint32_t start = stdPlatform_GetTimeMsec(), lastHello = 0;
    uint32_t numOther = 0, lastOtherIp = 0, lastOtherPort = 0, lastOtherType = 0;
    while (stdPlatform_GetTimeMsec() - start < GCCOMM_CONNECT_MS) {
        uint32_t now = stdPlatform_GetTimeMsec();
        if (!lastHello || gcComm_Since(now, lastHello) >= GCCOMM_HELLO_MS) {
            lastHello = now;
            gcComm_SendMsg(&gcComm_server, GCPKT_HELLO, 0, 1, NULL, 0);
        }
        gcCommPkt pk;
        while (gcComm_NextPkt(&pk)) {
            if (pk.type != GCPKT_WELCOME || pk.count != 1 || pk.len != sizeof(jkMultiEntry)) {
                numOther++;
                lastOtherIp = pk.ip;
                lastOtherPort = pk.port;
                lastOtherType = pk.type | (pk.len << 8);
                continue;
            }
            if (pk.ip != ip || pk.port != port) {
                struct in_addr a; a.s_addr = pk.ip;
                gcComm_Printf("the host answered from %s:%u\n", inet_ntoa(a), pk.port);
                gcComm_server.ip = pk.ip;
                gcComm_server.port = pk.port;
            }
            jkMultiEntry e;
            memcpy(&e, pk.data, sizeof(e));
            gcComm_selfId = pk.to;
            sithMulti_multiModeFlags = e.multiModeFlags;
            sithNet_MultiModeFlags = e.multiModeFlags;
            gcComm_server.lastHeardMs = gcComm_server.lastPingMs = stdPlatform_GetTimeMsec();
            {
                struct in_addr a; a.s_addr = gcComm_server.ip;
                gcComm_Printf("joined as player %u, host %s:%u\n", gcComm_selfId, inet_ntoa(a), gcComm_server.port);
            }
            gcComm_statRx = gcComm_statBad = gcComm_statOther = 0;
            gcComm_lastPollMs = 0; // the level load that follows is not silence
            return 1;
        }
        usleep(10 * 1000);
    }
    {
        struct in_addr a; a.s_addr = ip;
        gcComm_Printf("no answer from the host %s:%u\n", inet_ntoa(a), port);
        if (numOther) {
            struct in_addr o; o.s_addr = lastOtherIp;
            gcComm_Printf("  %u other packets, last from %s:%u (type %u, %u bytes; WELCOME is %u bytes)\n",
                          numOther, inet_ntoa(o), lastOtherPort, lastOtherType & 0xFF, lastOtherType >> 8,
                          (unsigned)sizeof(jkMultiEntry));
        }
    }
    gcComm_CloseGameSock();
    gcComm_PeerReset(&gcComm_server);
    return 0;
}

static void gcComm_ClientClose(void)
{
    if (gcComm_server.bUsed)
        gcComm_SendMsg(&gcComm_server, GCPKT_BYE, gcComm_selfId, 1, NULL, 0);
    gcComm_PeerReset(&gcComm_server);
    gcComm_CloseGameSock();
    gcComm_selfId = 0;
}

static int gcComm_ClientReceive(int* pIdOut, void* pMsg, int* pLen)
{
    int maxLen = *pLen;
    *pIdOut = 0;
    *pLen = 0;
    if (!gcComm_server.bUsed && !gcComm_bHostLost) return -1;

    uint32_t now = stdPlatform_GetTimeMsec();
    if (gcComm_PollGap(now))
        gcComm_server.lastHeardMs = now;
    gcCommPkt pk;
    while (!gcComm_bHostLost && gcComm_NextPkt(&pk)) {
        if (pk.ip != gcComm_server.ip || pk.port != gcComm_server.port) {
            gcComm_statOther++;
            gcComm_statOtherIp = pk.ip;
            gcComm_statOtherPort = pk.port;
            continue;
        }
        gcComm_server.lastHeardMs = now;
        if (pk.type == GCPKT_BYE) {
            gcComm_Printf("the host closed the game\n");
            gcComm_bHostLost = 1;
            break;
        }
        if (pk.type != GCPKT_DATA) continue;
        if (pk.to && pk.to != gcComm_selfId) continue; // not for us
        uint32_t len;
        const uint8_t* msg = gcComm_Reassemble(&gcComm_server, &pk, &len);
        if (!msg) continue;
        if ((int)len > maxLen) len = maxLen;
        memcpy(pMsg, msg, len);
        *pIdOut = pk.from;
        *pLen = len;
        return 0;
    }

    if (!gcComm_bHostLost && gcComm_Since(now, gcComm_server.lastHeardMs) > GCCOMM_TIMEOUT_MS) {
        struct in_addr a; a.s_addr = gcComm_server.ip;
        struct in_addr o; o.s_addr = gcComm_statOtherIp;
        gcComm_Printf("lost the host %s:%u (timed out)\n", inet_ntoa(a), gcComm_server.port);
        gcComm_Printf("  received %u packets: %u not ours, %u from %s:%u, last recv error %u, %u dropped (queue full), %u on idle ports, %u send errors\n",
                      gcComm_statRx, gcComm_statBad, gcComm_statOther, inet_ntoa(o), gcComm_statOtherPort,
                      gcComm_statLastErr, gcComm_statRxqDropped, gcComm_statIdleDropped, gcComm_statSendErrors);
        gcComm_bHostLost = 1;
    }
    if (gcComm_bHostLost) {
        gcComm_bHostLost = 0;
        gcComm_PeerReset(&gcComm_server);
        *pIdOut = 1;
        return 2;
    }
    if (gcComm_Since(now, gcComm_server.lastPingMs) >= GCCOMM_PING_MS) {
        gcComm_server.lastPingMs = now;
        gcComm_SendMsg(&gcComm_server, GCPKT_PING, gcComm_selfId, 1, NULL, 0);
    }
    return -1;
}

// --- Session discovery ---------------------------------------------------------------

static void gcComm_DiscoveryOpen(void)
{
    if (gcComm_listenUdp >= 0) return;
    gcComm_listenUdp = gcComm_OpenUdp(GCCOMM_DISC_PORT, 1);
    gcComm_numSessions = 0;
    gcComm_lastQueryMs = 0;
}

static void gcComm_DiscoveryClose(void)
{
    gcComm_CloseUdp(gcComm_listenUdp);
    gcComm_listenUdp = -1;
}

static void gcComm_DiscoveryQuery(uint32_t ip)
{
    uint8_t q[8];
    gcComm_Put32(q, GCCOMM_MAGIC);
    gcComm_Put32(q + 4, GCCOMM_QUERY);
    gcComm_SendTo(gcComm_listenUdp, ip, GCCOMM_DISC_PORT, q, sizeof(q));
}

static void gcComm_DiscoveryPump(void)
{
    if (gcComm_listenUdp < 0) return;
    uint32_t now = stdPlatform_GetTimeMsec();
    if (!gcComm_lastQueryMs || gcComm_Since(now, gcComm_lastQueryMs) >= GCCOMM_BEACON_MS) {
        gcComm_lastQueryMs = now;
        gcComm_DiscoveryQuery(0xFFFFFFFF);
        uint32_t subnet = gcNet_GetBroadcast();
        if (subnet && subnet != 0xFFFFFFFF) gcComm_DiscoveryQuery(subnet);
#ifdef GC_MP_TEST_join
        // Dolphin's emulated adapter doesn't pass broadcasts between instances:
        // ask the test host (first container on the Docker bridge) directly.
        gcComm_DiscoveryQuery(inet_addr("172.17.0.2"));
#endif
    }

    while (1) {
        gcCommBeacon b;
        struct sockaddr_in from;
        socklen_t flen = sizeof(from);
        gcComm_NetLock();
        s32 n = net_recvfrom(gcComm_listenUdp, &b, sizeof(b), 0, (struct sockaddr*)&from, &flen);
        gcComm_NetUnlock();
        if (n < 0) break; // nothing more (-EWOULDBLOCK)
        if (n != (s32)sizeof(b)) continue;
        if (gcComm_Get32((uint8_t*)&b.magic) != GCCOMM_MAGIC
            || gcComm_Get32((uint8_t*)&b.version) != GCCOMM_VERSION
            || gcComm_Get32((uint8_t*)&b.gameTag) != gcComm_GameTag())
            continue;
        uint32_t ip = from.sin_addr.s_addr;
        int idx = 0;
        while (idx < gcComm_numSessions && gcComm_aSessionIp[idx] != ip) idx++;
        if (idx == gcComm_numSessions) {
            if (gcComm_numSessions == 32) continue;
            gcComm_numSessions++;
        }
        gcComm_aSessionIp[idx] = ip;
        gcComm_aSessionPort[idx] = gcComm_Get32((uint8_t*)&b.gamePort);
        gcComm_aSessionSeen[idx] = now;
        gcComm_aSessions[idx] = b.entry;
        // Identifies the game in the list across refreshes (the UI compares it).
        memset(&gcComm_aSessions[idx].guidInstance, 0, sizeof(gcComm_aSessions[idx].guidInstance));
        memcpy(&gcComm_aSessions[idx].guidInstance, &ip, sizeof(ip));
        gcComm_aSessions[idx].field_E0 = 10;
    }
    // Forget games that went quiet.
    for (int i = 0; i < gcComm_numSessions; ) {
        if (gcComm_Since(now, gcComm_aSessionSeen[i]) > GCCOMM_SESSION_TTL) {
            gcComm_numSessions--;
            gcComm_aSessionIp[i] = gcComm_aSessionIp[gcComm_numSessions];
            gcComm_aSessionPort[i] = gcComm_aSessionPort[gcComm_numSessions];
            gcComm_aSessionSeen[i] = gcComm_aSessionSeen[gcComm_numSessions];
            gcComm_aSessions[i] = gcComm_aSessions[gcComm_numSessions];
        }
        else i++;
    }
}

// --- Engine interface (same contract as stdComm_GNS.cpp) ---------------------------

void Hack_ResetClients()
{
    DirectPlay_numPlayers = 32;
    for (int i = 0; i < 32; i++) {
        DirectPlay_aPlayers[i].dpId = i + 1;
        jk_snwprintf(DirectPlay_aPlayers[i].waName, 32, u"Player");
    }
    stdComm_dplayIdSelf = stdComm_bIsServer ? 1 : gcComm_selfId;
}

void stdComm_Gc_Startup()
{
    jkGuiMultiplayer_numConnections = 1;
    jk_snwprintf(jkGuiMultiplayer_aConnections[0].name, 0x80, u"Broadband Adapter (LAN)");
    stdComm_dword_8321E0 = 0;
    memset(jkGuiMultiplayer_aEntries, 0, sizeof(jkMultiEntry) * 32);
    dplay_dword_55D618 = 0;
}

// Added: back to a clean state when the multiplayer menu opens: whatever an
// earlier session left behind (a socket, the "connection open" flags, host or
// client mode) made the next game fail to start with "no connection".
void stdComm_Gc_ResetSession()
{
    if (gcComm_bHosting) gcComm_HostClose();
    else gcComm_ClientClose();
    gcComm_DiscoveryClose();
    stdComm_dword_8321DC = 0;
    stdComm_dword_8321E0 = 0;
    stdComm_bIsServer = 0;
    stdComm_dplayIdSelf = 0;
    gcComm_numSessions = 0;
}

void stdComm_Gc_Shutdown()
{
    if (gcComm_bHosting) gcComm_HostClose();
    else gcComm_ClientClose();
    gcComm_DiscoveryClose();
}

int DirectPlay_Receive(int *pIdOut, int *pMsgIdOut, int *pLenOut)
{
    Hack_ResetClients();
    if (stdComm_bIsServer)
        return gcComm_HostReceive(pIdOut, pMsgIdOut, pLenOut);
    return gcComm_ClientReceive(pIdOut, pMsgIdOut, pLenOut);
}

BOOL DirectPlay_Send(DPID idFrom, DPID idTo, void *lpData, DWORD dwDataSize)
{
    Hack_ResetClients();
    if (!lpData || !dwDataSize) return 0;
    if (stdComm_bIsServer)
        return gcComm_HostSend(idFrom, idTo, lpData, dwDataSize);
    if (!gcComm_server.bUsed) return 0;
    gcComm_SendMsg(&gcComm_server, GCPKT_DATA, idFrom, idTo, lpData, dwDataSize);
    return 1;
}

int stdComm_OpenConnection(void* a)
{
    if (!gcNet_IsReady()) return 1;
    stdComm_dword_8321DC = 1;
    return 0;
}

void stdComm_CloseConnection()
{
    if (stdComm_dword_8321DC) {
        if (stdComm_dword_8321E0) {
            DirectPlay_Close();
            stdComm_dword_8321E0 = 0;
            stdComm_bIsServer = 0;
            stdComm_dplayIdSelf = 0;
        }
        stdComm_dword_8321DC = 0;
    }
    gcComm_DiscoveryClose();
}

int stdComm_Open(int idx, char16_t* pwPassword)
{
    gcComm_DiscoveryClose();
    stdComm_dword_8321E8 = 0;
    stdComm_dword_8321E0 = 1;
    stdComm_bIsServer = 0;
    if (idx < 0 || idx >= gcComm_numSessions) return 1;
    jkGuiMultiplayer_checksumSeed = jkGuiMultiplayer_aEntries[idx].checksumSeed;
    if (!gcComm_ClientOpen(gcComm_aSessionIp[idx], (uint16_t)gcComm_aSessionPort[idx]))
        return 1;
    stdComm_dplayIdSelf = gcComm_selfId;
    return 0;
}

void stdComm_Close()
{
    if (gcComm_bHosting) gcComm_HostClose();
    else gcComm_ClientClose();
}

int DirectPlay_SendLobbyMessage(void* pPkt, uint32_t pktLen)
{
    return 1;
}

void DirectPlay_SetSessionDesc(const char* a1, DWORD maxPlayers)
{
    _strncpy(gcComm_hostEntry.mapJklFname, jkMain_aLevelJklFname, 0x1F);
}

BOOL DirectPlay_SetSessionFlagidk(int a1)
{
    return 1;
}

BOOL DirectPlay_Startup()
{
    jkGuiMultiplayer_numConnections = 0;
    memset(&jkGuiMultiplayer_aConnections, 0, 0x1180u);
    return 1;
}

int DirectPlay_EarlyInit(char16_t* pwIdk, char16_t* pwPlayerName)
{
    return 0;
}

DPID DirectPlay_CreatePlayer(char16_t* pwIdk, int idk2)
{
    return 1;
}

void DirectPlay_Close()
{
    stdComm_Close();
}

int DirectPlay_OpenHost(jkMultiEntry* pEntry)
{
    // The network may still be coming up (DHCP) when a game is hosted at boot.
    uint32_t start = stdPlatform_GetTimeMsec();
    while (gcNet_GetState() == GCNET_PROBING && stdPlatform_GetTimeMsec() - start < 20000)
        usleep(100 * 1000);
    if (!gcNet_IsReady()) {
        gcComm_Printf("can't host: no network\n");
        return 1;
    }
    gcComm_DiscoveryClose();
    gcComm_hostEntry = *pEntry;
    jkPlayer_maxPlayers = pEntry->maxPlayers;
    stdComm_bIsServer = 1;
    return gcComm_HostOpen() ? 0 : 1;
}

int DirectPlay_GetSession_passwordidk(jkMultiEntry* pEntry)
{
    gcComm_hostEntry = *pEntry;
    jkPlayer_maxPlayers = pEntry->maxPlayers;
    return 1;
}

int DirectPlay_EnumSessions2()
{
    gcComm_DiscoveryClose();
    return 0;
}

int stdComm_EnumSessions(int a, void* b)
{
    gcComm_DiscoveryOpen();
    gcComm_DiscoveryPump();
    memset(jkGuiMultiplayer_aEntries, 0, sizeof(jkMultiEntry) * 32);
    for (int i = 0; i < gcComm_numSessions; i++)
        jkGuiMultiplayer_aEntries[i] = gcComm_aSessions[i];
    dplay_dword_55D618 = gcComm_numSessions;
    return 0;
}

void DirectPlay_EnumPlayers(int a)
{
    Hack_ResetClients();
}

int DirectPlay_StartSession(void* a, void* b)
{
    return 1;
}

void DirectPlay_Destroy()
{
    stdComm_Gc_Shutdown();
}

int DirectPlay_IdkSessionDesc(jkMultiEntry* pEntry)
{
    return 1;
}
