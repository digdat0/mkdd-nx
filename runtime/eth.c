/* Broadband Adapter emulation at the ETH API level, as memcard.c does the memory card at CARD's.
 * The game's whole network stack (IP / ARP / DHCP / AutoIP / UDP / SSDP / HTTP, BBAMgr,
 * NetGameMgr's LAN play) is the recompiled original running on top of this; only the driver
 * under it, which talks to the adapter chip over EXI (channel 0, device 2), is replaced.
 *
 * Off (g_lan_on 0, the default) every ETH call runs the original, whose ETHInit finds no adapter
 * and returns -1, as on a GameCube without one. On, the adapter is there with its link up:
 *   EXIGetID(0, 2)             the adapter's ID, EXI_ETHER (BBAMgr's EXIGetType at boot, which
 *                              puts LAN MODE on the title menu, and ETHInit); other devices: original.
 *                              Not under RTGP: its LAN entry runs code it loads from the disc
 *                              (files/DATA), which the recompiled build doesn't have
 *   ETHInit                    ETH_OK; the MAC address is 00:09:BF (Nintendo's) + 3 random bytes;
 *                              the tunnel's socket and receive thread start (once)
 *   ETHGetMACAddr              that address
 *   ETHGetLinkStateAsync       *status = TRUE (link up), returns TRUE (the answer is ready)
 *   ETHSetRecvCallback         cb0(type, length, status) -> buffer, cb1(buffer, length): kept
 *   ETHSetProtoType            the Ethernet types to deliver (none: all): kept
 *   ETHAdd/ClearMulticast...   logged (every frame reaches the game; the stack filters)
 *   ETHSendAsync               the frame goes through the tunnel (below), and its callback runs at
 *                              the next interrupt delivery (IRQ_NET), through __ETHPostSend as the
 *                              adapter's interrupt handler does, with status 0 (sent, no errors)
 * The callbacks run where the adapter's interrupt handler would: in rt_deliver (sched.c).
 *
 * The tunnel: every frame is one UDP datagram (Hdr + the frame) to port g_lan_port on the other
 * consoles running this port. A frame for one MAC address goes to the console that address was
 * last heard from; any other (broadcast, multicast, an address not heard yet) goes to the
 * network's broadcast address, to g_lan_peer and to every console heard. A receive thread takes
 * the datagrams, drops the ones from another game or version (Hdr.fp: the main.dol and the tunnel
 * version), from itself and repeats (Hdr.seq), and queues the frames; the interrupt hands each to
 * the game as the adapter's receive interrupt does: cb0 for a buffer, the frame and its FCS copied
 * there, __ETHFilter, cb1. Nothing answers DHCP on a network of consoles, so the adapter does (dhcp_reply):
 * each console gets 10.77.x.y from its MAC address, at once, instead of AutoIP after 30 s. */
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#define close_sock closesocket
typedef SOCKET Sock;
#define BAD_SOCK INVALID_SOCKET
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#define close_sock close
typedef int Sock;
#define BAD_SOCK (-1)
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "rt.h"
#include "table.h"

volatile uint32_t g_lan_on, g_lan_play;
uint32_t g_lan_port;
char g_lan_peer[64];

#define ETH_OK 1
#define ETH_PORT 51900        /* the tunnel's UDP port */
#define TUNNEL_VERSION 1      /* Hdr's layout and what the adapter does: both consoles must match */
#define SEND_MAX 16           /* sends waiting for their callback (the IP stack keeps one in flight) */
#define RX_MAX 1024           /* frames received, waiting for the interrupt: over 15 s of race traffic, so a stall
                               * (an emulator paused, a slow load) loses none, and the game gives up on its own */
#define FRAME_MAX 1536
#define PEER_MAX 8
#define LOG_FRAMES 300        /* frames logged one by one (each way); then a summary every 10 s */

typedef struct {
    char magic[4];            /* "MKLN" */
    uint8_t version, pad[3];  /* TUNNEL_VERSION */
    uint8_t fp[4];            /* fingerprint (big-endian) */
    uint8_t seq[4];           /* the sender's datagram count (big-endian) */
} Hdr;

static uint8_t s_mac[6];
static uint32_t s_cb0, s_cb1;          /* receive callbacks (guest addresses) */
static uint32_t s_types, s_ntypes;     /* ETHSetProtoType's u16 array and count */
static uint32_t s_post_send, s_filter; /* __ETHPostSend, __ETHFilter */
static struct { uint32_t cb, addr; } s_sent[SEND_MAX];
static volatile uint32_t s_nsent;
static PMutex s_lock;                  /* s_sent, s_rx, s_peers, s_stat */

static struct { uint16_t len; uint8_t data[FRAME_MAX]; } s_rx[RX_MAX];
static uint32_t s_rx_head, s_rx_n;

static Sock s_sock = BAD_SOCK;
static uint32_t s_fp, s_seq;
static struct sockaddr_in s_bcast, s_bcast_net, s_peer; /* s_bcast_net, s_peer: sin_port 0: none */
typedef struct {
    uint8_t mac[6];
    struct sockaddr_in addr;
    uint32_t seq;           /* the highest seq taken */
    uint64_t window;        /* bit k: seq - k taken */
    bool logged;
} Peer;
static Peer s_peers[PEER_MAX];
static uint32_t s_npeers, s_peers_logged;
static struct {
    uint64_t out, in, bytes_out, bytes_in, repeats, other_game, own, full, unwanted, no_buffer, send_err;
    char other_from[24];
} s_stat;
static uint64_t s_logged_out, s_logged_in, s_next_summary;

static const char *ip_str(const uint8_t *p, char *out) {
    snprintf(out, 16, "%u.%u.%u.%u", p[0], p[1], p[2], p[3]);
    return out;
}

static const char *addr_str(const struct sockaddr_in *a, char *out) {
    uint32_t ip = ntohl(a->sin_addr.s_addr);
    snprintf(out, 24, "%u.%u.%u.%u:%u", ip >> 24, ip >> 16 & 255, ip >> 8 & 255, ip & 255, ntohs(a->sin_port));
    return out;
}

static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }
static void put32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 24), p[1] = (uint8_t)(v >> 16), p[2] = (uint8_t)(v >> 8), p[3] = (uint8_t)v; }

/* One line per frame: addresses, type and what's in it (ARP, IP / UDP / TCP / IGMP / ICMP). */
static void log_frame(const char *dir, const uint8_t *f, uint32_t len) {
    char line[640], a[16], b[16];
    int n = snprintf(line, sizeof line, "eth %s %u: %02x:%02x:%02x:%02x:%02x:%02x <- %02x:%02x:%02x:%02x:%02x:%02x",
                     dir, len, f[0], f[1], f[2], f[3], f[4], f[5], f[6], f[7], f[8], f[9], f[10], f[11]);
    uint16_t type = (uint16_t)(f[12] << 8 | f[13]);
    const uint8_t *p = f + 14;
    if (type == 0x0806 && len >= 42) {
        uint16_t op = (uint16_t)(p[6] << 8 | p[7]);
        if (op == 2) n += snprintf(line + n, sizeof line - n, " ARP %s is-at (sender)", ip_str(p + 14, a));
        else n += snprintf(line + n, sizeof line - n, " ARP op %u: who has %s, tell %s", op, ip_str(p + 24, a), ip_str(p + 14, b));
    } else if (type == 0x0800 && len >= 34) {
        unsigned hl = (p[0] & 15) * 4, proto = p[9];
        n += snprintf(line + n, sizeof line - n, " IP %s -> %s", ip_str(p + 12, a), ip_str(p + 16, b));
        const uint8_t *q = p + hl;
        if (proto == 17 && 14 + hl + 8 <= len) {
            n += snprintf(line + n, sizeof line - n, " UDP %u -> %u len %u", q[0] << 8 | q[1], q[2] << 8 | q[3], q[4] << 8 | q[5]);
            /* SSDP (UPnP discovery, port 1900): its first line and NT / ST, which say what it is */
            if ((q[0] << 8 | q[1]) == 1900 || (q[2] << 8 | q[3]) == 1900) {
                const char *t = (const char *)q + 8, *end = (const char *)f + len;
                const char *keys[] = {"", "\nNT:", "\nST:", "\nNTS:"};
                for (int k = 0; k < 4 && n < (int)sizeof line - 2; k++) {
                    const char *s = t;
                    if (k) {
                        size_t kl = strlen(keys[k]);
                        for (s = t; s + kl <= end && strncmp(s, keys[k], kl); s++) {}
                        if (s + kl > end) continue;
                        s++;
                    }
                    const char *e = s;
                    while (e < end && *e != '\r' && *e != '\n' && e - s < 80) e++;
                    n += snprintf(line + n, sizeof line - n, " | %.*s", (int)(e - s), s);
                }
            }
        }
        else if (proto == 6 && 14 + hl + 14 <= len)
            n += snprintf(line + n, sizeof line - n, " TCP %u -> %u flags %#x", q[0] << 8 | q[1], q[2] << 8 | q[3], q[13]);
        else
            n += snprintf(line + n, sizeof line - n, " proto %u", proto);
    } else {
        n += snprintf(line + n, sizeof line - n, " type %#06x", type);
    }
    rt_log("%s", line);
}

static void mac_init(void) {
    if (s_mac[0] || s_mac[1] || s_mac[2]) return;
    uint64_t r = plat_now_ns() * 6364136223846793005ull + 1442695040888963407ull;
    s_mac[0] = 0x00; s_mac[1] = 0x09; s_mac[2] = 0xBF;
    s_mac[3] = (uint8_t)(r >> 40); s_mac[4] = (uint8_t)(r >> 32); s_mac[5] = (uint8_t)(r >> 24);
}

/* A frame for the game, as if the adapter had received it (s_lock held). */
static void rx_push(const uint8_t *f, uint32_t len) {
    if (s_rx_n == RX_MAX) { s_stat.full++; return; }
    uint32_t at = (s_rx_head + s_rx_n++) % RX_MAX;
    s_rx[at].len = (uint16_t)len;
    memcpy(s_rx[at].data, f, len);
}

/* ---- the local DHCP server ---- */

#define DHCP_NET 0x0A4D0000u      /* 10.77.0.0/16 */
#define DHCP_SERVER (DHCP_NET | 1)
static const uint8_t SERVER_MAC[6] = {0x02, 0x00, 0x00, 0x4D, 0x4B, 0x01}; /* locally administered */

static uint16_t ip_checksum(const uint8_t *p, uint32_t n) {
    uint32_t s = 0;
    for (uint32_t i = 0; i + 1 < n; i += 2) s += (uint32_t)(p[i] << 8 | p[i + 1]);
    if (n & 1) s += (uint32_t)p[n - 1] << 8;
    while (s >> 16) s = (s & 0xffff) + (s >> 16);
    return (uint16_t)~s;
}

/* The address a MAC address gets: 10.77.<its last two bytes>, never the network's, the server's
 * or the broadcast address. */
static uint32_t dhcp_addr(const uint8_t *mac) {
    uint32_t host = (uint32_t)mac[4] << 8 | mac[5];
    if (host < 2) host += 2;
    if (host == 0xffff) host = 0xfffe;
    return DHCP_NET | host;
}

/* The game's DHCP DISCOVER or REQUEST (f, len): the OFFER or ACK queued for it. false: not DHCP
 * from a client (the frame goes out as any other). */
static bool dhcp_reply(const uint8_t *f, uint32_t len) {
    if (len < 14 + 20 + 8 + 240 || f[12] != 0x08 || f[13] != 0x00) return false;
    const uint8_t *ip = f + 14;
    unsigned hl = (ip[0] & 15) * 4;
    if (ip[9] != 17 || 14 + hl + 8 + 240 > len) return false;
    const uint8_t *udp = ip + hl, *bp = udp + 8;
    if ((udp[0] << 8 | udp[1]) != 68 || (udp[2] << 8 | udp[3]) != 67 || bp[0] != 1 || be32(bp + 236) != 0x63825363u) return false;
    int type = 0;
    for (const uint8_t *o = bp + 240, *end = f + len; o < end && *o != 255;) {
        if (*o == 0) { o++; continue; }
        if (o + 2 > end || o + 2 + o[1] > end) break;
        if (o[0] == 53 && o[1] >= 1) type = o[2];
        o += 2 + o[1];
    }
    if (type != 1 && type != 3) return true; /* RELEASE, DECLINE, INFORM: nothing to answer, nowhere to go */
    uint8_t r[14 + 20 + 8 + 240 + 32] = {0};
    uint8_t *rip = r + 14, *rudp = rip + 20, *rbp = rudp + 8, *o = rbp + 240;
    uint32_t yi = dhcp_addr(f + 6);
    *o++ = 53, *o++ = 1, *o++ = type == 1 ? 2 : 5; /* OFFER / ACK */
    *o++ = 54, *o++ = 4, put32(o, DHCP_SERVER), o += 4;
    *o++ = 51, *o++ = 4, put32(o, 30 * 86400), o += 4; /* lease: 30 days */
    *o++ = 1, *o++ = 4, put32(o, 0xffff0000u), o += 4;
    *o++ = 255;
    uint32_t blen = (uint32_t)(o - rbp), total = 14 + 20 + 8 + blen;
    memcpy(r, f + 6, 6);
    memcpy(r + 6, SERVER_MAC, 6);
    r[12] = 0x08;
    rbp[0] = 2, rbp[1] = 1, rbp[2] = 6;
    memcpy(rbp + 4, bp + 4, 4);    /* xid */
    memcpy(rbp + 10, bp + 10, 2);  /* flags */
    put32(rbp + 16, yi);
    memcpy(rbp + 28, bp + 28, 16); /* chaddr */
    put32(rbp + 236, 0x63825363u);
    rudp[1] = 67, rudp[3] = 68;
    rudp[4] = (uint8_t)((8 + blen) >> 8), rudp[5] = (uint8_t)(8 + blen); /* UDP checksum 0: none */
    rip[0] = 0x45, rip[8] = 64, rip[9] = 17;
    rip[2] = (uint8_t)((20 + 8 + blen) >> 8), rip[3] = (uint8_t)(20 + 8 + blen);
    put32(rip + 12, DHCP_SERVER);
    put32(rip + 16, 0xffffffffu);
    uint16_t sum = ip_checksum(rip, 20);
    rip[10] = (uint8_t)(sum >> 8), rip[11] = (uint8_t)sum;
    pmutex_lock(&s_lock);
    rx_push(r, total < 60 ? 60 : total);
    pmutex_unlock(&s_lock);
    rt_log("eth: DHCP %s: %u.%u.%u.%u (local server)", type == 1 ? "DISCOVER, OFFER" : "REQUEST, ACK", yi >> 24, yi >> 16 & 255,
           yi >> 8 & 255, yi & 255);
    return true;
}

/* ---- the tunnel ---- */

static bool parse_peer(const char *s, struct sockaddr_in *out) {
    unsigned a, b, c, d, port = 0;
    int n = sscanf(s, "%u.%u.%u.%u:%u", &a, &b, &c, &d, &port);
    if (n < 4 || a > 255 || b > 255 || c > 255 || d > 255 || port > 65535) return false;
    memset(out, 0, sizeof *out);
    out->sin_family = AF_INET;
    out->sin_addr.s_addr = htonl(a << 24 | b << 16 | c << 8 | d);
    out->sin_port = htons((uint16_t)(port ? port : ETH_PORT));
    return true;
}

static Peer *peer_of(const uint8_t *mac) {
    for (uint32_t i = 0; i < s_npeers; i++)
        if (!memcmp(s_peers[i].mac, mac, 6)) return &s_peers[i];
    return NULL;
}

static void send_to(const uint8_t *dgram, uint32_t n, const struct sockaddr_in *to) {
    if (sendto(s_sock, (const char *)dgram, (int)n, 0, (const struct sockaddr *)to, sizeof *to) >= 0) return;
#ifdef _WIN32
    int e = WSAGetLastError();
#else
    int e = errno;
#endif
    char a[24];
    if (s_stat.send_err++ < 8) rt_log("eth: send to %s failed (error %d)", addr_str(to, a), e); /* the first few */
}

static void tunnel_send(const uint8_t *f, uint32_t len) {
    if (s_sock == BAD_SOCK) return;
    uint8_t d[sizeof(Hdr) + FRAME_MAX];
    Hdr *h = (Hdr *)d;
    memcpy(h->magic, "MKLN", 4);
    h->version = TUNNEL_VERSION;
    memset(h->pad, 0, sizeof h->pad);
    put32(h->fp, s_fp);
    memcpy(d + sizeof(Hdr), f, len);
    uint32_t n = sizeof(Hdr) + len;
    pmutex_lock(&s_lock);
    put32(h->seq, ++s_seq);
    struct sockaddr_in to[PEER_MAX + 3];
    int nto = 0;
    Peer *p = f[0] & 1 ? NULL : peer_of(f);
    if (p) { /* twice: Wi-Fi loses datagrams, the game's lockstep stops at a lost one; the other copy is a repeat */
        to[nto++] = p->addr;
        to[nto++] = p->addr;
    } else {
        to[nto++] = s_bcast;
        if (s_bcast_net.sin_port) to[nto++] = s_bcast_net;
        if (s_peer.sin_port) to[nto++] = s_peer;
        for (uint32_t i = 0; i < s_npeers; i++)
            if (!s_peer.sin_port || s_peers[i].addr.sin_addr.s_addr != s_peer.sin_addr.s_addr || s_peers[i].addr.sin_port != s_peer.sin_port)
                to[nto++] = s_peers[i].addr;
    }
    s_stat.out++;
    s_stat.bytes_out += len;
    pmutex_unlock(&s_lock);
    for (int i = 0; i < nto; i++) send_to(d, n, &to[i]);
}

/* ---- link quality: pings between the consoles ----
 * The receive thread pings every console it knows 5 times a second (and, once a second, the
 * broadcast addresses and LAN PEER, which finds consoles before the game sends anything); each
 * answers at once with the time it was sent. Round trip and the pings unanswered per 5 s give
 * eth_status (the - menu, the FPS counter) and the 10 s summary. "MKPI" / "MKPO" datagrams: a
 * build without them drops them (not "MKLN"), so it just never answers. */
#define PING_NS 200000000ull
#define LINK_WINDOW_NS 5000000000ull
#define LINK_GONE_NS 6000000000ull
typedef struct {
    struct sockaddr_in addr;
    uint64_t heard;          /* its last ping or pong */
    uint64_t rtt, rtt_max;   /* smoothed round trip, the most this window (ns) */
    uint32_t sent, got;      /* pings this window, answered */
    uint32_t last_sent, last_got, loss_pct; /* the last whole window */
    bool answered;           /* ever: a build that answers pings */
} Link;
static Link s_links[PEER_MAX];
static uint32_t s_nlinks;
static uint64_t s_window_end, s_tunnel_since;

typedef struct {
    char magic[4];
    uint8_t version, pad[3];
    uint8_t fp[4];
    uint8_t who[4];          /* the sender's s_self (its own broadcast pings come back to it) */
    uint8_t t[8];            /* the pinger's plat_now_ns, echoed back */
} Ping;
static uint32_t s_self;      /* this console's ping id, random */

static Link *link_of(const struct sockaddr_in *a, bool add, uint64_t now) {
    for (uint32_t i = 0; i < s_nlinks; i++)
        if (s_links[i].addr.sin_addr.s_addr == a->sin_addr.s_addr && s_links[i].addr.sin_port == a->sin_port) return &s_links[i];
    if (!add || s_nlinks == PEER_MAX) return NULL;
    Link *l = &s_links[s_nlinks++];
    memset(l, 0, sizeof *l);
    l->addr = *a;
    l->heard = now;
    l->loss_pct = ~0u;
    char s[24];
    rt_log("eth: link to %s (console %u)", addr_str(a, s), s_nlinks);
    return l;
}

static void ping_send(const char *magic, uint64_t t, const struct sockaddr_in *to) {
    Ping p;
    memcpy(p.magic, magic, 4);
    p.version = TUNNEL_VERSION;
    memset(p.pad, 0, sizeof p.pad);
    put32(p.fp, s_fp);
    put32(p.who, s_self);
    put32(p.t, (uint32_t)(t >> 32));
    put32(p.t + 4, (uint32_t)t);
    send_to((const uint8_t *)&p, sizeof p, to);
}

/* A datagram from (from): the frame queued for the game, or why not (s_stat). */
static void tunnel_take(const uint8_t *d, uint32_t n, const struct sockaddr_in *from) {
    const Hdr *h = (const Hdr *)d;
    if (n < sizeof(Hdr) + 14 || n > sizeof(Hdr) + FRAME_MAX || memcmp(h->magic, "MKLN", 4)) return;
    const uint8_t *f = d + sizeof(Hdr);
    uint32_t len = n - sizeof(Hdr), seq = be32(h->seq);
    pmutex_lock(&s_lock);
    if (h->version != TUNNEL_VERSION || be32(h->fp) != s_fp) {
        if (!s_stat.other_game++) addr_str(from, s_stat.other_from);
    } else if (!memcmp(f + 6, s_mac, 6)) {
        s_stat.own++; /* our own broadcast, back */
    } else {
        Peer *p = peer_of(f + 6);
        if (!p && s_npeers < PEER_MAX) {
            p = &s_peers[s_npeers++];
            memset(p, 0, sizeof *p);
            memcpy(p->mac, f + 6, 6);
            p->seq = seq - 1;
        }
        if (!p) p = &s_peers[(seq ^ f[11]) % PEER_MAX]; /* full: one is forgotten */
        if (memcmp(p->mac, f + 6, 6)) memset(p, 0, sizeof *p), memcpy(p->mac, f + 6, 6), p->seq = seq - 1;
        p->addr = *from; /* where it is now (a console that came back on another address) */
        Link *l = link_of(from, true, plat_now_ns());
        if (l) l->heard = plat_now_ns();
        int32_t ahead = (int32_t)(seq - p->seq);
        bool take = true;
        if (ahead > 0) {
            p->window = ahead >= 64 ? 1 : p->window << ahead | 1;
            p->seq = seq;
        } else if (ahead > -64) {
            if (p->window >> -ahead & 1) take = false;
            else p->window |= 1ull << -ahead;
        } else if (ahead < -1000) { /* the other console started again: its count too */
            p->seq = seq, p->window = 1;
        } else {
            take = false;
        }
        if (!take) s_stat.repeats++;
        else {
            s_stat.in++;
            s_stat.bytes_in += len;
            rx_push(f, len);
        }
    }
    pmutex_unlock(&s_lock);
    sched_raise(IRQ_NET);
}

/* A ping or pong from (from); false: neither (a tunnel datagram). */
static bool ping_take(const uint8_t *d, uint32_t n, const struct sockaddr_in *from) {
    const Ping *p = (const Ping *)d;
    bool ping = n == sizeof(Ping) && !memcmp(p->magic, "MKPI", 4), pong = n == sizeof(Ping) && !memcmp(p->magic, "MKPO", 4);
    if (!ping && !pong) return false;
    if (p->version != TUNNEL_VERSION || be32(p->fp) != s_fp || be32(p->who) == s_self) return true;
    uint64_t now = plat_now_ns(), t = (uint64_t)be32(p->t) << 32 | be32(p->t + 4);
    if (ping) {
        ping_send("MKPO", t, from);
        pmutex_lock(&s_lock);
        Link *l = link_of(from, true, now);
        if (l) l->heard = now;
        pmutex_unlock(&s_lock);
        return true;
    }
    if (t > now || now - t > 5000000000ull) return true; /* not one of ours */
    pmutex_lock(&s_lock);
    Link *l = link_of(from, true, now);
    if (l) {
        uint64_t rtt = now - t;
        l->heard = now;
        l->rtt = l->answered ? (l->rtt * 7 + rtt) / 8 : rtt;
        if (rtt > l->rtt_max) l->rtt_max = rtt;
        l->got++;
        l->answered = true;
    }
    pmutex_unlock(&s_lock);
    return true;
}

/* The receive thread, every PING_NS: the pings, the window's loss, consoles gone. */
static void ping_tick(uint64_t now) {
    static uint64_t next, next_bcast;
    if (now < next) return;
    next = now + PING_NS;
    struct sockaddr_in to[PEER_MAX + 3];
    int nto = 0;
    pmutex_lock(&s_lock);
    if (now >= s_window_end) {
        for (uint32_t i = 0; i < s_nlinks; i++) {
            Link *l = &s_links[i];
            l->last_sent = l->sent, l->last_got = l->got > l->sent ? l->sent : l->got;
            if (l->sent) l->loss_pct = (l->last_sent - l->last_got) * 100 / l->last_sent;
            l->sent = l->got = 0;
            l->rtt_max = 0;
        }
        s_window_end = now + LINK_WINDOW_NS;
    }
    for (uint32_t i = 0; i < s_nlinks;) {
        if (now - s_links[i].heard > LINK_GONE_NS) {
            char a[24];
            rt_log("eth: link to %s lost (nothing for %u s)", addr_str(&s_links[i].addr, a), (unsigned)(LINK_GONE_NS / 1000000000ull));
            s_links[i] = s_links[--s_nlinks];
            continue;
        }
        s_links[i].sent++;
        to[nto++] = s_links[i++].addr;
    }
    pmutex_unlock(&s_lock);
    for (int i = 0; i < nto; i++) ping_send("MKPI", now, &to[i]);
    if (now >= next_bcast) {
        next_bcast = now + 1000000000ull;
        ping_send("MKPI", now, &s_bcast);
        if (s_bcast_net.sin_port) ping_send("MKPI", now, &s_bcast_net);
        if (s_peer.sin_port) ping_send("MKPI", now, &s_peer);
    }
}

void eth_status(EthStatus *st) {
    memset(st, 0, sizeof *st);
    st->loss_pct = -1;
    if (s_sock == BAD_SOCK) return;
    st->up = true;
    pmutex_lock(&s_lock);
    st->seconds = (uint32_t)((plat_now_ns() - s_tunnel_since) / 1000000000ull);
    for (uint32_t i = 0; i < s_nlinks; i++) {
        const Link *l = &s_links[i];
        st->consoles++;
        if (!l->answered) { st->silent++; continue; }
        uint32_t ms = (uint32_t)((l->rtt + 500000) / 1000000), mx = (uint32_t)((l->rtt_max + 500000) / 1000000);
        if (ms > st->rtt_ms) st->rtt_ms = ms;
        if (mx > st->rtt_max_ms) st->rtt_max_ms = mx;
        if (l->loss_pct <= 100 && (int)l->loss_pct > st->loss_pct) st->loss_pct = (int)l->loss_pct;
    }
    pmutex_unlock(&s_lock);
}

static void rx_thread(void *arg) {
    (void)arg;
    static uint8_t d[sizeof(Hdr) + FRAME_MAX + 64];
    unsigned errors = 0;
    for (;;) {
        ping_tick(plat_now_ns());
        struct sockaddr_in from;
        socklen_t fl = sizeof from;
        int n = (int)recvfrom(s_sock, (char *)d, sizeof d, 0, (struct sockaddr *)&from, &fl);
        if (n < 0) {
#ifdef _WIN32
            bool none = WSAGetLastError() == WSAEWOULDBLOCK;
#else
            /* ECONNREFUSED: a console we sent to has quit (an ICMP port unreachable came back) */
            bool none = errno == EWOULDBLOCK || errno == EAGAIN || errno == ECONNREFUSED;
#endif
            if (none) { plat_sleep_ns(1000000); continue; } /* nothing yet: look again in 1 ms */
            if (++errors % 100 == 1) rt_log("eth: receive error (%u)", errors);
            plat_sleep_ns(20000000); /* a network gone (Wi-Fi off): don't spin */
            continue;
        }
        if (!ping_take(d, (uint32_t)n, &from)) tunnel_take(d, (uint32_t)n, &from);
    }
}

/* The socket and its thread (once): no network, no tunnel (the adapter still works, alone). */
static void tunnel_start(void) {
    static bool tried;
    if (tried) return;
    tried = true;
    s_fp = (uint32_t)fnv_bytes(fnv_bytes(FNV_INIT, g_dol_sha1, strlen(g_dol_sha1)), &(uint32_t){TUNNEL_VERSION}, 4);
    s_seq = (uint32_t)(plat_now_ns() >> 10);
    s_self = (uint32_t)(plat_now_ns() * 2654435761u) | 1;
    s_tunnel_since = plat_now_ns();
#ifdef _WIN32
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa)) { rt_log("eth: no sockets (WSAStartup): no tunnel"); return; }
#endif
    if (!plat_net_up()) { rt_log("eth: no sockets: no tunnel"); return; }
    s_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s_sock == BAD_SOCK) { rt_log("eth: socket failed: no tunnel"); plat_net_down(); return; }
    int one = 1;
    setsockopt(s_sock, SOL_SOCKET, SO_BROADCAST, (const char *)&one, sizeof one);
#ifdef _WIN32
    /* Windows fails the next recvfrom (WSAECONNRESET) after a datagram to a port nobody listens on */
    BOOL no = FALSE;
    DWORD got;
    WSAIoctl(s_sock, _WSAIOW(IOC_VENDOR, 12) /* SIO_UDP_CONNRESET */, &no, sizeof no, NULL, 0, &got, NULL, NULL);
#endif
    /* never blocking: Ryujinx doesn't do blocking socket calls well (they slowed the whole game) */
#ifdef _WIN32
    u_long nb = 1;
    ioctlsocket(s_sock, FIONBIO, &nb);
#else
    fcntl(s_sock, F_SETFL, fcntl(s_sock, F_GETFL, 0) | O_NONBLOCK);
#endif
    uint16_t port = (uint16_t)(g_lan_port ? g_lan_port : ETH_PORT);
    struct sockaddr_in me = {0};
    me.sin_family = AF_INET;
    me.sin_addr.s_addr = htonl(INADDR_ANY);
    me.sin_port = htons(port);
    if (bind(s_sock, (struct sockaddr *)&me, sizeof me) < 0) {
        rt_log("eth: UDP port %u is taken: no tunnel", port);
        close_sock(s_sock);
        s_sock = BAD_SOCK;
        plat_net_down();
        return;
    }
    s_bcast.sin_family = AF_INET;
    s_bcast.sin_addr.s_addr = htonl(0xffffffffu);
    s_bcast.sin_port = htons(ETH_PORT);
#ifdef __SWITCH__
    /* and the network's own broadcast address (a /24 assumed: home Wi-Fi), which gets through where
     * 255.255.255.255 doesn't */
    uint32_t self = ntohl((uint32_t)gethostid());
    if (self && self != 0x7f000001u) {
        s_bcast_net = s_bcast;
        s_bcast_net.sin_addr.s_addr = htonl(self | 0xff);
    }
#endif
    char a[24] = "none";
    if (g_lan_peer[0] && !parse_peer(g_lan_peer, &s_peer)) rt_log("eth: peer \"%s\" is not a.b.c.d[:port]: not used", g_lan_peer);
    if (s_peer.sin_port) addr_str(&s_peer, a);
    if (!pthread_start("mkdd-lan", rx_thread, NULL, 64u << 10, -1, -1)) {
        rt_log("eth: no receive thread: no tunnel");
        close_sock(s_sock);
        s_sock = BAD_SOCK;
        plat_net_down();
        return;
    }
    char nb_s[24] = "none";
    if (s_bcast_net.sin_port) addr_str(&s_bcast_net, nb_s);
    rt_log("eth: tunnel on UDP %u (broadcast, network broadcast %s, peer %s), fingerprint %08x (main.dol %.8s, version %d)", port, nb_s, a, s_fp, g_dol_sha1, TUNNEL_VERSION);
}

bool eth_call(Ctx *c, const char *name, uint32_t *result) {
    if (!g_lan_on) return false;
    if (MOD_IS_RTGP(g_mod)) return false; /* see the top (RTGP and its Baby / Classic) */
    uint32_t a[3] = {(uint32_t)ctx_gpr(c, 3), (uint32_t)ctx_gpr(c, 4), (uint32_t)ctx_gpr(c, 5)};
    *result = 0;
    if (!strcmp(name, "EXIGetID")) { /* (chan, dev, u32 *id) -> 1 when a device answered */
        if (a[0] != 0 || a[1] != 2) return false;
        if (IS_RAM(a[2])) m_write_be(a[2], 4, 0x04020200u); /* EXI_ETHER */
        rt_log("eth: EXIGetID(0, 2): Broadband Adapter");
        *result = 1;
    } else if (!strcmp(name, "ETHInit")) {
        static bool once;
        if (!once) {
            once = true;
            pmutex_init(&s_lock);
            s_post_send = m_addr_of("__ETHPostSend");
            s_filter = m_addr_of("__ETHFilter");
        }
        mac_init(); /* the Wi-Fi tunnel opens with LAN mode's first send, not here */
        pmutex_lock(&s_lock);
        s_cb0 = s_cb1 = 0;
        s_types = s_ntypes = 0;
        s_rx_n = 0;
        pmutex_unlock(&s_lock);
        rt_log("eth: ETHInit(mode %u): adapter %02x:%02x:%02x:%02x:%02x:%02x, link up", a[0], s_mac[0], s_mac[1],
               s_mac[2], s_mac[3], s_mac[4], s_mac[5]);
        *result = ETH_OK;
    } else if (!strcmp(name, "ETHGetMACAddr")) {
        for (int i = 0; i < 6; i++) m_write_be(a[0] + i, 1, s_mac[i]);
    } else if (!strcmp(name, "ETHGetLinkStateAsync")) {
        if (IS_RAM(a[0])) m_write_be(a[0], 4, 1);
        *result = 1;
    } else if (!strcmp(name, "ETHSetRecvCallback")) {
        s_cb0 = a[0];
        s_cb1 = a[1];
        rt_log("eth: ETHSetRecvCallback(%#x, %#x)", a[0], a[1]);
    } else if (!strcmp(name, "ETHSetProtoType")) {
        s_types = a[0];
        s_ntypes = IS_RAM(a[0]) && a[1] < 64 ? a[1] : 0;
        char list[64 * 7 + 1] = "";
        for (uint32_t i = 0; i < s_ntypes; i++)
            snprintf(list + strlen(list), sizeof list - strlen(list), " %#06x", (unsigned)m_read_be(s_types + 2 * i, 2));
        rt_log("eth: ETHSetProtoType(%u):%s", s_ntypes, list);
    } else if (!strcmp(name, "ETHAddMulticastAddress")) {
        uint8_t m[6];
        for (int i = 0; i < 6; i++) m[i] = (uint8_t)m_read_be(a[0] + i, 1);
        rt_log("eth: ETHAddMulticastAddress(%02x:%02x:%02x:%02x:%02x:%02x)", m[0], m[1], m[2], m[3], m[4], m[5]);
    } else if (!strcmp(name, "ETHClearMulticastAddresses")) {
        rt_log("eth: ETHClearMulticastAddresses");
    } else if (!strcmp(name, "ETHSendAsync")) {
        uint32_t len = a[1] < 60 ? 60 : a[1]; /* the adapter pads short frames, as the original does */
        if (!IS_RAM(a[0]) || a[1] > FRAME_MAX || ram_off(a[0]) < 0 || ram_off(a[0]) + len > RAM_SIZE) {
            rt_log("eth: ETHSendAsync(%#x, %u): bad frame, dropped", a[0], a[1]);
        } else {
            const uint8_t *f = c->ram + ram_off(a[0]);
            /* the game calls ETHInit at start-up when it finds the adapter, but sends only in LAN mode */
            if (!g_lan_play) rt_log("eth: LAN play: the port's gameplay settings (cheats, laps, any kart) are the game's own until the app restarts");
            g_lan_play = 1;
            tunnel_start(); /* once */
            if (s_logged_out < LOG_FRAMES) s_logged_out++, log_frame("out", f, len);
            if (!dhcp_reply(f, len)) tunnel_send(f, len);
        }
        pmutex_lock(&s_lock);
        if (s_nsent < SEND_MAX) s_sent[s_nsent++] = (typeof(s_sent[0])){a[2], a[0]};
        else rt_log("eth: %u sends waiting, callback of %#x lost", SEND_MAX, a[0]);
        pmutex_unlock(&s_lock);
        sched_raise(IRQ_NET);
    } else {
        return false;
    }
    return true;
}

/* Every 10 s while frames move: what the tunnel did (s_stat), and the consoles heard. */
static void log_summary(void) {
    uint64_t now = plat_now_ns();
    if (now < s_next_summary) return;
    s_next_summary = now + 10000000000ull;
    pmutex_lock(&s_lock);
    typeof(s_stat) st = s_stat;
    char peers[PEER_MAX][64];
    uint32_t np = 0, from = s_peers_logged;
    for (uint32_t i = s_peers_logged; i < s_npeers; i++) {
        char a[24];
        const uint8_t *m = s_peers[i].mac;
        snprintf(peers[np++], sizeof peers[0], "%02x:%02x:%02x:%02x:%02x:%02x at %s", m[0], m[1], m[2], m[3], m[4], m[5], addr_str(&s_peers[i].addr, a));
    }
    s_peers_logged = s_npeers;
    char links[PEER_MAX][96];
    uint32_t nl = 0;
    for (uint32_t i = 0; i < s_nlinks; i++) {
        const Link *l = &s_links[i];
        char a[24], loss[16] = "?";
        if (l->loss_pct <= 100) snprintf(loss, sizeof loss, "%u%%", l->loss_pct);
        if (!l->answered) snprintf(links[nl++], sizeof links[0], "%s: no answer to pings (an older build?)", addr_str(&l->addr, a));
        else snprintf(links[nl++], sizeof links[0], "%s: round trip %.1f ms (most %.1f), pings lost %s", addr_str(&l->addr, a),
                      (double)l->rtt / 1e6, (double)l->rtt_max / 1e6, loss);
    }
    pmutex_unlock(&s_lock);
    for (uint32_t i = 0; i < np; i++) rt_log("eth: console %u heard: %s", from + i + 1, peers[i]);
    for (uint32_t i = 0; i < nl; i++) rt_log("eth: link %s", links[i]);
    if (!st.out && !st.in) return;
    rt_log("eth: frames out %llu (%llu bytes), in %llu (%llu bytes); dropped: repeats %llu, own %llu, queue full %llu, "
           "unwanted type %llu, no buffer %llu; send errors %llu",
           (unsigned long long)st.out, (unsigned long long)st.bytes_out, (unsigned long long)st.in, (unsigned long long)st.bytes_in,
           (unsigned long long)st.repeats, (unsigned long long)st.own, (unsigned long long)st.full, (unsigned long long)st.unwanted,
           (unsigned long long)st.no_buffer, (unsigned long long)st.send_err);
    if (st.other_game) rt_log("eth: %llu frames from another game or version (first from %s): not taken", (unsigned long long)st.other_game, st.other_from);
}

/* The Ethernet frame check sequence (CRC-32, sent low byte first), which the adapter leaves at the
 * end of a frame it hands over: the IP stack takes it off (Callback1: length - 4). */
static uint32_t fcs(const uint8_t *p, uint32_t n) {
    uint32_t c = 0xffffffffu;
    for (uint32_t i = 0; i < n; i++) {
        c ^= p[i];
        for (int k = 0; k < 8; k++) c = c >> 1 ^ (0xedb88320u & (0u - (c & 1)));
    }
    return ~c;
}

/* The type filter (ETHSetProtoType): none set takes every frame. */
static bool wanted(uint16_t type) {
    if (!s_ntypes) return true;
    for (uint32_t i = 0; i < s_ntypes; i++)
        if (m_read_be(s_types + 2 * i, 2) == type) return true;
    return false;
}

/* ---- the LAN dialogs' words ----
 * main.dol's LAN messages (a BMG: "Begin LAN setup.", "Looking for Nintendo GameCube systems...")
 * speak of LAN cables and GameCubes. Rewritten in RAM after the load, before the game runs: the
 * table's text section rebuilt with the replacements below, the entries' offsets moved. The
 * instruction booklet lines go (there is none for this): that makes room. Nothing changes unless
 * the table is the one expected and the new text fits in the old section. */
static const char *const LAN_WORDS[][2] = {
    {"\n(Please see the Mario Kart: Double Dash!!\nInstruction Booklet for more details.)", ""},
    {" (Please see\nthe Mario Kart: Double Dash!!\nInstruction Booklet for more details.)", ""},
    {"Do not touch the LAN cable\nor the POWER Button.", "Do not touch\nthe POWER Button."},
    {"Check LAN cable connections", "Check the Wi-Fi connection"},
    {"are connected to the LAN link cables.", "are on the same Wi-Fi network."},
    {"Press START / PAUSE", "Press the + Button"},
    {"LAN setup", "Wi-Fi mode setup"},
    {"LAN link", "Wi-Fi mode link"},
    {"Nintendo GameCube", "Nintendo Switch"},
};

/* Length of the message at p (up to end), its terminating 0 included; control codes (0x1A, then
 * their length) may hold zeros. 0: no end before end. */
static uint32_t bmg_len(const uint8_t *p, const uint8_t *end) {
    const uint8_t *s = p;
    while (p < end) {
        if (*p == 0) return (uint32_t)(p - s) + 1;
        if (*p == 0x1A) {
            if (p + 1 >= end || p[1] < 2) return 0;
            p += p[1];
        } else p++;
    }
    return 0;
}

void eth_patch_text(void) {
    if (!g_lan_on || MOD_IS_RTGP(g_mod)) return; /* no LAN MODE on the title menu: the dialogs never show */
    static const char FIRST[] = "Begin LAN setup.";
    const uint32_t scan = 0x01800000u < RAM_SIZE ? 0x01800000u : RAM_SIZE; /* MEM1 */
    for (uint32_t o = 0; o + 0x40 <= scan; o += 4) {
        uint8_t *h = g_ram + o;
        if (memcmp(h, "MESGbmg1", 8)) continue;
        uint32_t size = be32(h + 8), nsec = be32(h + 12);
        if (size < 0x40 || size > 0x10000 || o + size > scan || nsec != 2) continue;
        uint8_t *inf = h + 0x20, *dat = inf + be32(inf + 4);
        if (memcmp(inf, "INF1", 4) || dat + 8 > h + size || memcmp(dat, "DAT1", 4)) continue;
        uint32_t dsize = be32(dat + 4), n = (uint32_t)(inf[8] << 8 | inf[9]), esz = (uint32_t)(inf[10] << 8 | inf[11]);
        if (dsize < 8 || dat + dsize > h + size || esz < 4 || 16 + n * esz > be32(inf + 4) || !n || n > 64) continue;
        uint8_t *text = dat + 8, *tend = dat + dsize;
        if (be32(inf + 16) + sizeof FIRST > dsize - 8 || memcmp(text + be32(inf + 16), FIRST, sizeof FIRST - 1)) continue;

        /* the new section: offset 0 stays the empty message, as the game's tables start */
        uint32_t cap = dsize - 8, used = 1, changed = 0;
        uint8_t *out = calloc(1, cap), *buf = malloc(1024);
        if (!out || !buf) { free(out); free(buf); return; }
        uint32_t *offs = calloc(n, 4);
        bool ok = offs != NULL;
        for (uint32_t i = 0; ok && i < n; i++) {
            uint32_t at = be32(inf + 16 + i * esz);
            uint32_t len = at < cap ? bmg_len(text + at, tend) : 0;
            if (!len || len > 512) { ok = false; break; }
            memcpy(buf, text + at, len);
            for (size_t r = 0; r < sizeof LAN_WORDS / sizeof LAN_WORDS[0]; r++) {
                size_t ol = strlen(LAN_WORDS[r][0]), nl = strlen(LAN_WORDS[r][1]);
                for (uint32_t k = 0; k + ol <= len - 1;) {
                    if (buf[k] == 0x1A) { k += buf[k + 1]; continue; } /* never inside a control code */
                    if (memcmp(buf + k, LAN_WORDS[r][0], ol)) { k++; continue; }
                    if (len - ol + nl > 1024) { ok = false; break; }
                    memmove(buf + k + nl, buf + k + ol, len - k - ol);
                    memcpy(buf + k, LAN_WORDS[r][1], nl);
                    len = len - (uint32_t)ol + (uint32_t)nl, k += (uint32_t)nl, changed++;
                }
            }
            if (!ok || used + len > cap) { ok = false; break; }
            memcpy(out + used, buf, len);
            offs[i] = used;
            used += len;
        }
        if (ok) {
            memcpy(text, out, cap);
            for (uint32_t i = 0; i < n; i++) {
                uint8_t *e = inf + 16 + i * esz;
                e[0] = (uint8_t)(offs[i] >> 24), e[1] = (uint8_t)(offs[i] >> 16), e[2] = (uint8_t)(offs[i] >> 8), e[3] = (uint8_t)offs[i];
            }
            rt_log("eth: LAN messages at %#x: %u words changed (LAN -> Wi-Fi mode, GameCube -> Switch), %u of %u bytes", 0x80000000u + o, changed, used, cap);
        } else rt_log("eth: LAN messages at %#x left as they are (an entry too long, or no room)", 0x80000000u + o);
        free(offs);
        free(out);
        free(buf);
        return;
    }
    rt_log("eth: no LAN message table found: the game's own words");
}

/* At exit, the receive thread paused (plat_pause_threads): the socket closed, its share of the
 * sockets given back. */
void eth_exit(void) {
    if (s_sock == BAD_SOCK) return;
#ifdef _WIN32
    closesocket(s_sock);
    WSACleanup();
#else
    close(s_sock);
#endif
    s_sock = BAD_SOCK;
    plat_net_down();
}

/* IRQ_NET, in rt_deliver: the sends' callbacks, as the adapter's TX-done interrupt runs them, then
 * the frames received, as its receive interrupt hands them over (eth.c recvsub1 / readbuffer1). */
void eth_interrupt(Ctx *c) {
    typeof(s_sent[0]) done[SEND_MAX];
    pmutex_lock(&s_lock);
    uint32_t n = s_nsent;
    memcpy(done, s_sent, n * sizeof *done);
    s_nsent = 0;
    pmutex_unlock(&s_lock);
    for (uint32_t i = 0; i < n; i++) {
        uint32_t args[3] = {0, done[i].cb, done[i].addr}; /* __ETHPostSend(ltps, callback, frame) */
        if (s_post_send) ctx_call_guest(c, s_post_send, args, 3);
        else if (done[i].cb) ctx_call_guest(c, done[i].cb, args, 1);
    }
    static uint8_t f[FRAME_MAX + 4];
    for (int k = 0; k < RX_MAX; k++) {
        pmutex_lock(&s_lock);
        if (!s_rx_n) { pmutex_unlock(&s_lock); break; }
        uint32_t len = s_rx[s_rx_head].len;
        memcpy(f, s_rx[s_rx_head].data, len);
        s_rx_head = (s_rx_head + 1) % RX_MAX;
        s_rx_n--;
        pmutex_unlock(&s_lock);
        uint16_t type = (uint16_t)(f[12] << 8 | f[13]);
        if (!s_cb0 || !wanted(type)) { pmutex_lock(&s_lock), s_stat.unwanted++, pmutex_unlock(&s_lock); continue; }
        if (s_logged_in < LOG_FRAMES) s_logged_in++, log_frame("in", f, len);
        uint32_t crc = fcs(f, len);
        for (int i = 0; i < 4; i++) f[len++] = (uint8_t)(crc >> 8 * i);
        uint32_t args[3] = {type, len, 0}; /* the receive status: the IP stack doesn't read it */
        uint32_t buf = ctx_call_guest(c, s_cb0, args, 3);
        if (!buf || !IS_RAM(buf) || ram_off(buf) < 0 || ram_off(buf) + len > RAM_SIZE) { pmutex_lock(&s_lock), s_stat.no_buffer++, pmutex_unlock(&s_lock); continue; }
        memcpy(c->ram + ram_off(buf), f, len);
        uint32_t fargs[2] = {buf, len};
        if (s_filter) ctx_call_guest(c, s_filter, fargs, 2);
        if (s_cb1) ctx_call_guest(c, s_cb1, fargs, 2);
    }
    log_summary();
}
