#include "core/devices/cmos.h"

#include <windows.h>

#include <algorithm>
#include <limits>
#include <optional>
#include <stdexcept>
#include <utility>

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

namespace sandvm::devices {
namespace {
constexpr uint8_t kSeconds = 0x00;
constexpr uint8_t kSecondsAlarm = 0x01;
constexpr uint8_t kMinutes = 0x02;
constexpr uint8_t kMinutesAlarm = 0x03;
constexpr uint8_t kHours = 0x04;
constexpr uint8_t kHoursAlarm = 0x05;
constexpr uint8_t kWeekday = 0x06;
constexpr uint8_t kDayOfMonth = 0x07;
constexpr uint8_t kMonth = 0x08;
constexpr uint8_t kYear = 0x09;
constexpr uint8_t kStatusA = 0x0A;
constexpr uint8_t kStatusB = 0x0B;
constexpr uint8_t kStatusC = 0x0C;
constexpr uint8_t kStatusD = 0x0D;
constexpr uint8_t kCentury = 0x32;

constexpr uint8_t kStatusAUpdateInProgress = 0x80;
constexpr uint8_t kStatusB24Hour = 0x02;
constexpr uint8_t kStatusBBinary = 0x04;
constexpr uint8_t kStatusBSet = 0x80;
constexpr uint8_t kStatusBUpdateIrq = 0x10;
constexpr uint8_t kStatusBAlarmIrq = 0x20;
constexpr uint8_t kStatusBPeriodicIrq = 0x40;
constexpr uint8_t kStatusCIrq = 0x80;
constexpr uint8_t kStatusCPeriodic = 0x40;
constexpr uint8_t kStatusCAlarm = 0x20;
constexpr uint8_t kStatusCUpdate = 0x10;
constexpr uint8_t kSourceMask = 0x70;  // PF, AF, UF in C line up with PIE, AIE, UIE in B
constexpr uint8_t kDividerMask = 0x70;
constexpr uint8_t kDividerRunning = 0x20;  // 32.768 kHz time base, oscillator on, divider not in reset
constexpr uint64_t kRtcHz = 32768;
constexpr uint64_t kFileTimeHz = 10'000'000;

constexpr uint64_t kKiB = 1024;
constexpr uint64_t kMiB = 1024 * kKiB;
constexpr uint64_t kFourGiB = 4ull * 1024 * kMiB;

uint64_t qpc_now() {
  LARGE_INTEGER v{};
  QueryPerformanceCounter(&v);
  return static_cast<uint64_t>(v.QuadPart);
}

uint64_t filetime_now() {  // 100 ns units
  FILETIME ft{};
  GetSystemTimeAsFileTime(&ft);
  return (uint64_t{ft.dwHighDateTime} << 32) | ft.dwLowDateTime;
}

uint64_t mul_div(uint64_t a, uint64_t b, uint64_t c) {
  return static_cast<uint64_t>(static_cast<unsigned __int128>(a) * b / c);
}

void store_u16(std::array<uint8_t, 128>& cmos, uint8_t index, uint64_t value) {
  value = std::min<uint64_t>(value, std::numeric_limits<uint16_t>::max());
  cmos[index] = static_cast<uint8_t>(value);
  cmos[index + 1] = static_cast<uint8_t>(value >> 8);
}
}  // namespace

void CmosRtc::set_nvram(uint8_t index, uint8_t value) {
  if (index < 0x0E || index > 0x7F) return;  // clock and status registers are not settings
  std::lock_guard lock(mutex_);
  cmos_[index] = value;
}

CmosRtc::CmosRtc(uint64_t ram_below_4g, uint64_t ram_above_4g, IrqCallback on_irq) : on_irq_(std::move(on_irq)) {
  cmos_[kStatusA] = 0x26;  // 32.768 kHz divider, 1024 Hz periodic rate
  cmos_[kStatusB] = kStatusB24Hour;
  cmos_[kStatusD] = 0x80;  // CMOS battery/power good

  // These fields follow the de-facto PC layout consumed by SeaBIOS and other PC firmware.
  store_u16(cmos_, 0x15, std::min<uint64_t>(ram_below_4g / kKiB, 640));

  const uint64_t extended_kib = ram_below_4g > kMiB ? (ram_below_4g - kMiB) / kKiB : 0;
  store_u16(cmos_, 0x17, extended_kib);
  store_u16(cmos_, 0x30, extended_kib);

  const uint64_t below_4g = std::min(ram_below_4g, kFourGiB);
  const uint64_t above_16m_blocks = below_4g > 16 * kMiB ? (below_4g - 16 * kMiB) / (64 * kKiB) : 0;
  store_u16(cmos_, 0x34, above_16m_blocks);

  const uint64_t above_4g_blocks = ram_above_4g / (64 * kKiB);
  const uint64_t capped_above_4g = std::min<uint64_t>(above_4g_blocks, 0xFFFFFF);
  cmos_[0x5B] = static_cast<uint8_t>(capped_above_4g);
  cmos_[0x5C] = static_cast<uint8_t>(capped_above_4g >> 8);
  cmos_[0x5D] = static_cast<uint8_t>(capped_above_4g >> 16);

  LARGE_INTEGER frequency{};
  QueryPerformanceFrequency(&frequency);
  qpc_frequency_ = static_cast<uint64_t>(frequency.QuadPart);
  periodic_start_ = qpc_now();
  second_seen_ = filetime_now() / kFileTimeHz;

  timer_ = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
  if (!timer_) timer_ = CreateWaitableTimerExW(nullptr, nullptr, 0, TIMER_ALL_ACCESS);
  wake_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  if (!timer_ || !wake_) {
    if (timer_) CloseHandle(timer_);
    if (wake_) CloseHandle(wake_);
    throw std::runtime_error("cmos: cannot create timer objects");
  }
  thread_ = std::thread([this] { timer_thread(); });
}

CmosRtc::~CmosRtc() {
  stop();
  CloseHandle(timer_);
  CloseHandle(wake_);
}

void CmosRtc::stop() {
  {
    std::lock_guard lock(mutex_);
    quit_ = true;
  }
  SetEvent(wake_);
  if (thread_.joinable()) thread_.join();
}

uint64_t CmosRtc::periodic_ticks_locked() const {
  // Rate select (status A bits 3:0) divides the 32.768 kHz time base: 1 and 2 give 256 and 128 Hz
  // (the 3.90625 / 7.8125 ms special cases), n >= 3 gives 32768 >> (n - 1) Hz; 0 is off.
  const unsigned rs = cmos_[kStatusA] & 0x0F;
  if (rs == 0) return 0;
  const uint64_t cycles = rs <= 2 ? (uint64_t{1} << (rs + 6)) : (uint64_t{1} << (rs - 1));
  return std::max<uint64_t>(1, mul_div(cycles, qpc_frequency_, kRtcHz));
}

bool CmosRtc::alarm_matches_locked() {
  // Alarm bytes with the two top bits set (0xC0-0xFF) match any value.
  auto match = [this](uint8_t alarm_index, uint8_t time_index) {
    const uint8_t alarm = cmos_[alarm_index];
    return alarm >= 0xC0 || alarm == read_register_locked(time_index);
  };
  return match(kSecondsAlarm, kSeconds) && match(kMinutesAlarm, kMinutes) && match(kHoursAlarm, kHours);
}

void CmosRtc::update_locked(uint64_t now) {
  if ((cmos_[kStatusA] & kDividerMask) == kDividerRunning) {
    if (const uint64_t period = periodic_ticks_locked()) {
      const uint64_t elapsed = now > periodic_start_ ? (now - periodic_start_) / period : 0;
      if (elapsed > periodic_seen_) {
        cmos_[kStatusC] |= kStatusCPeriodic;  // several missed periods coalesce into one
        periodic_seen_ = elapsed;
      }
    }
    const uint64_t second = filetime_now() / kFileTimeHz;
    if (second != second_seen_) {
      second_seen_ = second;
      // No update cycle runs while SET holds the clock.
      if (!(cmos_[kStatusB] & kStatusBSet)) {
        cmos_[kStatusC] |= kStatusCUpdate;
        if (alarm_matches_locked()) cmos_[kStatusC] |= kStatusCAlarm;
      }
    }
  }
  if (cmos_[kStatusC] & cmos_[kStatusB] & kSourceMask) cmos_[kStatusC] |= kStatusCIrq;
  const bool level = (cmos_[kStatusC] & kStatusCIrq) != 0;
  if (level != irq_line_) {
    irq_line_ = level;
    if (on_irq_) on_irq_(level);
  }
}

void CmosRtc::timer_thread() {
  std::unique_lock lock(mutex_);
  while (!quit_) {
    const uint64_t t = qpc_now();
    update_locked(t);
    std::optional<uint64_t> deadline;  // QPC
    if ((cmos_[kStatusA] & kDividerMask) == kDividerRunning) {
      const uint64_t period = periodic_ticks_locked();
      if ((cmos_[kStatusB] & kStatusBPeriodicIrq) && period) {
        // +1 so that rounding never wakes us a hair before the edge and makes us spin.
        deadline = periodic_start_ + (periodic_seen_ + 1) * period + 1;
      }
      if (cmos_[kStatusB] & (kStatusBUpdateIrq | kStatusBAlarmIrq)) {
        const uint64_t to_next_second = kFileTimeHz - filetime_now() % kFileTimeHz;
        const uint64_t at = t + mul_div(to_next_second, qpc_frequency_, kFileTimeHz) + 1;
        deadline = deadline ? std::min(*deadline, at) : at;
      }
    }
    lock.unlock();
    if (deadline) {
      const uint64_t wait_qpc = *deadline > t ? *deadline - t : 0;
      LARGE_INTEGER due{};
      due.QuadPart = -static_cast<LONGLONG>(mul_div(wait_qpc, kFileTimeHz, qpc_frequency_));  // relative
      if (due.QuadPart == 0) due.QuadPart = -1;
      SetWaitableTimer(timer_, &due, 0, nullptr, nullptr, FALSE);
      HANDLE handles[] = {wake_, timer_};
      WaitForMultipleObjects(2, handles, FALSE, INFINITE);
    } else {
      WaitForSingleObject(wake_, INFINITE);
    }
    lock.lock();
  }
}

bool CmosRtc::nmi_masked() const {
  std::lock_guard lock(mutex_);
  return nmi_masked_;
}

uint32_t CmosRtc::pio_read(uint16_t port, unsigned size) {
  std::lock_guard lock(mutex_);
  uint32_t value = 0;
  for (unsigned i = 0; i < size && i < 4; ++i) {
    const uint32_t current_port = static_cast<uint32_t>(port) + i;
    const uint8_t byte = current_port == kDataPort ? read_register_locked(index_) : 0xFF;
    value |= uint32_t{byte} << (8 * i);
  }
  return value;
}

void CmosRtc::pio_write(uint16_t port, unsigned size, uint32_t value) {
  std::lock_guard lock(mutex_);
  for (unsigned i = 0; i < size && i < 4; ++i) {
    const uint32_t current_port = static_cast<uint32_t>(port) + i;
    const uint8_t byte = static_cast<uint8_t>(value >> (8 * i));
    if (current_port == kIndexPort) {
      index_ = byte & 0x7F;
      nmi_masked_ = (byte & 0x80) != 0;
    } else if (current_port == kDataPort) {
      write_register_locked(index_, byte);
    }
  }
}

uint8_t CmosRtc::read_register_locked(uint8_t index) {
  if (index == kStatusA) {
    SYSTEMTIME now{};
    GetLocalTime(&now);
    // Host clock resolution cannot reproduce the RTC's brief UIP interval, so expose it
    // during the final millisecond before the visible second changes. No update cycle runs
    // (so no UIP) while SET is held or the divider is stopped.
    const bool updating = now.wMilliseconds == 999 && !(cmos_[kStatusB] & kStatusBSet) &&
                          (cmos_[kStatusA] & kDividerMask) == kDividerRunning;
    return static_cast<uint8_t>((cmos_[kStatusA] & 0x7F) | (updating ? kStatusAUpdateInProgress : 0));
  }
  if (index == kStatusC) {
    // Reading C acknowledges: all flags clear and IRQ8 drops.
    update_locked(qpc_now());
    const uint8_t value = cmos_[kStatusC];
    cmos_[kStatusC] = 0;
    update_locked(qpc_now());
    return value;
  }
  if (index == kStatusD) return 0x80;

  if ((cmos_[kStatusB] & kStatusBSet) == 0) {
    SYSTEMTIME now{};
    GetLocalTime(&now);
    switch (index) {
      case kSeconds: return encode_locked(now.wSecond);
      case kMinutes: return encode_locked(now.wMinute);
      case kHours: {
        if (cmos_[kStatusB] & kStatusB24Hour) return encode_locked(now.wHour);
        const bool pm = now.wHour >= 12;
        const unsigned hour = now.wHour % 12 == 0 ? 12 : now.wHour % 12;
        return static_cast<uint8_t>(encode_locked(hour) | (pm ? 0x80 : 0));
      }
      case kWeekday: return encode_locked(now.wDayOfWeek + 1);
      case kDayOfMonth: return encode_locked(now.wDay);
      case kMonth: return encode_locked(now.wMonth);
      case kYear: return encode_locked(now.wYear % 100);
      case kCentury: return encode_locked(now.wYear / 100);
      default: break;
    }
  }
  return cmos_[index];
}

void CmosRtc::write_register_locked(uint8_t index, uint8_t value) {
  switch (index) {
    case kStatusA:
      if ((value ^ cmos_[kStatusA]) & 0x7F) {
        // A new rate or divider setting restarts the periodic chain.
        periodic_start_ = qpc_now();
        periodic_seen_ = 0;
      }
      cmos_[kStatusA] = value & 0x7F;  // UIP is read-only
      SetEvent(wake_);
      break;
    case kStatusB:
      // Setting SET also clears UIE (MC146818 datasheet, register B bit 7).
      cmos_[kStatusB] = (value & kStatusBSet) ? (value & ~kStatusBUpdateIrq) : value;
      update_locked(qpc_now());  // enabling a source whose flag is already set raises IRQF at once
      SetEvent(wake_);
      break;
    case kStatusC:
    case kStatusD: break;  // read-only
    default: cmos_[index] = value; break;
  }
}

uint8_t CmosRtc::encode_locked(unsigned value) const {
  if (cmos_[kStatusB] & kStatusBBinary) return static_cast<uint8_t>(value);
  return static_cast<uint8_t>(((value / 10) << 4) | (value % 10));
}

}  // namespace sandvm::devices
