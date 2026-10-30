// Winsock's select() takes fixed-size sets; raise the limit from 64 sockets before the header defines it.
#define FD_SETSIZE 1024
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <icmpapi.h>

#include "core/net/user_nat.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <deque>
#include <list>
#include <map>
#include <mutex>
#include <random>
#include <stdexcept>
#include <thread>
#include <tuple>

#include "core/log.h"

namespace sandvm::net {
namespace {
// Addresses are host byte order inside this file.
constexpr uint32_t ip4(unsigned a, unsigned b, unsigned c, unsigned d) { return (a << 24) | (b << 16) | (c << 8) | d; }
constexpr uint32_t kNet = ip4(10, 0, 2, 0);
constexpr uint32_t kMask = ip4(255, 255, 255, 0);
constexpr uint32_t kGateway = ip4(10, 0, 2, 2);
constexpr uint32_t kDns = ip4(10, 0, 2, 3);
constexpr uint32_t kGuest = ip4(10, 0, 2, 15);
constexpr uint32_t kLoopback = ip4(127, 0, 0, 1);
constexpr std::array<uint8_t, 6> kGatewayMac = {0x52, 0x55, 0x0A, 0x00, 0x02, 0x02};
constexpr std::array<uint8_t, 6> kBroadcastMac = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

constexpr uint16_t kEthArp = 0x0806, kEthIpv4 = 0x0800;
constexpr uint8_t kProtoIcmp = 1, kProtoTcp = 6, kProtoUdp = 17;
constexpr uint8_t kFin = 0x01, kSyn = 0x02, kRst = 0x04, kPsh = 0x08, kAck = 0x10;

constexpr uint16_t kOurMss = 1460;
constexpr size_t kToHostMax = 256 * 1024;   // guest -> host bytes buffered per connection
constexpr size_t kToGuestMax = 256 * 1024;  // host -> guest bytes buffered (unacked + unsent)
constexpr size_t kGuestQueueMax = 4096;     // frames waiting for NIC receive buffers
constexpr uint64_t kRtoMs = 300;
constexpr unsigned kMaxRetries = 12;
constexpr uint64_t kUdpIdleMs = 60'000;
constexpr uint64_t kDnsIdleMs = 10'000;
constexpr unsigned kMaxPings = 16;

uint16_t get16(const uint8_t* p) { return static_cast<uint16_t>((p[0] << 8) | p[1]); }
uint32_t get32(const uint8_t* p) { return (uint32_t{p[0]} << 24) | (p[1] << 16) | (p[2] << 8) | p[3]; }
void put16(uint8_t* p, uint16_t v) {
  p[0] = static_cast<uint8_t>(v >> 8);
  p[1] = static_cast<uint8_t>(v);
}
void put32(uint8_t* p, uint32_t v) {
  put16(p, static_cast<uint16_t>(v >> 16));
  put16(p + 2, static_cast<uint16_t>(v));
}

uint32_t sum16(const uint8_t* p, size_t len, uint32_t sum = 0) {
  for (size_t i = 0; i + 1 < len; i += 2) sum += get16(p + i);
  if (len & 1) sum += uint32_t{p[len - 1]} << 8;
  return sum;
}
uint16_t fold(uint32_t sum) {
  while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
  return static_cast<uint16_t>(~sum);
}

uint64_t now_ms() { return GetTickCount64(); }

// Sequence number comparisons modulo 2^32.
bool seq_lt(uint32_t a, uint32_t b) { return static_cast<int32_t>(a - b) < 0; }
bool seq_le(uint32_t a, uint32_t b) { return static_cast<int32_t>(a - b) <= 0; }

sockaddr_in make_addr(uint32_t ip, uint16_t port) {
  sockaddr_in a{};
  a.sin_family = AF_INET;
  a.sin_addr.s_addr = htonl(ip);
  a.sin_port = htons(port);
  return a;
}

void set_nonblocking(SOCKET s) {
  u_long one = 1;
  ioctlsocket(s, FIONBIO, &one);
}

bool would_block() {
  const int e = WSAGetLastError();
  return e == WSAEWOULDBLOCK || e == WSAEINPROGRESS;
}

// Closes with RST instead of FIN (linger 0), for connections the guest aborted.
void abort_socket(SOCKET s) {
  linger l{1, 0};
  setsockopt(s, SOL_SOCKET, SO_LINGER, reinterpret_cast<const char*>(&l), sizeof(l));
  closesocket(s);
}
}  // namespace

struct UserNat::Impl {
  enum class TcpState { Connecting, SynSent, SynReceived, Established };

  struct TcpConn {
    SOCKET sock = INVALID_SOCKET;
    TcpState state = TcpState::Connecting;
    uint32_t remote_ip = 0;  // as the guest sees it
    uint16_t remote_port = 0;
    uint16_t guest_port = 0;
    uint32_t iss = 0;      // our initial sequence number
    uint32_t snd_una = 0;  // oldest byte the guest has not acknowledged
    uint32_t snd_nxt = 0;  // next sequence number to send
    uint32_t snd_wnd = 0;  // the guest's receive window
    uint32_t rcv_nxt = 0;  // next sequence number expected from the guest
    uint16_t mss = 536;
    std::deque<uint8_t> to_guest;  // starts at snd_una (after the SYN)
    std::vector<uint8_t> to_host;
    bool host_eof = false;    // the host side finished sending (recv returned 0)
    bool fin_sent = false;    // our FIN is in flight at snd_nxt - 1
    bool fin_acked = false;
    bool guest_fin = false;   // the guest finished sending
    bool host_shut = false;   // shutdown(SD_SEND) done
    bool window_was_full = false;
    uint64_t rto_at = 0;      // 0 = no timer
    unsigned retries = 0;
  };
  using TcpKey = std::tuple<uint32_t, uint16_t, uint16_t>;  // remote ip, remote port, guest port

  struct UdpFlow {
    SOCKET sock = INVALID_SOCKET;
    bool dns = false;  // talks to the host DNS server on behalf of 10.0.2.3
    uint64_t last_used = 0;
  };

  struct Listener {
    SOCKET sock = INVALID_SOCKET;
    uint16_t guest_port = 0;
  };

  struct Ping {
    std::thread thread;
    std::atomic<bool> done{false};
  };
  struct PingResult {
    uint32_t remote_ip;
    std::vector<uint8_t> icmp;  // the guest's echo request (ICMP header + data)
    bool ok;
  };

  Deliver deliver;
  std::thread thread;
  std::atomic<bool> quit{false};
  SOCKET wake_rx = INVALID_SOCKET;
  SOCKET wake_tx = INVALID_SOCKET;
  sockaddr_in wake_addr{};
  std::atomic<bool> wake_pending{false};  // a wake-up datagram is in flight; do not send another
  bool wsa_started = false;

  std::mutex queue_mutex;  // guards from_guest and ping_results
  std::deque<std::vector<uint8_t>> from_guest;
  std::deque<PingResult> ping_results;

  // NAT thread only from here.
  std::array<uint8_t, 6> guest_mac = kBroadcastMac;
  std::deque<std::vector<uint8_t>> to_guest;
  uint16_t ip_id = 1;
  uint32_t host_dns = ip4(8, 8, 8, 8);
  std::map<TcpKey, TcpConn> tcp;
  std::map<std::pair<uint16_t, bool>, UdpFlow> udp;  // (guest port, dns)
  std::vector<Listener> listeners;
  std::list<Ping> pings;
  std::mt19937 rng{std::random_device{}()};
  uint16_t next_forward_port = 40000;  // source ports for forwarded connections, as seen by the guest

  // --- Plumbing ----------------------------------------------------------------------------------

  // Called from vCPU threads with the NIC lock held, so it must never block: one datagram at most is
  // outstanding (a full loopback buffer could otherwise stall sendto while the NAT thread waits for
  // the NIC lock), and the socket is non-blocking anyway.
  void wake() {
    if (wake_pending.exchange(true)) return;
    char b = 0;
    sendto(wake_tx, &b, 1, 0, reinterpret_cast<const sockaddr*>(&wake_addr), sizeof(wake_addr));
  }

  void send_to_guest(std::vector<uint8_t> frame) {
    if (to_guest.size() >= kGuestQueueMax) return;  // the guest stopped taking frames; drop
    to_guest.push_back(std::move(frame));
  }

  void flush_to_guest() {
    while (!to_guest.empty()) {
      if (!deliver || !deliver(to_guest.front())) return;
      to_guest.pop_front();
    }
  }

  std::vector<uint8_t> ethernet(const std::array<uint8_t, 6>& dst, uint16_t type, size_t payload) {
    std::vector<uint8_t> f(14 + payload);
    std::memcpy(f.data(), dst.data(), 6);
    std::memcpy(f.data() + 6, kGatewayMac.data(), 6);
    put16(f.data() + 12, type);
    return f;
  }

  // IPv4 packet to the guest with an L4 payload; returns the frame and fills in the IP header.
  std::vector<uint8_t> ipv4(uint32_t src, uint32_t dst, uint8_t proto, size_t l4_len,
                            const std::array<uint8_t, 6>* mac = nullptr) {
    std::vector<uint8_t> f = ethernet(mac ? *mac : guest_mac, kEthIpv4, 20 + l4_len);
    uint8_t* ip = f.data() + 14;
    ip[0] = 0x45;
    put16(ip + 2, static_cast<uint16_t>(20 + l4_len));
    put16(ip + 4, ip_id++);
    ip[6] = 0x40;  // don't fragment
    ip[8] = 64;
    ip[9] = proto;
    put32(ip + 12, src);
    put32(ip + 16, dst);
    put16(ip + 10, fold(sum16(ip, 20)));
    return f;
  }

  static uint32_t pseudo_sum(uint32_t src, uint32_t dst, uint8_t proto, size_t len) {
    return (src >> 16) + (src & 0xFFFF) + (dst >> 16) + (dst & 0xFFFF) + proto + static_cast<uint32_t>(len);
  }

  void send_udp(uint32_t src, uint16_t sport, uint32_t dst, uint16_t dport, const uint8_t* data, size_t len,
                const std::array<uint8_t, 6>* mac = nullptr) {
    std::vector<uint8_t> f = ipv4(src, dst, kProtoUdp, 8 + len, mac);
    uint8_t* u = f.data() + 34;
    put16(u, sport);
    put16(u + 2, dport);
    put16(u + 4, static_cast<uint16_t>(8 + len));
    std::memcpy(u + 8, data, len);
    uint16_t c = fold(sum16(u, 8 + len, pseudo_sum(src, dst, kProtoUdp, 8 + len)));
    put16(u + 6, c ? c : 0xFFFF);
    send_to_guest(std::move(f));
  }

  // --- Guest frames ------------------------------------------------------------------------------

  void handle_frame(const std::vector<uint8_t>& f) {
    if (f.size() < 14) return;
    if (!(f[6] & 1)) std::memcpy(guest_mac.data(), f.data() + 6, 6);  // learn the guest's address
    const uint16_t type = get16(f.data() + 12);
    if (type == kEthArp) handle_arp(f.data() + 14, f.size() - 14);
    else if (type == kEthIpv4) handle_ipv4(f.data() + 14, f.size() - 14);
  }

  void handle_arp(const uint8_t* a, size_t len) {
    if (len < 28 || get16(a) != 1 || get16(a + 2) != kEthIpv4 || a[4] != 6 || a[5] != 4) return;
    if (get16(a + 6) != 1) return;  // requests only
    const uint32_t target = get32(a + 24);
    if (target != kGateway && target != kDns) return;
    std::array<uint8_t, 6> sender_mac;
    std::memcpy(sender_mac.data(), a + 8, 6);
    std::vector<uint8_t> f = ethernet(sender_mac, kEthArp, 28);
    uint8_t* r = f.data() + 14;
    put16(r, 1);
    put16(r + 2, kEthIpv4);
    r[4] = 6;
    r[5] = 4;
    put16(r + 6, 2);  // reply
    std::memcpy(r + 8, kGatewayMac.data(), 6);
    put32(r + 14, target);
    std::memcpy(r + 18, a + 8, 10);  // the requester's MAC and IP
    send_to_guest(std::move(f));
  }

  void handle_ipv4(const uint8_t* ip, size_t len) {
    if (len < 20 || (ip[0] >> 4) != 4) return;
    const size_t ihl = (ip[0] & 0xF) * 4u;
    const size_t total = get16(ip + 2);
    if (ihl < 20 || total < ihl || total > len) return;
    if (get16(ip + 6) & 0x3FFF) return;  // fragments: not supported (TCP uses PMTU-sized segments)
    const uint32_t src = get32(ip + 12);
    const uint32_t dst = get32(ip + 16);
    const uint8_t* l4 = ip + ihl;
    const size_t l4_len = total - ihl;
    switch (ip[9]) {
      case kProtoIcmp: handle_icmp(src, dst, l4, l4_len); break;
      case kProtoUdp: handle_udp(src, dst, l4, l4_len); break;
      case kProtoTcp: handle_tcp(src, dst, l4, l4_len); break;
      default: break;
    }
  }

  // Where a guest destination goes on the host: the gateway means the host itself.
  static bool host_address(uint32_t dst, uint32_t* out) {
    if (dst == kGateway) {
      *out = kLoopback;
      return true;
    }
    if ((dst & kMask) == kNet) return false;  // nothing else lives on the virtual network
    if (dst == 0xFFFFFFFF || (dst >> 28) == 0xE || (dst >> 24) == 0 || (dst >> 24) == 127) return false;
    *out = dst;
    return true;
  }

  // --- ICMP ----------------------------------------------------------------------------------------

  void send_echo_reply(uint32_t from, const uint8_t* req, size_t len) {
    std::vector<uint8_t> f = ipv4(from, kGuest, kProtoIcmp, len);
    uint8_t* r = f.data() + 34;
    std::memcpy(r, req, len);
    r[0] = 0;  // echo reply
    r[2] = r[3] = 0;
    put16(r + 2, fold(sum16(r, len)));
    send_to_guest(std::move(f));
  }

  void handle_icmp(uint32_t src, uint32_t dst, const uint8_t* p, size_t len) {
    (void)src;
    if (len < 8 || p[0] != 8) return;  // echo requests only
    if (dst == kGateway || dst == kDns) {
      send_echo_reply(dst, p, len);
      return;
    }
    uint32_t host = 0;
    if (!host_address(dst, &host)) return;
    // Reap finished pings, then start one on its own thread (IcmpSendEcho blocks up to a second).
    pings.remove_if([](Ping& ping) {
      if (!ping.done) return false;
      ping.thread.join();
      return true;
    });
    if (pings.size() >= kMaxPings) return;
    std::vector<uint8_t> request(p, p + len);
    Ping& ping = pings.emplace_back();
    ping.thread = std::thread([this, &ping, host, dst, request = std::move(request)] {
      bool ok = false;
      HANDLE h = IcmpCreateFile();
      if (h != INVALID_HANDLE_VALUE) {
        std::vector<uint8_t> reply(sizeof(ICMP_ECHO_REPLY) + request.size() + 8);
        IPAddr addr = htonl(host);
        ok = IcmpSendEcho(h, addr, const_cast<uint8_t*>(request.data() + 8), static_cast<WORD>(request.size() - 8),
                          nullptr, reply.data(), static_cast<DWORD>(reply.size()), 1000) > 0 &&
             reinterpret_cast<ICMP_ECHO_REPLY*>(reply.data())->Status == IP_SUCCESS;
        IcmpCloseHandle(h);
      }
      {
        std::lock_guard lock(queue_mutex);
        ping_results.push_back({dst, request, ok});
      }
      ping.done = true;
      wake();
    });
  }

  // --- UDP and DHCP --------------------------------------------------------------------------------

  void handle_udp(uint32_t src, uint32_t dst, const uint8_t* u, size_t len) {
    if (len < 8) return;
    const uint16_t sport = get16(u);
    const uint16_t dport = get16(u + 2);
    const size_t ulen = get16(u + 4);
    if (ulen < 8 || ulen > len) return;
    const uint8_t* data = u + 8;
    const size_t data_len = ulen - 8;
    if (dport == 67) {
      handle_dhcp(data, data_len);
      return;
    }
    (void)src;
    const bool dns = dst == kDns && dport == 53;
    uint32_t host = 0;
    if (dns) host = host_dns;
    else if (!host_address(dst, &host)) return;

    auto key = std::make_pair(sport, dns);
    auto it = udp.find(key);
    if (it == udp.end()) {
      SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
      if (s == INVALID_SOCKET) return;
      set_nonblocking(s);
      sockaddr_in any = make_addr(0, 0);
      bind(s, reinterpret_cast<sockaddr*>(&any), sizeof(any));
      it = udp.emplace(key, UdpFlow{s, dns, 0}).first;
    }
    it->second.last_used = now_ms();
    sockaddr_in to = make_addr(host, dport);
    sendto(it->second.sock, reinterpret_cast<const char*>(data), static_cast<int>(data_len), 0,
           reinterpret_cast<sockaddr*>(&to), sizeof(to));
  }

  void udp_readable(std::pair<uint16_t, bool> key, UdpFlow& flow) {
    for (;;) {
      uint8_t buf[65536];
      sockaddr_in from{};
      int from_len = sizeof(from);
      const int n = recvfrom(flow.sock, reinterpret_cast<char*>(buf), sizeof(buf), 0,
                             reinterpret_cast<sockaddr*>(&from), &from_len);
      if (n < 0) return;  // would block, or ICMP port unreachable reported as WSAECONNRESET
      flow.last_used = now_ms();
      uint32_t peer = ntohl(from.sin_addr.s_addr);
      uint16_t peer_port = ntohs(from.sin_port);
      if (flow.dns) {
        peer = kDns;
        peer_port = 53;
      } else if (peer == kLoopback) {
        peer = kGateway;
      }
      send_udp(peer, peer_port, kGuest, key.first, buf, static_cast<size_t>(n));
    }
  }

  void handle_dhcp(const uint8_t* d, size_t len) {
    if (len < 240 || d[0] != 1 || get32(d + 236) != 0x63825363) return;
    uint8_t type = 0;
    for (size_t i = 240; i < len;) {
      const uint8_t opt = d[i];
      if (opt == 0) {
        ++i;
        continue;
      }
      if (opt == 255 || i + 1 >= len) break;
      const uint8_t olen = d[i + 1];
      if (i + 2 + olen > len) break;
      if (opt == 53 && olen >= 1) type = d[i + 2];
      i += 2 + olen;
    }
    uint8_t reply_type;
    if (type == 1) reply_type = 2;                       // DISCOVER -> OFFER
    else if (type == 3 || type == 8) reply_type = 5;     // REQUEST / INFORM -> ACK
    else return;

    std::vector<uint8_t> r(240);
    r[0] = 2;  // BOOTREPLY
    r[1] = 1;
    r[2] = 6;
    std::memcpy(&r[4], d + 4, 4);    // xid
    std::memcpy(&r[10], d + 10, 2);  // flags
    if (type == 8) std::memcpy(&r[12], d + 12, 4);  // INFORM: ciaddr stays, no yiaddr
    else put32(&r[16], kGuest);
    put32(&r[20], kGateway);  // siaddr
    std::memcpy(&r[28], d + 28, 16);  // chaddr
    put32(&r[236], 0x63825363);
    auto opt = [&r](uint8_t code, std::initializer_list<uint8_t> bytes) {
      r.push_back(code);
      r.push_back(static_cast<uint8_t>(bytes.size()));
      r.insert(r.end(), bytes);
    };
    opt(53, {reply_type});
    opt(54, {10, 0, 2, 2});          // server identifier
    if (type != 8) opt(51, {0x00, 0x01, 0x51, 0x80});  // lease 86400 s
    opt(1, {255, 255, 255, 0});      // subnet mask
    opt(3, {10, 0, 2, 2});           // router
    opt(6, {10, 0, 2, 3});           // DNS server
    r.push_back(255);
    while (r.size() < 300) r.push_back(0);  // BOOTP minimum size
    // Broadcast: the guest has no address yet and may not accept unicast before it does.
    send_udp(kGateway, 67, 0xFFFFFFFF, 68, r.data(), r.size(), &kBroadcastMac);
  }

  // --- TCP -----------------------------------------------------------------------------------------

  uint16_t advertised_window(const TcpConn& c) const {
    return static_cast<uint16_t>(std::min<size_t>(0xFFFF, kToHostMax - std::min(kToHostMax, c.to_host.size())));
  }

  void send_tcp(const TcpConn& c, uint8_t flags, uint32_t seq, const uint8_t* data = nullptr, size_t len = 0) {
    const bool mss_option = (flags & kSyn) != 0;
    const size_t hlen = mss_option ? 24 : 20;
    std::vector<uint8_t> f = ipv4(c.remote_ip, kGuest, kProtoTcp, hlen + len);
    uint8_t* t = f.data() + 34;
    put16(t, c.remote_port);
    put16(t + 2, c.guest_port);
    put32(t + 4, seq);
    put32(t + 8, (flags & kAck) ? c.rcv_nxt : 0);
    t[12] = static_cast<uint8_t>((hlen / 4) << 4);
    t[13] = flags;
    put16(t + 14, advertised_window(c));
    if (mss_option) {
      t[20] = 2;
      t[21] = 4;
      put16(t + 22, kOurMss);
    }
    if (len) std::memcpy(t + hlen, data, len);
    put16(t + 16, fold(sum16(t, hlen + len, pseudo_sum(c.remote_ip, kGuest, kProtoTcp, hlen + len))));
    send_to_guest(std::move(f));
  }

  // A RST for a segment that matches no connection (RFC 793 "reset generation").
  void send_rst_for(uint32_t remote_ip, uint16_t remote_port, uint16_t guest_port, const uint8_t* t, size_t seg_len) {
    TcpConn c;
    c.remote_ip = remote_ip;
    c.remote_port = remote_port;
    c.guest_port = guest_port;
    const uint8_t flags = t[13];
    if (flags & kAck) {
      send_tcp(c, kRst, get32(t + 8));
    } else {
      c.rcv_nxt = get32(t + 4) + static_cast<uint32_t>(seg_len) + ((flags & kSyn) ? 1 : 0) + ((flags & kFin) ? 1 : 0);
      send_tcp(c, kRst | kAck, 0);
    }
  }

  void close_conn(std::map<TcpKey, TcpConn>::iterator it, bool abort) {
    if (it->second.sock != INVALID_SOCKET) {
      if (abort) abort_socket(it->second.sock);
      else closesocket(it->second.sock);
    }
    tcp.erase(it);
  }

  void reset_conn(std::map<TcpKey, TcpConn>::iterator it) {
    send_tcp(it->second, kRst | kAck, it->second.snd_nxt);
    close_conn(it, true);
  }

  void arm_rto(TcpConn& c) { c.rto_at = now_ms() + (kRtoMs << std::min(c.retries, 5u)); }

  // Sends whatever the guest's window allows, then our FIN once the host side is done and all data went.
  void tcp_output(TcpConn& c) {
    if (c.state != TcpState::Established) return;
    const uint32_t queued = static_cast<uint32_t>(c.to_guest.size());
    for (;;) {
      const uint32_t in_flight = c.snd_nxt - c.snd_una - (c.fin_sent ? 1 : 0);
      if (in_flight >= queued || c.fin_sent) break;
      const uint32_t window = c.snd_wnd;
      if (in_flight >= window) break;
      const uint32_t len = std::min({uint32_t{c.mss}, queued - in_flight, window - in_flight});
      std::vector<uint8_t> data(c.to_guest.begin() + in_flight, c.to_guest.begin() + in_flight + len);
      send_tcp(c, kAck | kPsh, c.snd_nxt, data.data(), len);
      c.snd_nxt += len;
      if (!c.rto_at) arm_rto(c);
    }
    const uint32_t in_flight = c.snd_nxt - c.snd_una - (c.fin_sent ? 1 : 0);
    if (c.host_eof && !c.fin_sent && in_flight == queued) {
      send_tcp(c, kFin | kAck, c.snd_nxt);
      c.snd_nxt += 1;
      c.fin_sent = true;
      if (!c.rto_at) arm_rto(c);
    }
    // Zero window with data waiting: the retransmission timer doubles as the persist timer.
    if (c.snd_wnd == 0 && queued > 0 && !c.rto_at) arm_rto(c);
  }

  void tcp_timer(std::map<TcpKey, TcpConn>::iterator it, uint64_t now) {
    TcpConn& c = it->second;
    if (!c.rto_at || now < c.rto_at) return;
    c.rto_at = 0;
    if (++c.retries > kMaxRetries) {
      reset_conn(it);
      return;
    }
    switch (c.state) {
      case TcpState::SynSent: send_tcp(c, kSyn, c.iss); break;
      case TcpState::SynReceived: send_tcp(c, kSyn | kAck, c.iss); break;
      case TcpState::Established:
        if (c.snd_wnd == 0 && !c.to_guest.empty() && c.snd_nxt == c.snd_una) {
          // Window probe: one byte beyond the closed window.
          const uint8_t b = c.to_guest.front();
          send_tcp(c, kAck, c.snd_una, &b, 1);
        } else {
          // Go back N: resend everything from the oldest unacknowledged byte.
          c.snd_nxt = c.snd_una;
          c.fin_sent = false;
          tcp_output(c);
        }
        break;
      default: break;
    }
    arm_rto(c);
  }

  void handle_tcp(uint32_t src, uint32_t dst, const uint8_t* t, size_t len) {
    (void)src;
    if (len < 20) return;
    const size_t hlen = (t[12] >> 4) * 4u;
    if (hlen < 20 || hlen > len) return;
    const uint16_t guest_port = get16(t);
    const uint16_t remote_port = get16(t + 2);
    const uint32_t seq = get32(t + 4);
    const uint32_t ack = get32(t + 8);
    const uint8_t flags = t[13];
    const uint32_t window = get16(t + 14);
    const uint8_t* data = t + hlen;
    const size_t data_len = len - hlen;

    auto it = tcp.find({dst, remote_port, guest_port});
    if (it == tcp.end()) {
      if (flags & kRst) return;
      if ((flags & (kSyn | kAck)) != kSyn) {
        send_rst_for(dst, remote_port, guest_port, t, data_len);
        return;
      }
      open_conn(dst, remote_port, guest_port, seq, window, t, hlen);
      return;
    }
    TcpConn& c = it->second;
    if (flags & kRst) {
      close_conn(it, true);
      return;
    }

    switch (c.state) {
      case TcpState::Connecting: return;  // the guest repeats its SYN; we answer once the host connects
      case TcpState::SynSent:
        if ((flags & (kSyn | kAck)) == (kSyn | kAck) && ack == c.iss + 1) {
          c.rcv_nxt = seq + 1;
          c.snd_una = c.snd_nxt = c.iss + 1;
          c.snd_wnd = window;
          c.mss = parse_mss(t, hlen);
          c.state = TcpState::Established;
          c.retries = 0;
          c.rto_at = 0;
          send_tcp(c, kAck, c.snd_nxt);
        }
        return;
      case TcpState::SynReceived:
        if (flags & kSyn) {  // our SYN-ACK got lost
          send_tcp(c, kSyn | kAck, c.iss);
          return;
        }
        if (!(flags & kAck) || ack != c.iss + 1) return;
        c.state = TcpState::Established;
        c.snd_una = c.snd_nxt = c.iss + 1;
        c.retries = 0;
        c.rto_at = 0;
        break;
      case TcpState::Established: break;
    }

    // Acknowledgement of our data / FIN.
    if (flags & kAck) {
      if (seq_lt(c.snd_una, ack) && seq_le(ack, c.snd_nxt)) {
        uint32_t acked = ack - c.snd_una;
        if (c.fin_sent && ack == c.snd_nxt) {
          c.fin_acked = true;
          acked -= 1;
        }
        acked = std::min<uint32_t>(acked, static_cast<uint32_t>(c.to_guest.size()));
        c.to_guest.erase(c.to_guest.begin(), c.to_guest.begin() + acked);
        c.snd_una = ack;
        c.retries = 0;
        c.rto_at = c.snd_una == c.snd_nxt ? 0 : now_ms() + kRtoMs;
      }
      c.snd_wnd = window;
    }

    // Data, in order only; anything else gets a duplicate ACK and the guest retransmits.
    bool need_ack = false;
    uint32_t taken = 0;
    if (data_len > 0) {
      need_ack = true;
      const uint32_t offset = c.rcv_nxt - seq;  // bytes of this segment we already have
      if (seq_le(seq, c.rcv_nxt) && offset < data_len && !c.guest_fin) {
        const size_t room = kToHostMax - std::min(kToHostMax, c.to_host.size());
        taken = static_cast<uint32_t>(std::min<size_t>(data_len - offset, room));
        c.to_host.insert(c.to_host.end(), data + offset, data + offset + taken);
        c.rcv_nxt += taken;
        if (taken < data_len - offset) c.window_was_full = true;
      }
    }
    if ((flags & kFin) && !c.guest_fin && seq + static_cast<uint32_t>(data_len) == c.rcv_nxt) {
      c.rcv_nxt += 1;
      c.guest_fin = true;
      need_ack = true;
    }
    flush_to_host(it);
    if (tcp.find({dst, remote_port, guest_port}) == tcp.end()) return;  // closed by an error
    if (need_ack) send_tcp(c, kAck, c.snd_nxt);
    tcp_output(c);
    maybe_finish(it);
  }

  static uint16_t parse_mss(const uint8_t* t, size_t hlen) {
    for (size_t i = 20; i < hlen;) {
      const uint8_t kind = t[i];
      if (kind == 0) break;
      if (kind == 1) {
        ++i;
        continue;
      }
      if (i + 1 >= hlen || t[i + 1] < 2) break;
      if (kind == 2 && t[i + 1] == 4 && i + 4 <= hlen) return std::clamp<uint16_t>(get16(t + i + 2), 64, kOurMss);
      i += t[i + 1];
    }
    return 536;  // RFC 1122 default
  }

  void open_conn(uint32_t remote_ip, uint16_t remote_port, uint16_t guest_port, uint32_t seq, uint32_t window,
                 const uint8_t* t, size_t hlen) {
    TcpConn c;
    c.remote_ip = remote_ip;
    c.remote_port = remote_port;
    c.guest_port = guest_port;
    c.rcv_nxt = seq + 1;
    c.snd_wnd = window;
    c.mss = parse_mss(t, hlen);
    c.iss = static_cast<uint32_t>(rng());
    c.snd_una = c.snd_nxt = c.iss;
    uint32_t host = 0;
    if (!host_address(remote_ip, &host)) {
      send_tcp(c, kRst | kAck, 0);
      return;
    }
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) {
      send_tcp(c, kRst | kAck, 0);
      return;
    }
    set_nonblocking(s);
    BOOL nodelay = TRUE;  // the guest already coalesces; do not add Nagle delay on top
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&nodelay), sizeof(nodelay));
    sockaddr_in to = make_addr(host, remote_port);
    if (connect(s, reinterpret_cast<sockaddr*>(&to), sizeof(to)) != 0 && !would_block()) {
      closesocket(s);
      send_tcp(c, kRst | kAck, 0);
      return;
    }
    c.sock = s;
    c.state = TcpState::Connecting;
    tcp.emplace(TcpKey{remote_ip, remote_port, guest_port}, std::move(c));
  }

  void connect_done(std::map<TcpKey, TcpConn>::iterator it, bool failed) {
    TcpConn& c = it->second;
    int err = 0;
    int err_len = sizeof(err);
    getsockopt(c.sock, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&err), &err_len);
    if (failed || err) {
      send_tcp(c, kRst | kAck, 0);
      close_conn(it, false);
      return;
    }
    c.state = TcpState::SynReceived;
    c.snd_nxt = c.iss + 1;
    send_tcp(c, kSyn | kAck, c.iss);
    arm_rto(c);
  }

  void flush_to_host(std::map<TcpKey, TcpConn>::iterator it) {
    TcpConn& c = it->second;
    if (c.state != TcpState::Established && c.state != TcpState::SynReceived) return;
    while (!c.to_host.empty()) {
      const int n = send(c.sock, reinterpret_cast<const char*>(c.to_host.data()),
                         static_cast<int>(std::min<size_t>(c.to_host.size(), 1 << 20)), 0);
      if (n < 0) {
        if (would_block()) break;
        reset_conn(it);
        return;
      }
      c.to_host.erase(c.to_host.begin(), c.to_host.begin() + n);
    }
    if (c.window_was_full && c.to_host.size() < kToHostMax / 2) {
      c.window_was_full = false;
      send_tcp(c, kAck, c.snd_nxt);  // window update
    }
    if (c.guest_fin && c.to_host.empty() && !c.host_shut) {
      shutdown(c.sock, SD_SEND);
      c.host_shut = true;
    }
  }

  void host_readable(std::map<TcpKey, TcpConn>::iterator it) {
    TcpConn& c = it->second;
    while (!c.host_eof && c.to_guest.size() < kToGuestMax) {
      uint8_t buf[65536];
      const int want = static_cast<int>(std::min<size_t>(sizeof(buf), kToGuestMax - c.to_guest.size()));
      const int n = recv(c.sock, reinterpret_cast<char*>(buf), want, 0);
      if (n == 0) {
        c.host_eof = true;
        break;
      }
      if (n < 0) {
        if (would_block()) break;
        reset_conn(it);
        return;
      }
      c.to_guest.insert(c.to_guest.end(), buf, buf + n);
    }
    tcp_output(c);
    maybe_finish(it);
  }

  void maybe_finish(std::map<TcpKey, TcpConn>::iterator it) {
    const TcpConn& c = it->second;
    if (c.guest_fin && c.fin_acked && c.host_shut) close_conn(it, false);
  }

  // A host client connected to a forwarded port: open the connection towards the guest.
  void accept_forward(Listener& l) {
    sockaddr_in from{};
    int from_len = sizeof(from);
    SOCKET s = accept(l.sock, reinterpret_cast<sockaddr*>(&from), &from_len);
    if (s == INVALID_SOCKET) return;
    set_nonblocking(s);
    TcpConn c;
    c.sock = s;
    c.remote_ip = kGateway;
    c.remote_port = next_forward_port;
    next_forward_port = next_forward_port >= 60000 ? 40000 : next_forward_port + 1;
    c.guest_port = l.guest_port;
    c.iss = static_cast<uint32_t>(rng());
    c.snd_una = c.iss;
    c.snd_nxt = c.iss + 1;
    c.state = TcpState::SynSent;
    send_tcp(c, kSyn, c.iss);
    arm_rto(c);
    tcp.emplace(TcpKey{c.remote_ip, c.remote_port, c.guest_port}, std::move(c));
  }

  // --- Main loop -----------------------------------------------------------------------------------

  void run() {
    while (!quit) {
      fd_set readable, writable, failed;
      FD_ZERO(&readable);
      FD_ZERO(&writable);
      FD_ZERO(&failed);
      FD_SET(wake_rx, &readable);
      uint64_t next_timer = now_ms() + 1000;
      unsigned count = 1;
      auto add = [&count](SOCKET s, fd_set& set) {
        if (count < FD_SETSIZE) FD_SET(s, &set);
      };
      for (auto& [key, c] : tcp) {
        if (c.state == TcpState::Connecting) {
          add(c.sock, writable);
          add(c.sock, failed);
        } else {
          if (!c.host_eof && c.to_guest.size() < kToGuestMax) add(c.sock, readable);
          if (!c.to_host.empty()) add(c.sock, writable);
        }
        if (c.rto_at) next_timer = std::min(next_timer, c.rto_at);
        ++count;
      }
      for (auto& [key, flow] : udp) {
        add(flow.sock, readable);
        ++count;
      }
      for (auto& l : listeners) {
        add(l.sock, readable);
        ++count;
      }
      // Frames the NIC could not take yet are retried on rx_space() (which wakes us) or every 50 ms.
      if (!to_guest.empty()) next_timer = std::min(next_timer, now_ms() + 50);
      const uint64_t now = now_ms();
      const uint64_t wait = next_timer > now ? next_timer - now : 0;
      timeval tv{static_cast<long>(wait / 1000), static_cast<long>((wait % 1000) * 1000)};
      const int ready = select(0, &readable, &writable, &failed, &tv);
      if (quit) break;
      if (ready < 0) {
        SVM_WARN("net: select failed (%d)", WSAGetLastError());
        Sleep(10);
        continue;
      }

      if (FD_ISSET(wake_rx, &readable)) {
        char buf[256];
        while (recv(wake_rx, buf, sizeof(buf), 0) > 0) {
        }
      }
      // Cleared before the queues are taken, so a request queued after this point wakes us again.
      wake_pending = false;
      std::deque<std::vector<uint8_t>> frames;
      std::deque<PingResult> results;
      {
        std::lock_guard lock(queue_mutex);
        frames.swap(from_guest);
        results.swap(ping_results);
      }
      for (const auto& f : frames) handle_frame(f);
      for (const auto& r : results) {
        if (r.ok) send_echo_reply(r.remote_ip, r.icmp.data(), r.icmp.size());
      }

      for (auto it = tcp.begin(); it != tcp.end();) {
        auto next = std::next(it);
        TcpConn& c = it->second;
        if (c.state == TcpState::Connecting) {
          if (FD_ISSET(c.sock, &failed)) connect_done(it, true);
          else if (FD_ISSET(c.sock, &writable)) connect_done(it, false);
        } else {
          const TcpKey key = it->first;
          const bool can_read = FD_ISSET(c.sock, &readable) != 0;
          if (FD_ISSET(c.sock, &writable)) flush_to_host(it);  // may close the connection
          auto again = tcp.find(key);
          if (can_read && again != tcp.end()) host_readable(again);
        }
        it = next;
      }
      for (auto& [key, flow] : udp) {
        if (FD_ISSET(flow.sock, &readable)) udp_readable(key, flow);
      }
      for (auto& l : listeners) {
        if (FD_ISSET(l.sock, &readable)) accept_forward(l);
      }

      const uint64_t t = now_ms();
      for (auto it = tcp.begin(); it != tcp.end();) {
        auto next = std::next(it);
        tcp_timer(it, t);
        it = next;
      }
      for (auto it = udp.begin(); it != udp.end();) {
        if (t - it->second.last_used > (it->second.dns ? kDnsIdleMs : kUdpIdleMs)) {
          closesocket(it->second.sock);
          it = udp.erase(it);
        } else {
          ++it;
        }
      }
      flush_to_guest();
    }
  }

  void start(std::vector<PortForward> forwards) {
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) throw std::runtime_error("net: WSAStartup failed");
    wsa_started = true;

    // The wake-up channel: a UDP socket on loopback that other threads poke to interrupt select().
    wake_rx = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    wake_tx = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (wake_rx == INVALID_SOCKET || wake_tx == INVALID_SOCKET) throw std::runtime_error("net: socket failed");
    wake_addr = make_addr(kLoopback, 0);
    int len = sizeof(wake_addr);
    if (bind(wake_rx, reinterpret_cast<sockaddr*>(&wake_addr), sizeof(wake_addr)) != 0 ||
        getsockname(wake_rx, reinterpret_cast<sockaddr*>(&wake_addr), &len) != 0) {
      throw std::runtime_error("net: cannot bind the wake-up socket");
    }
    set_nonblocking(wake_rx);
    set_nonblocking(wake_tx);

    // The host's DNS server, for queries to 10.0.2.3.
    ULONG size = 0;
    GetNetworkParams(nullptr, &size);
    std::vector<uint8_t> info(size ? size : sizeof(FIXED_INFO));
    if (GetNetworkParams(reinterpret_cast<FIXED_INFO*>(info.data()), &size) == ERROR_SUCCESS) {
      in_addr a{};
      if (inet_pton(AF_INET, reinterpret_cast<FIXED_INFO*>(info.data())->DnsServerList.IpAddress.String, &a) == 1 &&
          a.s_addr != 0) {
        host_dns = ntohl(a.s_addr);
      }
    }

    for (const PortForward& f : forwards) {
      SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
      sockaddr_in at = make_addr(kLoopback, f.host_port);
      if (s == INVALID_SOCKET || bind(s, reinterpret_cast<sockaddr*>(&at), sizeof(at)) != 0 || listen(s, 16) != 0) {
        SVM_WARN("net: cannot listen on 127.0.0.1:%u for the forward to guest port %u", f.host_port, f.guest_port);
        if (s != INVALID_SOCKET) closesocket(s);
        continue;
      }
      set_nonblocking(s);
      listeners.push_back({s, f.guest_port});
      SVM_INFO("net: 127.0.0.1:%u -> guest port %u", f.host_port, f.guest_port);
    }
    thread = std::thread([this] { run(); });
  }

  void shutdown_all() {
    quit = true;
    if (wake_tx != INVALID_SOCKET) wake();
    if (thread.joinable()) thread.join();
    for (Ping& p : pings) {
      if (p.thread.joinable()) p.thread.join();
    }
    pings.clear();
    for (auto& [key, c] : tcp) {
      if (c.sock != INVALID_SOCKET) closesocket(c.sock);
    }
    tcp.clear();
    for (auto& [key, flow] : udp) closesocket(flow.sock);
    udp.clear();
    for (auto& l : listeners) closesocket(l.sock);
    listeners.clear();
    if (wake_rx != INVALID_SOCKET) closesocket(wake_rx);
    if (wake_tx != INVALID_SOCKET) closesocket(wake_tx);
    wake_rx = wake_tx = INVALID_SOCKET;
    if (wsa_started) WSACleanup();
    wsa_started = false;
  }
};

UserNat::UserNat(Deliver deliver, std::vector<PortForward> forwards) : impl_(std::make_unique<Impl>()) {
  impl_->deliver = std::move(deliver);
  try {
    impl_->start(std::move(forwards));
  } catch (...) {
    impl_->shutdown_all();
    throw;
  }
  SVM_INFO("net: user-mode NAT, guest 10.0.2.15/24, gateway 10.0.2.2, DNS 10.0.2.3");
}

UserNat::~UserNat() { stop(); }

void UserNat::stop() { impl_->shutdown_all(); }

void UserNat::guest_send(std::span<const uint8_t> frame) {
  if (impl_->quit) return;
  {
    std::lock_guard lock(impl_->queue_mutex);
    if (impl_->from_guest.size() >= kGuestQueueMax) return;
    impl_->from_guest.emplace_back(frame.begin(), frame.end());
  }
  impl_->wake();
}

void UserNat::rx_space() {
  if (!impl_->quit) impl_->wake();
}

}  // namespace sandvm::net
