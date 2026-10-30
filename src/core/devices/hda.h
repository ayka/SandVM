#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <mutex>
#include <vector>

namespace sandvm::devices {

// Intel High Definition Audio controller (ICH6 register set, "High Definition Audio Specification"
// rev 1.0a, chapter 3) with one output-only codec at address 0 (chapter 7 verbs and parameters):
//   NID 0 root -> NID 1 audio function group -> NID 2 stereo DAC (16-bit, 44.1 / 48 kHz, output amp
//   with mute) -> NID 3 line-out pin.
// Windows (hdaudbus + the class driver, since Vista) and Linux (snd-hda-intel + the generic codec
// parser) drive it without vendor drivers.
//
// Codec commands go through the CORB/RIRB rings or the immediate command registers and are answered
// synchronously. Stream data is pulled, not pushed: the host audio backend calls pull() as it needs
// samples, which walks the bound output stream's buffer descriptor list in guest memory, advances its
// link position (and the DMA position buffer) and raises buffer-completion interrupts, so the guest's
// playback is paced by the host's sound card. Input streams exist (the ICH6 has four) but no codec
// widget can record, so guests never start them.
//
// Thread-safe. The IRQ callback is called with the controller lock held (device -> PIC order).
class Hda {
 public:
  static constexpr uint32_t kMmioSize = 0x4000;

  using IrqCallback = std::function<void(bool level)>;
  using DmaMemory = std::function<bool(uint64_t gpa, void* data, size_t len, bool to_guest)>;

  struct Format {
    uint32_t rate = 48000;
    unsigned channels = 2;
    bool running = false;  // an output stream bound to the DAC is running
  };

  Hda(IrqCallback on_irq, DmaMemory dma);

  uint32_t read(uint32_t offset, unsigned size);
  void write(uint32_t offset, unsigned size, uint32_t value);
  void set_bus_master(bool enabled);

  // What the host should play now.
  Format output_format();
  // Fills `frames` interleaved 16-bit frames in output_format()'s channel count from the playing
  // stream (silence while none runs, or the guest muted the DAC). Called from the audio thread.
  void pull(int16_t* out, size_t frames);

 private:
  static constexpr unsigned kInputStreams = 4, kOutputStreams = 4, kStreams = 8;
  struct Stream {
    uint32_t ctl = 0;  // SDnCTL, 24 bits
    uint8_t sts = 0;
    uint32_t lpib = 0;
    uint32_t cbl = 0;
    uint16_t lvi = 0;
    uint16_t fmt = 0;
    uint64_t bdl = 0;
    // DMA progress through the buffer descriptor list.
    uint32_t bde = 0;         // current entry
    uint32_t bde_offset = 0;  // bytes consumed in it
  };

  void reset_locked();
  void update_irq_locked();
  void run_corb_locked();
  uint32_t codec_verb(uint32_t verb);
  uint32_t parameter(unsigned nid, unsigned id);
  bool stream_running(unsigned n) const { return streams_[n].ctl & 0x2; }
  int output_stream_locked();  // the running output stream whose tag the DAC listens to, or -1
  bool consume_locked(unsigned n, uint8_t* out, uint32_t bytes);
  void write_position_locked(unsigned n);
  static uint32_t format_rate(uint16_t fmt);

  const IrqCallback on_irq_;
  const DmaMemory dma_;

  std::mutex mutex_;
  bool bus_master_ = false;
  bool irq_line_ = false;
  uint32_t gctl_ = 0;
  uint16_t wakeen_ = 0, statests_ = 0;
  uint32_t intctl_ = 0;
  uint64_t corb_base_ = 0, rirb_base_ = 0;
  uint16_t corb_wp_ = 0, corb_rp_ = 0, rirb_wp_ = 0, rintcnt_ = 0;
  uint8_t corb_ctl_ = 0, corb_sts_ = 0, corb_size_ = 0x42, rirb_ctl_ = 0, rirb_sts_ = 0, rirb_size_ = 0x42;
  unsigned rirb_count_ = 0;  // responses since RIRBSTS.RINTFL was last raised
  uint32_t icoi_ = 0, icii_ = 0;
  uint16_t icis_ = 0;
  uint64_t dpl_base_ = 0;  // bit 0: position buffer enabled
  std::array<Stream, kStreams> streams_{};
  uint64_t wall_start_ = 0;  // QPC at the last reset, for the 24 MHz wall clock

  // Codec state.
  uint8_t dac_stream_ = 0, dac_channel_ = 0;  // SET_CONV_STREAM_CHANNEL
  uint16_t dac_format_ = 0x0011;              // SET_CONV_FORMAT: 48 kHz, 16-bit, stereo
  uint8_t amp_gain_[2] = {kAmpSteps, kAmpSteps};
  bool amp_mute_[2] = {false, false};
  uint8_t pin_ctl_ = 0x40;  // output enabled
  uint8_t eapd_ = 0x02;
  uint8_t power_state_[4] = {};
  static constexpr uint8_t kAmpSteps = 0x4A;

  std::vector<uint8_t> scratch_;
};

}  // namespace sandvm::devices
