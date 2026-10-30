#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <vector>

namespace sandvm::net {

// User-mode NAT between the guest's Ethernet and the host's sockets, the same idea as QEMU's "user"
// networking (slirp), so the guest reaches the network with no drivers, adapters or admin rights on
// the host, also inside a Sandboxie box.
//
// The guest sees a private network 10.0.2.0/24: gateway 10.0.2.2 (which is also the host: TCP/UDP to
// it go to the host's 127.0.0.1), DNS 10.0.2.3 (forwarded to the host's first DNS server), and gets
// 10.0.2.15 from the built-in DHCP server. IPv4 only.
//  - ARP for the gateway and DNS addresses; DHCP DISCOVER/REQUEST/INFORM.
//  - UDP: one host socket per guest source port, replies from any peer come back (idle timeout).
//  - TCP: the guest's connection ends here, in a small TCP implementation (no window scaling, SACK or
//    timestamps), and a host socket carries the data on. Go-back-N retransmission towards the guest.
//  - ICMP echo: answered for the gateway/DNS, otherwise sent with the host's ICMP API.
//  - Port forwards: a host TCP port (bound to 127.0.0.1) connects through to a guest port.
//
// One thread does all the work. guest_send() and rx_space() may be called from any thread (the NIC's
// vCPU thread); deliver is called on the NAT thread, never with an internal lock held.
class UserNat {
 public:
  struct PortForward {
    uint16_t host_port;
    uint16_t guest_port;
  };
  // Hands a frame to the NIC; false = no room now (the frame is kept and retried after rx_space()).
  using Deliver = std::function<bool(std::span<const uint8_t> frame)>;

  UserNat(Deliver deliver, std::vector<PortForward> forwards = {});
  ~UserNat();  // stops the thread
  UserNat(const UserNat&) = delete;
  UserNat& operator=(const UserNat&) = delete;

  // Stops the thread; deliver is not called after this returns. Idempotent.
  void stop();

  void guest_send(std::span<const uint8_t> frame);  // a frame the guest transmitted
  void rx_space();                                  // the NIC has receive buffers again

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace sandvm::net
