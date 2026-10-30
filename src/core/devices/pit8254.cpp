#include "core/devices/pit8254.h"

#include <stdexcept>
#include <utility>

#include "core/log.h"

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

namespace sandvm::devices {
namespace {

// a * b / c without 64-bit overflow, as long as (c - 1) * b fits in 64 bits.
uint64_t mul_div(uint64_t a, uint64_t b, uint64_t c) { return a / c * b + a % c * b / c; }

// The DRAM refresh request bit of port B toggles every 15.085 us on an AT.
constexpr uint64_t kRefreshToggleHz = 66291;

}  // namespace

Pit8254::Pit8254(IrqCallback on_irq0) : on_irq0_(std::move(on_irq0)) {
  LARGE_INTEGER frequency{};
  QueryPerformanceFrequency(&frequency);
  qpc_frequency_ = static_cast<uint64_t>(frequency.QuadPart);

  // Without the high-resolution flag (Windows 10 1803+) waits round up to the ~15.6 ms system tick,
  // which a 1 kHz guest timer cannot live with. Fall back to it rather than fail.
  timer_ = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
  if (!timer_) {
    SVM_WARN("pit: no high-resolution waitable timer, IRQ0 timing will be coarse");
    timer_ = CreateWaitableTimerExW(nullptr, nullptr, 0, TIMER_ALL_ACCESS);
  }
  wake_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  if (!timer_ || !wake_) {
    if (timer_) CloseHandle(timer_);
    if (wake_) CloseHandle(wake_);
    throw std::runtime_error("pit: cannot create timer objects");
  }

  const uint64_t t = now();
  for (Channel& c : channels_) c.start = t;
  channels_[2].gate = false;  // port B bit 0 resets to 0; channels 0 and 1 have their gates tied high
  thread_ = std::thread([this] { timer_thread(); });
}

Pit8254::~Pit8254() {
  stop();
  CloseHandle(timer_);
  CloseHandle(wake_);
}

void Pit8254::stop() {
  {
    std::lock_guard lock(mutex_);
    quit_ = true;
  }
  SetEvent(wake_);
  if (thread_.joinable()) thread_.join();
}

uint64_t Pit8254::now() const {
  LARGE_INTEGER counter{};
  QueryPerformanceCounter(&counter);
  return static_cast<uint64_t>(counter.QuadPart);
}

uint64_t Pit8254::elapsed_ticks(const Channel& c, uint64_t now) const {
  if (!c.running || now <= c.start) return c.base_ticks;
  return c.base_ticks + mul_div(now - c.start, kFrequencyHz, qpc_frequency_);
}

uint16_t Pit8254::current_count(const Channel& c, uint64_t now) const {
  if (!c.armed) return static_cast<uint16_t>(c.count);
  const uint64_t n = c.count;
  const uint64_t d = elapsed_ticks(c, now);
  switch (c.mode) {
    case 2: return static_cast<uint16_t>(n - d % n);
    case 3: return static_cast<uint16_t>(n - (2 * d) % n);  // mode 3 decrements by two
    default: return static_cast<uint16_t>(n - d);           // 0, 1, 4, 5 keep wrapping after terminal count
  }
}

bool Pit8254::output(const Channel& c, uint64_t now) const {
  // After a control word OUT is low in mode 0 and high in every other mode until counting starts.
  if (!c.armed) return c.mode != 0;
  // A low gate forces OUT high in modes 2 and 3.
  if (!c.gate && (c.mode == 2 || c.mode == 3)) return true;
  const uint64_t n = c.count;
  const uint64_t d = elapsed_ticks(c, now);
  switch (c.mode) {
    case 0:
    case 1: return d >= n;
    case 2: return d % n != n - 1;  // low for the one clock in which the count is 1
    case 3: return d % n < (n + 1) / 2;
    default: return d != n;  // 4, 5: one-clock low strobe at terminal count
  }
}

std::optional<uint64_t> Pit8254::next_rising_edge(const Channel& c, uint64_t ticks) {
  const uint64_t n = c.count;
  switch (c.mode) {
    case 0:
      if (ticks < n) return n;
      return std::nullopt;
    case 2:
    case 3: return (ticks / n + 1) * n;
    case 4:
      if (ticks < n + 1) return n + 1;
      return std::nullopt;
    default: return std::nullopt;  // 1, 5: need a gate trigger, and the channel 0 gate is tied high
  }
}

uint32_t Pit8254::pio_read(uint16_t port, unsigned size) {
  std::lock_guard lock(mutex_);
  const uint64_t t = now();
  uint32_t value = 0;
  for (unsigned i = 0; i < size && i < 4; ++i) value |= uint32_t{read_port(static_cast<uint16_t>(port + i), t)} << (8 * i);
  return value;
}

void Pit8254::pio_write(uint16_t port, unsigned size, uint32_t value) {
  bool rearmed = false;
  {
    std::lock_guard lock(mutex_);
    const uint64_t t = now();
    for (unsigned i = 0; i < size && i < 4; ++i) {
      rearmed |= write_port(static_cast<uint16_t>(port + i), static_cast<uint8_t>(value >> (8 * i)), t);
    }
  }
  if (rearmed) SetEvent(wake_);
}

uint8_t Pit8254::read_port(uint16_t port, uint64_t now) {
  if (port == kPortB) {
    uint8_t value = port_b_ & 0x0F;
    if (mul_div(now, kRefreshToggleHz, qpc_frequency_) & 1) value |= 0x10;
    if (output(channels_[2], now)) value |= 0x20;
    return value;
  }
  if (port < kBase || port >= kBase + 3) return 0xFF;  // 0x43 is write-only

  Channel& c = channels_[port - kBase];
  if (c.status_latched) {
    c.status_latched = false;
    return c.status;
  }
  if (c.latch_remaining) {
    // LSB first when both bytes are latched (rw = 3); otherwise the single byte rw selects.
    const bool msb = c.rw == 2 || (c.rw == 3 && c.latch_remaining == 1);
    --c.latch_remaining;
    if (c.rw != 3) c.latch_remaining = 0;
    return static_cast<uint8_t>(msb ? c.latched_count >> 8 : c.latched_count);
  }
  const uint16_t count = current_count(c, now);
  bool msb = c.rw == 2;
  if (c.rw == 3) {
    msb = c.read_msb_next;
    c.read_msb_next = !c.read_msb_next;
  }
  return static_cast<uint8_t>(msb ? count >> 8 : count);
}

bool Pit8254::write_port(uint16_t port, uint8_t value, uint64_t now) {
  if (port == kPortB) {
    port_b_ = value & 0x0F;
    Channel& c = channels_[2];
    const bool gate = (value & 0x01) != 0;
    if (gate != c.gate) {
      if (c.mode == 0 || c.mode == 4) {
        // GATE low suspends counting in modes 0 and 4, GATE high resumes it from where it stopped.
        // TSC calibration loops load channel 2 with GATE low and then raise it, so this matters.
        if (!gate) c.base_ticks = elapsed_ticks(c, now);
        c.running = gate;
        c.start = now;
      } else if (gate && c.loaded) {
        // A rising GATE (re)triggers modes 1 and 5 and restarts the period in modes 2 and 3.
        c.armed = true;
        c.start = now;
        c.base_ticks = 0;
        c.running = true;
      }
    }
    c.gate = gate;
    return false;
  }
  if (port == kBase + 3) return write_control(value, now);
  if (port >= kBase && port < kBase + 3) return write_counter(port - kBase, value, now);
  return false;
}

bool Pit8254::write_control(uint8_t value, uint64_t now) {
  const unsigned select = value >> 6;
  if (select == 3) {  // read-back: bit 5 = 0 latches counts, bit 4 = 0 latches status, bits 1-3 pick channels
    for (unsigned i = 0; i < 3; ++i) {
      if (!(value & (2u << i))) continue;
      if (!(value & 0x20)) latch_count(channels_[i], now);
      if (!(value & 0x10)) latch_status(channels_[i], now);
    }
    return false;
  }

  Channel& c = channels_[select];
  const unsigned rw = (value >> 4) & 3;
  if (rw == 0) {  // counter latch command
    latch_count(c, now);
    return false;
  }
  c.rw = static_cast<uint8_t>(rw);
  c.mode = (value >> 1) & 7;
  if (c.mode > 5) c.mode -= 4;  // 6 and 7 are aliases of modes 2 and 3
  c.bcd = (value & 1) != 0;
  if (c.bcd && !bcd_warned_) {
    bcd_warned_ = true;
    SVM_WARN("pit: BCD counting requested, counting in binary");
  }
  c.loaded = false;
  c.armed = false;
  c.null_count = true;
  c.write_msb_next = false;
  c.read_msb_next = false;
  c.latch_remaining = 0;
  c.status_latched = false;
  if (select == 0) rearm_irq0(now);
  return select == 0;
}

bool Pit8254::write_counter(unsigned index, uint8_t value, uint64_t now) {
  Channel& c = channels_[index];
  switch (c.rw) {
    case 1: load_count(c, value, now); break;
    case 2: load_count(c, uint32_t{value} << 8, now); break;
    default:
      if (!c.write_msb_next) {
        c.write_lsb = value;
        c.write_msb_next = true;
        return false;
      }
      c.write_msb_next = false;
      load_count(c, c.write_lsb | (uint32_t{value} << 8), now);
      break;
  }
  if (index == 0) rearm_irq0(now);
  return index == 0;
}

void Pit8254::load_count(Channel& c, uint32_t value, uint64_t now) {
  // A new count takes effect at once. Real modes 2 and 3 wait for the end of the current period;
  // guests reprogram the PIT rarely enough that the difference is not visible.
  c.count = value ? value : 0x10000;
  c.loaded = true;
  c.null_count = false;
  c.armed = c.mode != 1 && c.mode != 5;  // hardware-triggered modes wait for a gate edge
  c.start = now;
  c.base_ticks = 0;
  c.running = (c.mode != 0 && c.mode != 4) || c.gate;  // modes 0 and 4 only count while GATE is high
}

void Pit8254::latch_count(Channel& c, uint64_t now) {
  if (c.latch_remaining) return;  // a second latch before the first is read is ignored
  c.latched_count = current_count(c, now);
  c.latch_remaining = c.rw == 3 ? 2 : 1;
}

void Pit8254::latch_status(Channel& c, uint64_t now) {
  if (c.status_latched) return;
  c.status = static_cast<uint8_t>((output(c, now) ? 0x80 : 0) | (c.null_count ? 0x40 : 0) | (c.rw << 4) |
                                  (c.mode << 1) | (c.bcd ? 1 : 0));
  c.status_latched = true;
}

void Pit8254::rearm_irq0(uint64_t now) {
  const Channel& c = channels_[0];
  next_irq0_edge_ = c.armed ? next_rising_edge(c, elapsed_ticks(c, now)) : std::nullopt;
}

void Pit8254::timer_thread() {
  std::unique_lock lock(mutex_);
  while (!quit_) {
    const Channel& c = channels_[0];
    const uint64_t t = now();
    std::optional<uint64_t> deadline;  // QPC
    bool fire = false;
    if (c.armed && c.running && next_irq0_edge_) {
      const uint64_t ticks = elapsed_ticks(c, t);
      if (ticks >= *next_irq0_edge_) {
        fire = true;
        next_irq0_edge_ = next_rising_edge(c, ticks);  // skips periods we were too late for
      }
      // +1 so that rounding never wakes us a hair before the edge and makes us spin.
      if (next_irq0_edge_) {
        const uint64_t ticks_from_start = *next_irq0_edge_ - c.base_ticks;  // base_ticks is 0 on channel 0
        deadline = c.start + mul_div(ticks_from_start, qpc_frequency_, kFrequencyHz) + 1;
      }
    }
    lock.unlock();

    if (fire && on_irq0_) {
      // IRQ0 is edge-triggered (ELCR cannot make it level), so a pulse is what the PIC needs.
      on_irq0_(true);
      on_irq0_(false);
    }
    if (deadline) {
      const uint64_t wait_qpc = *deadline > t ? *deadline - t : 0;
      LARGE_INTEGER due{};
      due.QuadPart = -static_cast<LONGLONG>(mul_div(wait_qpc, 10'000'000, qpc_frequency_));  // relative, 100 ns
      if (due.QuadPart == 0) due.QuadPart = -1;
      SetWaitableTimer(timer_, &due, 0, nullptr, nullptr, FALSE);
      HANDLE handles[] = {wake_, timer_};
      WaitForMultipleObjects(2, handles, FALSE, INFINITE);
    } else if (!fire) {
      WaitForSingleObject(wake_, INFINITE);
    }
    lock.lock();
  }
}

}  // namespace sandvm::devices
