#include "core/devices/uhci.h"

#include <algorithm>
#include <stdexcept>

#include "core/log.h"

namespace sandvm::devices {
namespace {
// Register offsets.
constexpr unsigned kCmd = 0x00, kSts = 0x02, kIntr = 0x04, kFrnum = 0x06, kFlbase = 0x08, kSofmod = 0x0C,
                   kPortsc = 0x10;
// USBCMD.
constexpr uint16_t kCmdRun = 0x0001, kCmdHcReset = 0x0002, kCmdGlobalReset = 0x0004;
// USBSTS.
constexpr uint16_t kStsUsbInt = 0x0001, kStsUsbErr = 0x0002, kStsResume = 0x0004, kStsHostError = 0x0008,
                   kStsProcessError = 0x0010, kStsHalted = 0x0020;
// PORTSC.
constexpr uint16_t kPortConnect = 0x0001, kPortConnectChange = 0x0002, kPortEnable = 0x0004,
                   kPortEnableChange = 0x0008, kPortReset = 0x0200;
// Bits a PORTSC write leaves alone: status, write-one-to-clear changes, line status, low speed, bit 7.
constexpr uint16_t kPortKeep = 0x01FB;
// Link pointers.
constexpr uint32_t kLinkTerminate = 1, kLinkQh = 2, kLinkDepth = 4;
// TD control/status.
constexpr uint32_t kTdSpd = 1u << 29, kTdIos = 1u << 25, kTdIoc = 1u << 24, kTdActive = 1u << 23,
                   kTdStall = 1u << 22, kTdTimeout = 1u << 18, kTdStatusMask = 0x00FF0000, kTdErrorCount = 3u << 27;
// Upper bounds on work per frame: real hardware is limited by the 1 ms frame; a guest's malformed
// schedule must not keep the thread busy forever.
constexpr int kTdBudget = 256;
constexpr int kMaxCatchUpFrames = 8;
}  // namespace

Uhci::Uhci(IrqCallback on_irq, DmaMemory dma) : on_irq_(std::move(on_irq)), dma_(std::move(dma)) {
  reset_locked();
  timer_ = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
  if (!timer_) timer_ = CreateWaitableTimerExW(nullptr, nullptr, 0, TIMER_ALL_ACCESS);
  wake_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  if (!timer_ || !wake_) {
    if (timer_) CloseHandle(timer_);
    if (wake_) CloseHandle(wake_);
    throw std::runtime_error("uhci: cannot create the frame timer");
  }
  thread_ = std::thread([this] { frame_thread(); });
}

Uhci::~Uhci() {
  stop();
  CloseHandle(timer_);
  CloseHandle(wake_);
}

void Uhci::stop() {
  {
    std::lock_guard lock(mutex_);
    quit_ = true;
  }
  SetEvent(wake_);
  if (thread_.joinable()) thread_.join();
}

void Uhci::attach(unsigned port, std::shared_ptr<UsbDevice> device) {
  if (port >= kPorts || !device) throw std::invalid_argument("uhci: bad port or null device");
  std::lock_guard lock(mutex_);
  ports_[port].device = std::move(device);
  ports_[port].sc = 0x0080 | kPortConnect | kPortConnectChange;
}

void Uhci::set_bus_master(bool enabled) {
  {
    std::lock_guard lock(mutex_);
    bus_master_ = enabled;
  }
  SetEvent(wake_);
}

void Uhci::reset_locked() {
  cmd_ = 0;
  status_ = kStsHalted;
  status2_ = 0;
  intr_ = 0;
  frnum_ = 0;
  flbase_ = 0;
  sofmod_ = 64;
  for (auto& p : ports_) p.sc = 0x0080 | (p.device ? kPortConnect | kPortConnectChange : 0);
  update_irq_locked();
}

void Uhci::update_irq_locked() {
  // USBINTR bits: 0 timeout/CRC, 1 resume, 2 interrupt on complete, 3 short packet.
  const bool level = ((status2_ & 1) && (intr_ & 0x4)) || ((status2_ & 2) && (intr_ & 0x8)) ||
                     ((status_ & kStsUsbErr) && (intr_ & 0x1)) || ((status_ & kStsResume) && (intr_ & 0x2)) ||
                     (status_ & (kStsHostError | kStsProcessError));
  if (level == irq_line_) return;
  irq_line_ = level;
  if (on_irq_) on_irq_(level);
}

uint32_t Uhci::pio_read(uint16_t port, unsigned size) {
  std::lock_guard lock(mutex_);
  const unsigned off = port & (kIoSize - 1);
  if (size == 4) return read16(off) | (uint32_t{read16(off + 2)} << 16);
  const uint16_t word = read16(off & ~1u);
  if (size == 1) return (off & 1) ? word >> 8 : word & 0xFF;
  return word;
}

void Uhci::pio_write(uint16_t port, unsigned size, uint32_t value) {
  bool started = false;
  {
    std::lock_guard lock(mutex_);
    const unsigned off = port & (kIoSize - 1);
    const bool was_running = cmd_ & kCmdRun;
    if (size == 4) {
      write16(off, static_cast<uint16_t>(value));
      write16(off + 2, static_cast<uint16_t>(value >> 16));
    } else if (size == 2) {
      write16(off, static_cast<uint16_t>(value));
    } else if (off == kSofmod) {
      sofmod_ = static_cast<uint8_t>(value);
    } else {
      // A byte write merges into the register's other byte (only SOFMOD is byte wide in the spec),
      // except that write-one-to-clear bits of the untouched byte must be written as zero.
      const unsigned reg = off & ~1u;
      uint16_t word = reg == kSts ? 0 : read16(reg);
      if (reg >= kPortsc) word &= static_cast<uint16_t>(~(kPortConnectChange | kPortEnableChange));
      word = (off & 1) ? static_cast<uint16_t>((word & 0x00FF) | ((value & 0xFF) << 8))
                       : static_cast<uint16_t>((word & 0xFF00) | (value & 0xFF));
      write16(reg, word);
    }
    started = !was_running && (cmd_ & kCmdRun);
  }
  if (started) SetEvent(wake_);
}

uint16_t Uhci::read16(unsigned off) {
  switch (off) {
    case kCmd: return cmd_;
    case kSts: return status_;
    case kIntr: return intr_;
    case kFrnum: return frnum_;
    case kFlbase: return static_cast<uint16_t>(flbase_);
    case kFlbase + 2: return static_cast<uint16_t>(flbase_ >> 16);
    case kSofmod: return sofmod_;
    case kPortsc:
    case kPortsc + 2:
      return ports_[(off - kPortsc) / 2].sc;
    default:
      // Reads beyond the two ports return what a nonexistent port would: bit 7 clear tells drivers
      // (Linux uhci_count_ports) that the port does not exist.
      return 0x7F;
  }
}

void Uhci::write16(unsigned off, uint16_t value) {
  switch (off) {
    case kCmd:
      if (value & kCmdGlobalReset) {
        for (auto& p : ports_) {
          if (p.device) p.device->reset();
        }
        reset_locked();
        cmd_ = kCmdGlobalReset;  // stays set until software clears it
        return;
      }
      if (value & kCmdHcReset) {
        reset_locked();
        return;  // self-clearing
      }
      cmd_ = value & 0x00FF;
      if (cmd_ & kCmdRun) status_ &= ~kStsHalted;
      else status_ |= kStsHalted;
      return;
    case kSts:
      status_ &= ~(value & 0x001F);
      if (value & kStsUsbInt) status2_ = 0;
      update_irq_locked();
      return;
    case kIntr:
      intr_ = value & 0x000F;
      update_irq_locked();
      return;
    case kFrnum:
      if (status_ & kStsHalted) frnum_ = value & 0x07FF;
      return;
    case kFlbase:
      flbase_ = (flbase_ & 0xFFFF0000) | (value & 0xF000);
      return;
    case kFlbase + 2:
      flbase_ = (flbase_ & 0x0000FFFF) | (uint32_t{value} << 16);
      return;
    case kSofmod:
      sofmod_ = static_cast<uint8_t>(value);
      return;
    case kPortsc:
    case kPortsc + 2: {
      Port& p = ports_[(off - kPortsc) / 2];
      // Reset is signalled while PR is set; the device sees it when PR is raised (USB 7.1.7.5).
      if ((value & kPortReset) && !(p.sc & kPortReset) && p.device) p.device->reset();
      p.sc = static_cast<uint16_t>((p.sc & kPortKeep) | (value & ~kPortKeep));
      p.sc &= static_cast<uint16_t>(~(value & (kPortConnectChange | kPortEnableChange)));
      if (!(p.sc & kPortConnect)) p.sc &= ~kPortEnable;  // nothing to enable
      p.sc |= 0x0080;
      return;
    }
    default:
      return;
  }
}

bool Uhci::read_dword(uint32_t gpa, uint32_t* value) { return dma_(gpa, value, 4, false); }
bool Uhci::write_dword(uint32_t gpa, uint32_t value) { return dma_(gpa, &value, 4, true); }

UsbDevice* Uhci::find_device_locked(uint8_t address) {
  for (auto& p : ports_) {
    if (p.device && (p.sc & kPortEnable) && !(p.sc & kPortReset) && p.device->address() == address) {
      return p.device.get();
    }
  }
  return nullptr;
}

void Uhci::frame_thread() {
  LARGE_INTEGER freq{};
  QueryPerformanceFrequency(&freq);
  const uint64_t ticks_per_frame = static_cast<uint64_t>(freq.QuadPart) / 1000;
  uint64_t next = 0;
  bool timer_armed = false;
  std::unique_lock lock(mutex_);
  while (!quit_) {
    if (!(cmd_ & kCmdRun) || !bus_master_) {
      if (timer_armed) CancelWaitableTimer(timer_);
      timer_armed = false;
      next = 0;
      lock.unlock();
      WaitForSingleObject(wake_, INFINITE);
      lock.lock();
      continue;
    }
    LARGE_INTEGER now{};
    QueryPerformanceCounter(&now);
    const uint64_t t = static_cast<uint64_t>(now.QuadPart);
    if (!next) next = t;
    // Frames that are due, a few at most if the host fell behind (the rest are skipped, as if they had
    // been empty: the frame counter still advances so guest timing based on it stays right).
    int frames = 0;
    while (next <= t && frames < kMaxCatchUpFrames && (cmd_ & kCmdRun)) {
      run_frame_locked();
      frnum_ = (frnum_ + 1) & 0x07FF;
      next += ticks_per_frame;
      ++frames;
    }
    if (next <= t) {
      const uint64_t skipped = (t - next) / ticks_per_frame + 1;
      frnum_ = static_cast<uint16_t>((frnum_ + skipped) & 0x07FF);
      next += skipped * ticks_per_frame;
    }
    // USBINT is reported at the end of the frame in which a TD with IOC (or a short packet) completed;
    // drivers check it in USBSTS before claiming the interrupt.
    if (status2_) status_ |= kStsUsbInt;
    update_irq_locked();
    if (!timer_armed) {
      LARGE_INTEGER due{};
      due.QuadPart = -10000;  // 1 ms
      SetWaitableTimer(timer_, &due, 1, nullptr, nullptr, FALSE);
      timer_armed = true;
    }
    lock.unlock();
    HANDLE handles[] = {wake_, timer_};
    WaitForMultipleObjects(2, handles, FALSE, INFINITE);
    lock.lock();
  }
  if (timer_armed) CancelWaitableTimer(timer_);
}

void Uhci::run_frame_locked() {
  uint32_t link = 0;
  if (!read_dword(flbase_ + (frnum_ & 0x3FF) * 4, &link)) {
    status_ |= kStsHostError;
    cmd_ &= ~kCmdRun;
    status_ |= kStsHalted;
    SVM_WARN("uhci: frame list at 0x%x is not in RAM, controller halted", flbase_);
    return;
  }
  int budget = kTdBudget;
  uint32_t seen_qh[32];
  unsigned seen = 0;
  while (!(link & kLinkTerminate) && budget > 0) {
    const uint32_t addr = link & ~0xFu;
    if (link & kLinkQh) {
      if (std::find(seen_qh, seen_qh + seen, addr) != seen_qh + seen || seen == std::size(seen_qh)) break;
      seen_qh[seen++] = addr;
      uint32_t head = 0, element = 0;
      if (!read_dword(addr, &head) || !read_dword(addr + 4, &element)) break;
      run_queue_locked(addr, element, &budget);
      link = head;
    } else {
      --budget;
      uint32_t next = 0;
      if (!read_dword(addr, &next)) break;
      run_td_locked(addr);
      link = next;
    }
  }
}

void Uhci::run_queue_locked(uint32_t qh, uint32_t element, int* budget) {
  while (!(element & kLinkTerminate) && (*budget)-- > 0) {
    if (element & kLinkQh) return;  // nested queue heads: not used by the drivers we know of
    const uint32_t td = element & ~0xFu;
    uint32_t next = 0;
    if (!read_dword(td, &next)) return;
    const TdResult r = run_td_locked(td);
    // A NAK, an error or an inactive TD stops this queue for the frame; a short packet with SPD set
    // stops it without advancing, so the driver can see where the transfer ended.
    if (r != TdResult::Ok) return;
    element = next;
    if (!write_dword(qh + 4, element)) return;
    if (!(next & kLinkDepth)) return;  // breadth first: one TD per queue per frame
  }
}

Uhci::TdResult Uhci::run_td_locked(uint32_t addr) {
  uint32_t ctrl = 0, token = 0, buffer = 0;
  if (!read_dword(addr + 4, &ctrl) || !read_dword(addr + 8, &token) || !read_dword(addr + 12, &buffer)) {
    return TdResult::Error;
  }
  if (!(ctrl & kTdActive)) return TdResult::Inactive;

  const uint8_t pid = static_cast<uint8_t>(token);
  const uint8_t dev_addr = (token >> 8) & 0x7F;
  const uint8_t endpoint = (token >> 15) & 0x0F;
  const size_t max_len = ((token >> 21) + 1) & 0x7FF;  // n-1 encoding, 0x7FF = zero length
  auto fail = [&](uint32_t bits) {
    ctrl = (ctrl & ~(kTdActive | kTdStatusMask | kTdErrorCount)) | bits;
    status_ |= kStsUsbErr;
    if (ctrl & kTdIoc) status2_ |= 1;
    write_dword(addr + 4, ctrl);
    return TdResult::Error;
  };

  UsbDevice* dev = find_device_locked(dev_addr);
  if (!dev || (ctrl & kTdIos) || (pid != 0x2D && pid != 0x69 && pid != 0xE1)) return fail(kTdTimeout);

  data_.resize(std::max<size_t>(max_len, 1));
  size_t len = max_len;
  UsbDevice::Result result;
  if (pid == 0x69) {
    len = 0;
    result = dev->packet(UsbDevice::Pid::In, endpoint, data_.data(), max_len, &len);
    if (result == UsbDevice::Result::Ok && len && !dma_(buffer, data_.data(), len, true)) {
      status_ |= kStsHostError;
      return fail(kTdTimeout);
    }
  } else {
    if (max_len && !dma_(buffer, data_.data(), max_len, false)) {
      status_ |= kStsHostError;
      return fail(kTdTimeout);
    }
    result = dev->packet(pid == 0x2D ? UsbDevice::Pid::Setup : UsbDevice::Pid::Out, endpoint, data_.data(), max_len,
                         &len);
  }
  if (result == UsbDevice::Result::Nak) return TdResult::Nak;  // stays active, retried next frame
  if (result == UsbDevice::Result::Stall) return fail(kTdStall);

  ctrl = (ctrl & ~(kTdActive | kTdStatusMask | 0x7FF)) | ((len - 1) & 0x7FF);
  if (ctrl & kTdIoc) status2_ |= 1;
  const bool short_packet = pid == 0x69 && len < max_len;
  if (short_packet && (ctrl & kTdSpd)) {
    status2_ |= 2;
    write_dword(addr + 4, ctrl);
    return TdResult::Short;
  }
  write_dword(addr + 4, ctrl);
  return TdResult::Ok;
}

}  // namespace sandvm::devices
