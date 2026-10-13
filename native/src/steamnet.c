// steamnet: co-op over Steam P2P (Steam's relay network) instead of raw IP + port forwarding.
//
// Why not Unreal's USteamNetDriver: it is compiled in, but it needs the "STEAM" socket subsystem, and this game's Steam
// OSS never creates one (ISocketSubsystem::Get("STEAM") is null at runtime), so the engine would fall back to IP.
// Instead this is a UDP shim under the retail net driver (PacketRelayNetDriver/IpNetDriver + DTLS, unchanged):
//   - every Steam peer gets a fake IPv4 in 198.18.0.0/15 (RFC 2544 benchmarking range, never routed);
//   - ws2_32 sendto() to a fake address from the game goes out as ISteamNetworking::SendP2PPacket to that SteamID;
//   - ws2_32 recvfrom() on the game's socket (the listen socket on a host, the socket that talked to a fake address on
//     a client) first returns queued P2P packets, with the peer's fake address as the source;
//   - P2P session requests (callback 1202) are accepted while Steam P2P is enabled and the host's join policy allows
//     the remote SteamID (joinpolicy.c: by default only the host's Steam friends and its own account); everyone else
//     is refused before a single packet reaches the game (registered through presence.c). Steam authenticates the
//     remote SteamID of a P2P session, so on this path the policy is not spoofable.
// So a host takes Steam joins and (host_ip=1 only; by default its UDP socket is bound to 127.0.0.1, see h_bind) UDP
// joins at the same time, and `join steam:<id64>` is just `open <fake ip>:7777`:
// travel.c's follow/rejoin reopens the same fake address, which keeps mapping to the same SteamID.
// Steam's networking runs in steamclient (SDR relay, NAT punching); the game's socket never sees those packets.
// Findings, test plan: docs/investigations/steam-p2p.md.
//
// b4bcoop.ini: steam_p2p=1|0 (default 1): accept/allow Steam P2P. `transport=ip` is the same as steam_p2p=0.
// host_ip=1 (cmds.c): leave the game's UDP sockets on all interfaces (IP hosting and joining, advanced).
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "MinHook.h"
#include "ue.h"
#include "log.h"
#include "cmds.h"

#define P2P_CHANNEL     27          // our ISteamNetworking channel (the Steam OSS would use 0..; it has no sockets here)
#define CB_SESSION_REQ  1202        // P2PSessionRequest_t { CSteamID remote; }
#define CB_SESSION_FAIL 1203        // P2PSessionConnectFail_t { CSteamID remote; uint8 error; }
#define FAKE_NET        0xC6120000u // 198.18.0.0/15
#define FAKE_MASK       0xFFFE0000u
#define MAX_PEERS       64
#define MAX_PACKET      1200        // ISteamNetworking unreliable limit
// Every P2P datagram starts with an 8-byte header: magic "B4C1" (protocol v1) + the sending process's random tag. A
// packet with our own tag came back to us: that only happens when two copies share one Steam account (local tests),
// where Steam may deliver a packet for "our" SteamID to the sender itself. Anything without the magic is dropped.
// The tag also tells the host which client socket a packet comes from: a client sends g_tag + n for its n-th game
// socket to the host (every join attempt opens a new one), and the host reports such packets from a source port made
// from the tag. So each attempt is a new address on the host, like a new UDP source port over IP: a failed attempt's
// half-open connection there (a DTLS handshake that failed while the host was loading a mission waits 180 s for its
// timeout) can't swallow the next attempt's packets, which used to arrive from the same fake address (#41). Peers
// with older b4bcoop send one tag per process: one port, as before.
#define HDR_LEN         8
static const uint8_t HDR_MAGIC[4] = {'B', '4', 'C', '1'};
static uint32_t g_tag;
static uint32_t g_sock_gen;          // client: game sockets that talked to the joined host so far
#define OWN_TAG(t) ((uint32_t)((t) - g_tag) < 4096u)   // our own process (any of our sockets)
static uint16_t tag_port(uint32_t t) { return (uint16_t)(1024 + t % 60000u); }
static uint32_t g_self_drops, g_bad_drops, g_refused_drops;
enum { SEND_UNRELIABLE = 0, SEND_RELIABLE = 2 };

static int g_enabled = 1;           // steam_p2p ini key
static CRITICAL_SECTION cs;
static int g_ready;                  // hooks installed

// ---- steam_api64 (flat API of SDK 1.47; the game loads the DLL, we only look it up) ----
typedef struct {
    uint8_t active, connecting, error, relay;
    int32_t bytes_queued, packets_queued;
    uint32_t remote_ip;
    uint16_t remote_port;
} P2PState;                                                   // P2PSessionState_t
static struct {
    HMODULE dll;
    int32_t (*user)(void);
    void *(*find)(int32_t user, const char *version);
    uint64_t (*id)(void *self);
    uint8_t (*logged_on)(void *self);
    uint8_t (*send)(void *self, uint64_t to, const void *data, uint32_t n, int type, int channel);
    uint8_t (*avail)(void *self, uint32_t *size, int channel);
    uint8_t (*read)(void *self, void *dest, uint32_t cap, uint32_t *size, uint64_t *from, int channel);
    uint8_t (*accept)(void *self, uint64_t remote);
    uint8_t (*close)(void *self, uint64_t remote);
    uint8_t (*relay)(void *self, uint8_t allow);
    uint8_t (*state)(void *self, uint64_t remote, P2PState *st);
} S;
static char g_why[128];             // why Steam P2P is unavailable (last check)

#define RESOLVE(f, name) ((*(void **)&S.f = (void *)GetProcAddress(m, name)) != NULL)
static int steam_api(void) {
    if (S.dll) return 1;
    HMODULE m = GetModuleHandleA("steam_api64.dll");   // delay-loaded by the game's Steam OSS; never load it ourselves
    if (!m) return 0;
    if (!(RESOLVE(user, "SteamAPI_GetHSteamUser") && RESOLVE(find, "SteamInternal_FindOrCreateUserInterface") &&
          RESOLVE(id, "SteamAPI_ISteamUser_GetSteamID") && RESOLVE(logged_on, "SteamAPI_ISteamUser_BLoggedOn") &&
          RESOLVE(send, "SteamAPI_ISteamNetworking_SendP2PPacket") &&
          RESOLVE(avail, "SteamAPI_ISteamNetworking_IsP2PPacketAvailable") &&
          RESOLVE(read, "SteamAPI_ISteamNetworking_ReadP2PPacket") &&
          RESOLVE(accept, "SteamAPI_ISteamNetworking_AcceptP2PSessionWithUser") &&
          RESOLVE(close, "SteamAPI_ISteamNetworking_CloseP2PSessionWithUser") &&
          RESOLVE(relay, "SteamAPI_ISteamNetworking_AllowP2PPacketRelay") &&
          RESOLVE(state, "SteamAPI_ISteamNetworking_GetP2PSessionState"))) {
        LOG("steamnet: steam_api64 lacks an expected export; Steam P2P off");
        return 0;
    }
    S.dll = m;
    return 1;
}
static void *iface(const char *ver) { int32_t u = steam_api() ? S.user() : 0; return u ? S.find(u, ver) : NULL; }
// The versions the game itself requests; stable for the process once SteamAPI is up, so cached (recvfrom hot path).
static void *steam_user(void) { static void *p; return p ? p : (p = iface("SteamUser020")); }
static void *net(void) { static void *p; return p ? p : (p = iface("SteamNetworking006")); }

uint64_t steamnet_local_id(void) { void *u = steam_user(); return u ? S.id(u) : 0; }
const char *steamnet_last_error(void) { return g_why[0] ? g_why : "unknown"; }

// 1 if Steam P2P can carry a session from this process; otherwise 0 and the reason in steamnet_last_error().
static int steamnet_available(void) {
    const char *why = NULL;
    void *u;
    if (!g_enabled) why = "disabled (steam_p2p=0)";
    else if (!g_ready) why = "hooks not installed";
    else if (!steam_api()) why = "steam_api64.dll not loaded (Steam not running?)";
    else if (!(u = steam_user())) why = "no Steam user (SteamAPI not initialized)";
    else if (!S.logged_on(u)) why = "Steam user not logged on (Steam offline?)";
    else if (!net()) why = "no ISteamNetworking";
    snprintf(g_why, sizeof g_why, "%s", why ? why : "");
    return why == NULL;
}

// ---- peers: SteamID <-> fake address ----
typedef struct {
    uint64_t id;
    uint16_t port;          // source port reported on our client socket (= the port we last sent to it; host side: tag_port)
    uint16_t family;        // AF_INET or AF_INET6 (v4-mapped), as the game's socket uses
    uint32_t rx, tx;
    uint8_t accepted;
    int8_t policy;          // join policy for this SteamID: 0 not checked yet, 1 allowed, -1 refused (packets dropped)
} Peer;
static Peer peers[MAX_PEERS];
static int npeers;
static uint64_t g_join_id;          // client: the Steam host we joined

static uint32_t fake_ip(int i) { return FAKE_NET | (uint32_t)(i + 1); }                 // 198.18.0.1 ...
static int peer_of_ip(uint32_t ip) {                                                   // index or -1
    if ((ip & FAKE_MASK) != FAKE_NET) return -1;
    int i = (int)(ip & ~FAKE_MASK) - 1;
    return i >= 0 && i < npeers ? i : -1;
}
static int peer_index(uint64_t id, int create) {   // under cs
    for (int i = 0; i < npeers; i++) if (peers[i].id == id) return i;
    if (!create || npeers >= MAX_PEERS) return -1;
    memset(&peers[npeers], 0, sizeof peers[0]);
    peers[npeers].id = id;
    peers[npeers].port = (uint16_t)(20000 + npeers);
    peers[npeers].family = AF_INET;
    return npeers++;
}
static void fmt_ip(uint32_t ip, char *b, size_t n) { snprintf(b, n, "%u.%u.%u.%u", ip >> 24, (ip >> 16) & 255, (ip >> 8) & 255, ip & 255); }

// Destination IPv4 of a sockaddr (plain or v4-mapped v6), host order; 0 if none.
static uint32_t v4_of(const struct sockaddr *sa, int len, uint16_t *port, uint16_t *fam) {
    if (!sa) return 0;
    if (sa->sa_family == AF_INET && len >= (int)sizeof(struct sockaddr_in)) {
        const struct sockaddr_in *a = (const void *)sa;
        *port = ntohs(a->sin_port); *fam = AF_INET;
        return ntohl(a->sin_addr.s_addr);
    }
    if (sa->sa_family == AF_INET6 && len >= (int)sizeof(struct sockaddr_in6)) {
        const struct sockaddr_in6 *a = (const void *)sa;
        static const uint8_t mapped[12] = {0,0,0,0,0,0,0,0,0,0,0xff,0xff};
        if (memcmp(&a->sin6_addr, mapped, 12)) return 0;
        uint32_t ip; memcpy(&ip, (const uint8_t *)&a->sin6_addr + 12, 4);
        *port = ntohs(a->sin6_port); *fam = AF_INET6;
        return ntohl(ip);
    }
    return 0;
}
static int fill_addr(struct sockaddr *sa, int *len, uint32_t ip, uint16_t port, uint16_t fam) {
    if (!sa || !len) return 0;
    if (fam == AF_INET6 && *len >= (int)sizeof(struct sockaddr_in6)) {
        struct sockaddr_in6 a = {0};
        a.sin6_family = AF_INET6; a.sin6_port = htons(port);
        uint8_t *b = (uint8_t *)&a.sin6_addr; b[10] = b[11] = 0xff;
        uint32_t n = htonl(ip); memcpy(b + 12, &n, 4);
        memcpy(sa, &a, sizeof a); *len = sizeof a;
        return 1;
    }
    if (*len < (int)sizeof(struct sockaddr_in)) return 0;
    struct sockaddr_in a = {0};
    a.sin_family = AF_INET; a.sin_port = htons(port); a.sin_addr.s_addr = htonl(ip);
    memcpy(sa, &a, sizeof a); *len = sizeof a;
    return 1;
}

// ---- the game's sockets ----
// Host: the UDP socket bound to the game net driver's listen port (from the engine log line). Client: the socket that
// sent to a fake address. Bound ports are recorded by the bind() hook, so the hot path needs no syscalls.
#define MAX_SOCKS 32
static struct { SOCKET s; uint16_t port; uint16_t family; } socks[MAX_SOCKS];
static int nsocks;
static volatile int g_listen_port;
static SOCKET g_client_sock = INVALID_SOCKET;
static int is_game_socket(SOCKET s, uint16_t *fam) {   // under cs
    if (s == g_client_sock) { *fam = 0; return 1; }
    if (!g_listen_port) return 0;
    for (int i = 0; i < nsocks; i++)
        if (socks[i].s == s) { *fam = socks[i].family; return socks[i].port == g_listen_port; }
    return 0;
}

// ---- join policy (joinpolicy.c) per Steam peer ----
// The host we joined is always admitted (we opened that session). Anyone else is checked once per peer and cached;
// a refused peer's session is closed and its packets are dropped (read and discarded) in recvfrom.
static int peer_admitted(uint64_t id) {
    EnterCriticalSection(&cs);
    int i = peer_index(id, 0);
    int pol = i >= 0 ? peers[i].policy : 0;
    int joined = id == g_join_id;
    LeaveCriticalSection(&cs);
    if (joined) return 1;
    if (pol) return pol > 0;
    char why[96];
    int ok = joinpolicy_check(id, why, sizeof why);
    EnterCriticalSection(&cs);
    if ((i = peer_index(id, 1)) >= 0) peers[i].policy = ok ? 1 : -1;
    LeaveCriticalSection(&cs);
    if (!ok) {
        LOG("steamnet: refused Steam P2P from %llu (%s): %s", (unsigned long long)id, presence_persona(id), why);
        joinpolicy_notify_refused(id, why);
        void *n = net();
        if (n) S.close(n, id);
    }
    return ok;
}

// ---- ws2_32 hooks ----
typedef int (WSAAPI *sendto_t)(SOCKET, const char *, int, int, const struct sockaddr *, int);
typedef int (WSAAPI *recvfrom_t)(SOCKET, char *, int, int, struct sockaddr *, int *);
typedef int (WSAAPI *bind_t)(SOCKET, const struct sockaddr *, int);
typedef int (WSAAPI *closesocket_t)(SOCKET);
static sendto_t o_sendto;
static recvfrom_t o_recvfrom;
static bind_t o_bind;
static closesocket_t o_closesocket;
static uint32_t g_tx_fail, g_big;

static int WSAAPI h_sendto(SOCKET s, const char *buf, int len, int flags, const struct sockaddr *to, int tolen) {
    uint16_t port = 0, fam = 0;
    uint32_t ip = v4_of(to, tolen, &port, &fam);
    if (!ip || (ip & FAKE_MASK) != FAKE_NET) return o_sendto(s, buf, len, flags, to, tolen);
    EnterCriticalSection(&cs);
    int i = peer_of_ip(ip);
    uint64_t id = i >= 0 ? peers[i].id : 0;
    if (i >= 0) { peers[i].port = port; peers[i].family = fam; peers[i].tx++; }
    uint32_t tag = g_tag;
    if (i >= 0 && id == g_join_id) {   // client: this is the game's socket
        if (g_client_sock != s) g_sock_gen++;
        g_client_sock = s;
        tag = g_tag + g_sock_gen;
    }
    LeaveCriticalSection(&cs);
    void *n = id && g_enabled ? net() : NULL;
    if (!n || len < 0) { WSASetLastError(WSAEHOSTUNREACH); return SOCKET_ERROR; }
    static uint8_t pkt[65536 + HDR_LEN];                 // game thread only (the net driver's socket)
    if (len > 65536) len = 65536;
    memcpy(pkt, HDR_MAGIC, 4); memcpy(pkt + 4, &tag, 4); memcpy(pkt + HDR_LEN, buf, len);
    int type = len + HDR_LEN <= MAX_PACKET ? SEND_UNRELIABLE : SEND_RELIABLE;
    if (type == SEND_RELIABLE && g_big++ < 5) LOG("steamnet: %d-byte packet to %llu sent reliable (over %d)", len, (unsigned long long)id, MAX_PACKET);
    if (!S.send(n, id, pkt, (uint32_t)(len + HDR_LEN), type, P2P_CHANNEL) && g_tx_fail++ < 10)
        LOG("steamnet: SendP2PPacket to %llu failed (%d bytes)", (unsigned long long)id, len);
    return len;   // like UDP: a lost packet is not an error
}

static int WSAAPI h_recvfrom(SOCKET s, char *buf, int len, int flags, struct sockaddr *from, int *fromlen) {
    uint16_t fam = 0;
    EnterCriticalSection(&cs);
    int game = g_enabled && S.dll && is_game_socket(s, &fam);
    LeaveCriticalSection(&cs);
    void *n = game && !(flags & MSG_PEEK) ? net() : NULL;
    uint32_t size;
    while (n && S.avail(n, &size, P2P_CHANNEL)) {
        static uint8_t pkt[65536];
        uint64_t remote = 0;
        if (!S.read(n, pkt, sizeof pkt, &size, &remote, P2P_CHANNEL)) break;
        if (size < HDR_LEN || memcmp(pkt, HDR_MAGIC, 4)) { g_bad_drops++; continue; }
        uint32_t tag;
        memcpy(&tag, pkt + 4, 4);
        if (OWN_TAG(tag)) { g_self_drops++; continue; }
        size -= HDR_LEN;
        if (!peer_admitted(remote)) { g_refused_drops++; continue; }   // join policy: not from this SteamID
        EnterCriticalSection(&cs);
        int i = peer_index(remote, 1);
        Peer p = {0};
        if (i >= 0) { peers[i].rx++; if (fam) peers[i].family = fam; p = peers[i]; }
        int client = s == g_client_sock;
        LeaveCriticalSection(&cs);
        if (i < 0) continue;                                   // peer table full: drop
        // client socket: from the host's address as we know it (where we send); listen socket: the sender's own
        // socket as a port (see tag_port)
        uint16_t port = client ? p.port : tag_port(tag);
        int flen = fromlen ? *fromlen : 0;
        if (from && !fill_addr(from, fromlen, fake_ip(i), port, p.family)) { *fromlen = flen; continue; }
        int k = (int)size < len ? (int)size : len;
        memcpy(buf, pkt + HDR_LEN, k);
        if ((int)size > len) { WSASetLastError(WSAEMSGSIZE); return SOCKET_ERROR; }
        return k;
    }
    return o_recvfrom(s, buf, len, flags, from, fromlen);
}

// host_ip=0 (default, cmds.c): a wildcard bind (0.0.0.0 / ::) of a UDP socket by the game itself (the net driver's
// listen socket on a host, its connection socket on a client) is made on the loopback address instead. Nothing the
// game hosts is reachable from the network then (no Windows Firewall prompt, nothing exposed); Steam P2P needs no
// reachable socket (packets come through recvfrom above), and local test copies still join 127.0.0.1. Only binds
// called from the game exe: steamclient64.dll (in-process on Windows) binds its own UDP sockets for Steam's networking.
static uint32_t g_loopback_binds;
static int from_game_exe(void *ra) {
    HMODULE m = NULL;
    return ra && GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                    (LPCWSTR)ra, &m) && m == GetModuleHandleW(NULL);
}
static const struct sockaddr *loopback_bind(SOCKET s, const struct sockaddr *sa, int len, struct sockaddr_storage *lo) {
    if (!sa || coop_host_ip()) return sa;
    int type = 0, tl = sizeof type;
    if (getsockopt(s, SOL_SOCKET, SO_TYPE, (char *)&type, &tl) || type != SOCK_DGRAM) return sa;
    memset(lo, 0, sizeof *lo);
    if (sa->sa_family == AF_INET && len >= (int)sizeof(struct sockaddr_in)) {
        struct sockaddr_in *a = (struct sockaddr_in *)lo;
        *a = *(const struct sockaddr_in *)sa;
        if (a->sin_addr.s_addr != htonl(INADDR_ANY)) return sa;
        a->sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    } else if (sa->sa_family == AF_INET6 && len >= (int)sizeof(struct sockaddr_in6)) {
        struct sockaddr_in6 *a = (struct sockaddr_in6 *)lo;
        *a = *(const struct sockaddr_in6 *)sa;
        static const uint8_t any[16] = {0};
        if (memcmp(&a->sin6_addr, any, 16)) return sa;
        DWORD v6only = 1; int vl = sizeof v6only;
        getsockopt(s, IPPROTO_IPV6, IPV6_V6ONLY, (char *)&v6only, &vl);
        uint8_t *b = (uint8_t *)&a->sin6_addr;
        if (v6only) b[15] = 1;                                                        // ::1
        else { b[10] = b[11] = 0xff; b[12] = 127; b[15] = 1; }                        // ::ffff:127.0.0.1 (dual stack)
    } else return sa;
    return (const struct sockaddr *)lo;
}

static int WSAAPI h_bind(SOCKET s, const struct sockaddr *sa, int len) {
    struct sockaddr_storage lo;
    const struct sockaddr *want = from_game_exe(__builtin_return_address(0)) ? loopback_bind(s, sa, len, &lo) : sa;
    int r = o_bind(s, want, len);
    if (want != sa && g_loopback_binds++ < 20) {
        uint16_t p = ntohs(sa->sa_family == AF_INET ? ((const struct sockaddr_in *)sa)->sin_port : ((const struct sockaddr_in6 *)sa)->sin6_port);
        LOG("steamnet: game UDP socket bound to %s port %u instead of all interfaces (host_ip=0: not reachable from the "
            "network)%s", sa->sa_family == AF_INET ? "127.0.0.1" : "loopback", p, r ? " -- bind failed" : "");
    }
    if (r == 0 && sa && (sa->sa_family == AF_INET || sa->sa_family == AF_INET6)) {
        struct sockaddr_storage a; int al = sizeof a;
        uint16_t port = 0;
        if (!getsockname(s, (struct sockaddr *)&a, &al))
            port = ntohs(a.ss_family == AF_INET6 ? ((struct sockaddr_in6 *)&a)->sin6_port : ((struct sockaddr_in *)&a)->sin_port);
        EnterCriticalSection(&cs);
        int i = 0;
        for (; i < nsocks && socks[i].s != s; i++) {}
        if (i == nsocks && nsocks < MAX_SOCKS) nsocks++;
        if (i < nsocks) { socks[i].s = s; socks[i].port = port; socks[i].family = sa->sa_family; }
        LeaveCriticalSection(&cs);
    }
    return r;
}

static int WSAAPI h_closesocket(SOCKET s) {
    EnterCriticalSection(&cs);
    for (int i = 0; i < nsocks; i++) if (socks[i].s == s) { socks[i] = socks[--nsocks]; break; }
    if (s == g_client_sock) g_client_sock = INVALID_SOCKET;
    LeaveCriticalSection(&cs);
    return o_closesocket(s);
}

// ---- Steam callbacks (registered through presence.c on the OSS callback thread) ----
typedef struct { void **vtbl; uint8_t flags; int32_t id; } CallbackBase;   // steam_api CCallbackBase
static volatile LONG n_requests, n_fails;
static void req_run(void *self, void *param) {
    (void)self;
    if (!param) return;
    uint64_t remote = *(uint64_t *)param;
    InterlockedIncrement(&n_requests);
    void *n = net();
    if (!g_enabled || !n) { LOG("steamnet: P2P session request from %llu ignored (Steam P2P off)", (unsigned long long)remote); return; }
    if (!peer_admitted(remote)) return;   // not accepted: Steam drops the session; logged and noticed in peer_admitted
    EnterCriticalSection(&cs);
    int i = peer_index(remote, 1);
    if (i >= 0) peers[i].accepted = 1;
    LeaveCriticalSection(&cs);
    uint8_t ok = S.accept(n, remote);
    LOG("steamnet: P2P session request from %llu: %s", (unsigned long long)remote, ok ? "accepted" : "accept failed");
}
static volatile LONG g_join_failed;   // the host we joined never accepted our P2P session (shown by steamnet_tick)
static void fail_run(void *self, void *param) {
    (void)self;
    if (!param) return;
    InterlockedIncrement(&n_fails);
    uint64_t id = *(uint64_t *)param;
    LOG("steamnet: P2P session with %llu failed, EP2PSessionError %u (1 not running app, 2 no rights, 3 not logged in, 4 timeout)",
        (unsigned long long)id, ((uint8_t *)param)[8]);
    if (id == g_join_id) InterlockedExchange(&g_join_failed, 1);
}

// Game thread: a Steam join whose session was never accepted (the host is gone, or its join policy refused us; a
// refusal is silent at the Steam level, so say what the likely reasons are).
void steamnet_tick(float dt) {
    (void)dt;
    if (!g_join_failed || !InterlockedExchange(&g_join_failed, 0)) return;
    chat_local_soon("Could not reach the host over Steam. Is it still hosting? Hosts only accept their Steam "
                     "friends by default (b4bcoop.ini allow_joins / allow_steamids on the host).");
}
static int req_size(void *self) { (void)self; return 8; }
static int fail_size(void *self) { (void)self; return 16; }   // sizeof(P2PSessionConnectFail_t), pack 8
static void *req_vtbl[3] = { (void *)req_run, (void *)req_run, (void *)req_size };
static void *fail_vtbl[3] = { (void *)fail_run, (void *)fail_run, (void *)fail_size };
static CallbackBase req_cb = { req_vtbl, 0, 0 }, fail_cb = { fail_vtbl, 0, 0 };

// ---- engine log (uelog.c): the game net driver's listen port ----
void steamnet_on_log(const char *cat, const char *msg) {
    const char *p;
    if (!strcmp(cat, "LogNet") && strstr(msg, "GameNetDriver") && (p = strstr(msg, "listening on port "))) {
        g_listen_port = atoi(p + 18);
        LOG("steamnet: game listen port %d%s", g_listen_port, g_enabled ? "; Steam P2P packets go to that socket too" : "");
    }
}

int steamnet_listen_port(void) { return g_listen_port; }
// A Steam P2P peer that sent us game packets (a client of ours over Steam), not ourselves
int steamnet_peer_known(uint64_t id) {
    if (!id || !g_ready) return 0;
    EnterCriticalSection(&cs);
    int i = peer_index(id, 0), ok = i >= 0 && peers[i].rx > 0;
    LeaveCriticalSection(&cs);
    return ok;
}

// Host: the authenticated SteamID behind a fake P2P address ("198.18.0.1" or "198.18.0.1:port"), 0 if it is not one.
uint64_t steamnet_peer_of_addr(const char *addr) {
    unsigned a, b, c, d;
    if (!addr || sscanf(addr, "%u.%u.%u.%u", &a, &b, &c, &d) != 4 || a > 255 || b > 255 || c > 255 || d > 255) return 0;
    uint32_t ip = a << 24 | b << 16 | c << 8 | d;
    EnterCriticalSection(&cs);
    int i = peer_of_ip(ip);
    uint64_t id = i >= 0 ? peers[i].id : 0;
    LeaveCriticalSection(&cs);
    return id;
}

// ---- joins ----
// "steam:<id64>" (also "steam.<id64>"; an optional ":port" is ignored: Steam peers are addressed by id) ->
// url "<fake ip>:7777", returns 1; "host[:port]" -> "host:port" (default 7777), returns 0; -1 malformed;
// -2 Steam target but Steam P2P unavailable here (steamnet_last_error()).
int steamnet_resolve_target(const char *t, char *url, size_t n) {
    while (*t == ' ') t++;
    if (!_strnicmp(t, "steam:", 6) || !_strnicmp(t, "steam.", 6)) {
        char *end;
        unsigned long long id = strtoull(t + 6, &end, 10);
        if (id < 0x0110000100000000ull || (*end && *end != ':')) return -1;
        if (!steamnet_available()) {
            LOG("steamnet: cannot join steam:%llu: Steam P2P unavailable (%s)", id, g_why);
            return -2;
        }
        if (id == steamnet_local_id()) LOG("steamnet: joining our own SteamID (works only if Steam loops it back)");
        EnterCriticalSection(&cs);
        int i = peer_index(id, 1);
        g_join_id = id;
        g_join_failed = 0;
        g_client_sock = INVALID_SOCKET;
        LeaveCriticalSection(&cs);
        if (i < 0) { snprintf(g_why, sizeof g_why, "peer table full"); return -2; }
        char ip[20]; fmt_ip(fake_ip(i), ip, sizeof ip);
        snprintf(url, n, "%s:7777", ip);
        S.relay(net(), 1);   // allow SDR relay when NAT punching fails (Steam's default; set it anyway)
        LOG("steamnet: steam:%llu -> %s (Steam P2P channel %d)", id, url, P2P_CHANNEL);
        return 1;
    }
    if (!*t || strchr(t, ' ')) return -1;
    snprintf(url, n, "%s%s", t, strchr(t, ':') ? "" : ":7777");
    return 0;
}

// ---- status / command ----
int steamnet_p2p_on(void) { return steamnet_available(); }

#ifndef B4B_RELEASE
void steamnet_status(Out *o) {
    uint64_t id = steamnet_local_id();
    int ok = steamnet_available();
    out_printf(o, "steam: id=%llu p2p=%s", (unsigned long long)id, ok ? "on" : g_why);
    if (ok && id && ue_is_listen_server(ue_world())) out_printf(o, " (join me: steam:%llu)", (unsigned long long)id);
    out_printf(o, "\n");
}

static void print_peer(Out *o, const Peer *p, int i) {
    char ip[20]; fmt_ip(fake_ip(i), ip, sizeof ip);
    out_printf(o, "  peer %llu = %s%s: rx=%u tx=%u policy=%s", (unsigned long long)p->id, ip, p->id == g_join_id ? " (joined host)" : "",
               p->rx, p->tx, p->policy > 0 ? "allowed" : p->policy < 0 ? "REFUSED" : "-");
    void *n = S.dll ? net() : NULL;
    P2PState s = {0};
    if (n && S.state(n, p->id, &s)) {
        uint32_t r = s.remote_ip;
        out_printf(o, " session: active=%d connecting=%d relay=%d error=%d queued=%d remote=%u.%u.%u.%u:%u\n", s.active,
                   s.connecting, s.relay, s.error, s.packets_queued, r >> 24, (r >> 16) & 255, (r >> 8) & 255, r & 255, s.remote_port);
    } else out_printf(o, " session: none\n");
}

// `steamnet ping [max age s]`: Steam's relay network (SDR) state and ISteamNetworkingUtils::CheckPingDataUpToDate,
// which starts a new ping measurement to every relay POP when the data is older than max age (0 = now). Used to show
// that the ~30 UDP sockets on 0.0.0.0 seen in bursts are Steam's own relay pings (steam-p2p.md "Loopback binding").
typedef struct { int avail, ping_in_progress, avail_config, avail_any_relay; char msg[256]; } RelayStatus;
static void relay_ping(char *arg, Out *o) {
    void *u = NULL;
    const char *ver = NULL;
    static const char *vers[] = {"SteamNetworkingUtils004", "SteamNetworkingUtils003"};
    for (size_t i = 0; !u && i < sizeof vers / sizeof *vers; i++) if ((u = iface(vers[i]))) ver = vers[i];
    if (!u) { out_printf(o, "no ISteamNetworkingUtils (003/004)\n"); return; }
    // v003/v004 vtable (checked against Proton's lsteamclient thunks; InitRelayNetworkAccess is an inline SDK helper
    // calling CheckPingDataUpToDate): [0] AllocateMessage [1] GetRelayNetworkStatus [2] GetLocalPingLocation ...
    // [7] CheckPingDataUpToDate [8] GetPingToDataCenter [9] GetDirectPingToPOP [10] GetPOPCount
    void **vt = *(void ***)u;
    int (*status)(void *, RelayStatus *) = (int (*)(void *, RelayStatus *))vt[1];
    uint8_t (*uptodate)(void *, float) = (uint8_t (*)(void *, float))vt[7];
    int (*pops)(void *) = (int (*)(void *))vt[10];
    RelayStatus st = {0};
    int a = status(u, &st);
    out_printf(o, "%s: relay network %d (config %d, any relay %d), ping in progress=%d, POPs=%d: %.200s\n", ver, a,
               st.avail_config, st.avail_any_relay, st.ping_in_progress, pops(u), st.msg);
    if (arg) {
        float age = (float)atof(arg);
        uint8_t fresh = uptodate(u, age);
        LOG("steamnet: relay ping data %s (max age %.0fs)", fresh ? "up to date" : "stale: Steam started a new ping measurement", age);
        out_printf(o, "CheckPingDataUpToDate(%.0f) = %d%s\n", age, fresh, fresh ? "" : " (new ping measurement started)");
    }
}

// `steamnet [on|off|ping [age]]`: state, and the P2P session of every Steam peer
int steamnet_cmd(const char *verb, char *rest, Out *o) {
    if (strcmp(verb, "steamnet")) return 0;
    if (rest && !strncmp(rest, "ping", 4) && (!rest[4] || rest[4] == ' ')) {
        char *a = rest + 4;
        while (*a == ' ') a++;
        relay_ping(*a ? a : NULL, o);
        return 1;
    }
    if (rest && (!strcmp(rest, "on") || !strcmp(rest, "off"))) {
        g_enabled = !strcmp(rest, "on");
        out_printf(o, "Steam P2P %s\n", g_enabled ? "on" : "off");
        return 1;
    }
    steamnet_status(o);
    out_printf(o, "hooks=%s game listen port=%d client socket=%s session requests=%ld failures=%ld channel=%d "
               "dropped: own=%u bad=%u refused=%u loopback binds=%u\n", g_ready ? "yes" : "no", g_listen_port,
               g_client_sock != INVALID_SOCKET ? "yes" : "no", n_requests, n_fails, P2P_CHANNEL, g_self_drops, g_bad_drops,
               g_refused_drops, g_loopback_binds);
    EnterCriticalSection(&cs);
    static Peer copy[MAX_PEERS];
    int n = npeers;
    memcpy(copy, peers, sizeof copy);
    LeaveCriticalSection(&cs);
    for (int i = 0; i < n; i++) print_peer(o, &copy[i], i);
    if (!n) out_printf(o, "no Steam peers yet\n");
    return 1;
}
#endif  // !B4B_RELEASE

void steamnet_config(const char *key, const char *v) {
    if (!strcmp(key, "steam_p2p")) g_enabled = atoi(v) != 0;
    else if (!strcmp(key, "transport")) g_enabled = _stricmp(v, "ip") != 0;
    else return;
    LOG("steamnet: Steam P2P %s (ini %s=%s)", g_enabled ? "on" : "off", key, v);
}

// init_thread (MinHook initialized). The Steam callbacks are registered once presence.c has bound Steam.
void steamnet_init(void) {
    InitializeCriticalSection(&cs);
    LARGE_INTEGER t; QueryPerformanceCounter(&t);
    g_tag = (uint32_t)(t.QuadPart ^ (t.QuadPart >> 32)) ^ (GetCurrentProcessId() * 2654435761u);
    HMODULE ws = GetModuleHandleA("ws2_32.dll");
    struct { const char *name; void *detour; void **orig; } h[] = {
        {"sendto", (void *)h_sendto, (void **)&o_sendto}, {"recvfrom", (void *)h_recvfrom, (void **)&o_recvfrom},
        {"bind", (void *)h_bind, (void **)&o_bind}, {"closesocket", (void *)h_closesocket, (void **)&o_closesocket}};
    int ok = ws != NULL;
    for (size_t i = 0; ok && i < sizeof h / sizeof *h; i++) {
        void *t = (void *)GetProcAddress(ws, h[i].name);
        ok = t && MH_CreateHook(t, h[i].detour, h[i].orig) == MH_OK && MH_EnableHook(t) == MH_OK;
        if (!ok) LOG("steamnet: hook %s failed", h[i].name);
    }
    g_ready = ok;
    presence_add_callback(&req_cb, CB_SESSION_REQ);
    presence_add_callback(&fail_cb, CB_SESSION_FAIL);
    LOG("steamnet: %s, Steam P2P %s", ok ? "ws2_32 sendto/recvfrom/bind/closesocket hooked" : "disabled (hooks)", g_enabled ? "on" : "off");
}
