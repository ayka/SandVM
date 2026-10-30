// sandvm command line front end.
//
//   sandvm --version
//   sandvm [--sandbox <box>] [-v] caps
//   sandvm [--sandbox <box>] [-v] selftest [--bios <bios.bin>]
//   sandvm [--sandbox <box>] [-v] run --flat <file> [--ram <MiB>] [--load <addr>]
//   sandvm [--sandbox <box>] [-v] run --bios <bios.bin> [--ram <MiB>] [--disk <image>] [--disk-ro]
//   sandvm [--sandbox <box>] [-v] run --kernel <bzImage> [--initrd <file>] [--append "<cmdline>"] [--ram <MiB>]
//                                     [--disk <image>] [--disk-ro]
//   sandvm disk <create|snapshot|info|commit|merge|convert> ...   image tools (cli/disk_cmd.cpp)
//   run also takes --cdrom <iso> (ATAPI drive, secondary master), --apic (hypervisor local APIC + our IOAPIC) and --cpus <N> (N > 1 implies --apic).
//   --disk may be given up to three times (primary master, primary slave, secondary slave); images may be raw,
//   VHD (fixed / dynamic / differencing) or VMDK. --discard-changes puts a temporary differencing image over every
//   disk, deleted at exit (VMware's non-persistent mode). --boot <order> sets the firmware boot order (a floppy,
//   c disk, d CD/DVD, n network, e.g. "dc"). With --bios an empty CD/DVD drive is present when --cdrom is not
//   given, so a disc can be inserted later through the control pipe.
//   --display adds a VGA and shows it in a window (keyboard goes to the guest); with --bios its option ROM
//   comes from --vgabios <file> (default: vgabios-stdvga.bin next to the BIOS). --screenshot <file.bmp> adds a
//   VGA too and writes its picture about once a second (without --display no window is opened). --name <name>
//   sets the window title and the control pipe name. --no-usb disables the USB tablet (on by default with a
//   display).
//   --net adds an e1000 network card behind a user-mode NAT (guest 10.0.2.15, gateway/host 10.0.2.2);
//   --hostfwd tcp:<host port>:<guest port> (repeatable) forwards 127.0.0.1:<host port> to the guest.
//   --control <name> opens the named events Local\sandvm.<name>.power / .stop for a GUI (cli/control_events.h) and
//   the command pipe \\.\pipe\sandvm.<name> (cli/control_pipe.h): power, stop, reset, pause, resume, status,
//   cdrom <iso>, eject, ctrl-alt-del, type <text> (US layout, \n = Enter), mouse <x> <y> [buttons] (USB tablet,
//   0..32767).
//
// Guest output: COM1 and the 0x402 debug console both go to stdout; stdin goes to COM1. Ctrl+C stops the guest.
//
// --sandbox re-launches the process inside a Sandboxie-Plus box (see src/sbie).

#include <windows.h>
#include <shellapi.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "cli/console_input.h"
#include "cli/control_events.h"
#include "cli/control_pipe.h"
#include "cli/disk_cmd.h"
#include "cli/keymap.h"
#include "cli/display_window.h"
#include "cli/selftest.h"
#include "core/block/disk.h"
#include "core/block/image.h"
#include "core/devices/debugcon.h"
#include "core/devices/e1000.h"
#include "core/devices/i8042.h"
#include "core/devices/ide.h"
#include "core/devices/serial16550.h"
#include "core/devices/usb.h"
#include "core/devices/vga.h"
#include "core/loader/linux.h"
#include "core/log.h"
#include "core/version.h"
#include "core/net/user_nat.h"
#include "core/vm.h"
#include "core/whpx.h"
#include "sbie/launcher.h"

namespace {

using namespace sandvm;

// Ctrl+C / console close stops the running VM: a guest idling in STI; HLT never stops on its own.
// The handler runs on its own thread, so the pointer is only used under the mutex.
std::mutex g_vm_mutex;
Vm* g_running_vm = nullptr;

BOOL WINAPI on_console_ctrl(DWORD type) {
  if (type != CTRL_C_EVENT && type != CTRL_BREAK_EVENT && type != CTRL_CLOSE_EVENT) return FALSE;
  std::lock_guard lock(g_vm_mutex);
  if (!g_running_vm) return FALSE;
  g_running_vm->request_stop();
  return TRUE;
}

struct StopOnCtrlC {
  explicit StopOnCtrlC(Vm& vm) {
    std::lock_guard lock(g_vm_mutex);
    g_running_vm = &vm;
    SetConsoleCtrlHandler(on_console_ctrl, TRUE);
  }
  ~StopOnCtrlC() {
    std::lock_guard lock(g_vm_mutex);
    SetConsoleCtrlHandler(on_console_ctrl, FALSE);
    g_running_vm = nullptr;
  }
  StopOnCtrlC(const StopOnCtrlC&) = delete;
  StopOnCtrlC& operator=(const StopOnCtrlC&) = delete;
};

// Publishes the machine of the current reboot-loop iteration to the control pipe for its lifetime.
struct CurrentVm {
  std::mutex& mutex;
  Vm*& vm;
  CurrentVm(std::mutex& m, Vm*& slot, Vm& v) : mutex(m), vm(slot) {
    std::lock_guard lock(mutex);
    vm = &v;
  }
  ~CurrentVm() {
    std::lock_guard lock(mutex);
    vm = nullptr;
  }
  CurrentVm(const CurrentVm&) = delete;
  CurrentVm& operator=(const CurrentVm&) = delete;
};

// Disk images of a run; temporary ones (--discard-changes) are deleted after being closed.
struct TempDisks {
  std::vector<std::shared_ptr<block::Disk>> images;
  std::vector<std::wstring> paths;
  ~TempDisks() {
    images.clear();
    for (const auto& p : paths) DeleteFileW(p.c_str());
  }
};

int usage() {
  std::fprintf(stderr,
               "usage: sandvm [--sandbox <box>] [-v] <command>\n"
               "       sandvm --version\n"
               "  caps                                   show WHPX capabilities\n"
               "  selftest [--bios <bios.bin>]           built-in guests; with --bios also boot firmware from a disk\n"
               "  run --flat <file> [--ram MiB] [--load addr]  run a flat binary in real mode\n"
               "  run --bios <bios.bin> [--ram MiB] [--disk img] [--disk-ro]\n"
               "                                         boot PC firmware (e.g. SeaBIOS); --disk = IDE primary master\n"
               "  run --kernel <bzImage> [--initrd f] [--append cmdline] [--ram MiB] [--disk img] [--disk-ro]\n"
               "                                         boot Linux directly (64-bit boot protocol), console on COM1\n"
               "  disk <create|snapshot|info|commit|merge|convert> ...  disk image tools ('sandvm disk' for help)\n"
               "  ctl <name> <command>                   send a command to a machine started with --control <name>:\n"
               "                                         power, stop, reset, pause, resume, status, cdrom <iso>,\n"
               "                                         eject, ctrl-alt-del, type <text>, mouse <x> <y> [buttons]\n"
               "  run options: --cdrom <iso>, --apic (local APIC + IOAPIC), --cpus N (N > 1 implies --apic),\n"
               "               --disk up to 3 times (raw, VHD, VMDK), --discard-changes, --boot <order of a c d n>,\n"
               "               --display (VGA in a window), --vgabios <file>, --screenshot <file.bmp>,\n"
               "               --name <name> (window title and control pipe), --no-usb (no USB tablet),\n"
               "               --net (e1000 + user-mode NAT), --hostfwd tcp:<host port>:<guest port>,\n"
               "               --control <name> (named events and command pipe for a GUI)\n");
  return 2;
}

std::vector<std::wstring> wide_args() {
  int argc = 0;
  LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
  std::vector<std::wstring> args;
  for (int i = 1; i < argc; ++i) args.emplace_back(argv[i]);
  LocalFree(argv);
  return args;
}

int cmd_caps() {
  const auto& w = whpx::api();
  WHV_CAPABILITY cap{};
  UINT32 written = 0;

  whpx::check(w.WHvGetCapability(WHvCapabilityCodeHypervisorPresent, &cap, sizeof(cap), &written), "HypervisorPresent");
  std::printf("hypervisor present : %s\n", cap.HypervisorPresent ? "yes" : "no");
  if (!cap.HypervisorPresent) return 1;

  cap = {};
  whpx::check(w.WHvGetCapability(WHvCapabilityCodeFeatures, &cap, sizeof(cap), &written), "Features");
  std::printf("features           : 0x%016llx\n", static_cast<unsigned long long>(cap.Features.AsUINT64));

  cap = {};
  whpx::check(w.WHvGetCapability(WHvCapabilityCodeExtendedVmExits, &cap, sizeof(cap), &written), "ExtendedVmExits");
  std::printf("extended vm exits  : 0x%016llx\n", static_cast<unsigned long long>(cap.ExtendedVmExits.AsUINT64));

  cap = {};
  whpx::check(w.WHvGetCapability(WHvCapabilityCodeProcessorVendor, &cap, sizeof(cap), &written), "ProcessorVendor");
  const char* vendor = cap.ProcessorVendor == WHvProcessorVendorAmd     ? "AMD"
                       : cap.ProcessorVendor == WHvProcessorVendorIntel ? "Intel"
                                                                        : "other";
  std::printf("processor vendor   : %s\n", vendor);
  return 0;
}

bool read_file(const char* path, std::vector<uint8_t>* out) {
  std::ifstream file(path, std::ios::binary);
  if (!file) {
    SVM_ERROR("cannot open %s", path);
    return false;
  }
  out->assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
  return true;
}

// argv is in the ANSI code page; Win32 file APIs want UTF-16.
std::wstring widen(const char* s) {
  const int n = MultiByteToWideChar(CP_ACP, 0, s, -1, nullptr, 0);
  std::wstring out(n > 0 ? n - 1 : 0, L'\0');
  if (n > 1) MultiByteToWideChar(CP_ACP, 0, s, -1, out.data(), n);
  return out;
}

int cmd_selftest(int argc, char** argv) {
  std::vector<uint8_t> firmware;
  for (int i = 0; i < argc; ++i) {
    if (!std::strcmp(argv[i], "--bios") && i + 1 < argc) {
      if (!read_file(argv[++i], &firmware)) return 1;
    } else {
      return usage();
    }
  }
  return cli::run_selftest(firmware);
}

int cmd_run(int argc, char** argv) {
  const char* flat = nullptr;
  const char* bios = nullptr;
  const char* kernel = nullptr;
  const char* initrd = nullptr;
  std::string append = "console=ttyS0 earlyprintk=serial,ttyS0 panic=-1";
  std::vector<const char*> disks;
  const char* cdrom = nullptr;
  std::string boot_order;
  bool discard_changes = false;
  const char* vgabios = nullptr;
  const char* screenshot = nullptr;
  const char* control = nullptr;
  bool display = false;
  bool usb = true;
  const char* name = nullptr;
  bool net = false;
  std::vector<net::UserNat::PortForward> forwards;
  bool disk_read_only = false;
  bool local_apic = false;
  uint32_t cpus = 1;
  uint64_t ram_mib = 0;
  uint64_t load = 0x7C00;
  for (int i = 0; i < argc; ++i) {
    if (!std::strcmp(argv[i], "--flat") && i + 1 < argc) flat = argv[++i];
    else if (!std::strcmp(argv[i], "--bios") && i + 1 < argc) bios = argv[++i];
    else if (!std::strcmp(argv[i], "--ram") && i + 1 < argc) ram_mib = std::strtoull(argv[++i], nullptr, 0);
    else if (!std::strcmp(argv[i], "--load") && i + 1 < argc) load = std::strtoull(argv[++i], nullptr, 0);
    else if (!std::strcmp(argv[i], "--kernel") && i + 1 < argc) kernel = argv[++i];
    else if (!std::strcmp(argv[i], "--initrd") && i + 1 < argc) initrd = argv[++i];
    else if (!std::strcmp(argv[i], "--append") && i + 1 < argc) append = argv[++i];
    else if (!std::strcmp(argv[i], "--disk") && i + 1 < argc) disks.push_back(argv[++i]);
    else if (!std::strcmp(argv[i], "--discard-changes")) discard_changes = true;
    else if (!std::strcmp(argv[i], "--boot") && i + 1 < argc) boot_order = argv[++i];
    else if (!std::strcmp(argv[i], "--disk-ro")) disk_read_only = true;
    else if (!std::strcmp(argv[i], "--cdrom") && i + 1 < argc) cdrom = argv[++i];
    else if (!std::strcmp(argv[i], "--apic")) local_apic = true;
    else if (!std::strcmp(argv[i], "--display")) display = true;
    else if (!std::strcmp(argv[i], "--vgabios") && i + 1 < argc) vgabios = argv[++i];
    else if (!std::strcmp(argv[i], "--screenshot") && i + 1 < argc) screenshot = argv[++i];
    else if (!std::strcmp(argv[i], "--net")) net = true;
    else if (!std::strcmp(argv[i], "--no-usb")) usb = false;
    else if (!std::strcmp(argv[i], "--name") && i + 1 < argc) name = argv[++i];
    else if (!std::strcmp(argv[i], "--control") && i + 1 < argc) control = argv[++i];
    else if (!std::strcmp(argv[i], "--hostfwd") && i + 1 < argc) {
      unsigned host_port = 0, guest_port = 0;
      if (std::sscanf(argv[++i], "tcp:%u:%u", &host_port, &guest_port) != 2 || !host_port || host_port > 65535 ||
          !guest_port || guest_port > 65535) {
        std::fprintf(stderr, "run: --hostfwd wants tcp:<host port>:<guest port>\n");
        return 2;
      }
      forwards.push_back({static_cast<uint16_t>(host_port), static_cast<uint16_t>(guest_port)});
      net = true;
    }
    else if (!std::strcmp(argv[i], "--cpus") && i + 1 < argc) cpus = static_cast<uint32_t>(std::strtoul(argv[++i], nullptr, 0));
    else return usage();
  }
  if (!flat + !bios + !kernel != 2 || ram_mib > 64512 || load > 0xFFFF || (initrd && !kernel) || disks.size() > 3 ||
      boot_order.size() > 3 || boot_order.find_first_not_of("acdn") != std::string::npos) {
    std::fprintf(stderr,
                 "run: exactly one of --flat / --bios / --kernel is required, --initrd needs --kernel,\n"
                 "     --ram must be 1..64512, --load must be < 0x10000, at most 3 --disk,\n"
                 "     --boot takes up to 3 of the letters a c d n\n");
    return 2;
  }
  // A modern kernel's decompressor alone wants tens of MiB; firmware and flat binaries need little.
  if (ram_mib == 0) ram_mib = kernel ? 512 : 64;

  std::vector<uint8_t> image;
  if (!read_file(flat ? flat : bios ? bios : kernel, &image)) return 1;
  std::vector<uint8_t> initrd_image;
  if (initrd && !read_file(initrd, &initrd_image)) return 1;

  VmConfig config;
  config.ram_bytes = ram_mib << 20;
  config.local_apic = local_apic || cpus > 1;
  config.cpu_count = cpus;
  config.nic = net;
  config.boot_order = boot_order;
  const bool vga = display || screenshot;
  if (vga) config.vga_vram_bytes = 16u << 20;
  // The USB tablet is what lets the window follow the host mouse without capturing it.
  config.usb_tablet = vga && usb && !flat;
  // SeaBIOS needs the VGA's option ROM; a directly booted kernel programs the card itself.
  std::vector<uint8_t> vga_rom;
  if (vga && bios) {
    std::string rom_path;
    if (vgabios) {
      rom_path = vgabios;
    } else {
      rom_path = bios;
      const size_t slash = rom_path.find_last_of("\\/");
      rom_path = (slash == std::string::npos ? std::string() : rom_path.substr(0, slash + 1)) + "vgabios-stdvga.bin";
    }
    if (!read_file(rom_path.c_str(), &vga_rom)) return 1;
  }
  // Disks stay open across guest reboots. --discard-changes stacks a temporary differencing image on
  // each, deleted when the run ends.
  TempDisks temps;
  for (const char* d : disks) {
    std::wstring path = widen(d);
    if (discard_changes) {
      wchar_t dir[MAX_PATH + 1];
      const DWORD n = GetTempPathW(MAX_PATH + 1, dir);
      const std::wstring temp = std::wstring(n ? dir : L".\\") + L"sandvm-" + std::to_wstring(GetCurrentProcessId()) +
                                L"-" + std::to_wstring(temps.paths.size()) + L".vhd";
      block::create_differencing_image(temp, path);
      temps.paths.push_back(temp);
      path = temp;
    }
    temps.images.push_back(block::open_image(path, disk_read_only && !discard_changes));
  }
  const auto& disk_images = temps.images;
  // The disc survives reboots too, and the control pipe can swap it.
  std::mutex cd_mutex;
  std::shared_ptr<block::Disk> cd_media;
  if (cdrom) cd_media = block::FileDisk::open(widen(cdrom), true);
  const bool cd_drive = cdrom || (bios && disks.size() < 3);

  // Swaps the disc (null = eject) in the drive of `vm`, remembered for the machines after a reboot.
  auto change_cd = [&](Vm& vm, std::shared_ptr<block::Disk> media) {
    std::lock_guard cd_lock(cd_mutex);
    cd_media = media;
    vm.ide(1).change_media(0, std::move(media));
  };

  // The command pipe outlives the machines of a reboot loop and reaches the current one through this.
  std::mutex current_mutex;
  Vm* current_vm = nullptr;
  std::unique_ptr<cli::ControlPipe> pipe;  // stopped before the locals it uses go away
  if (control) {
    pipe = std::make_unique<cli::ControlPipe>(widen(control), [&](const std::string& line) -> std::string {
      const size_t space = line.find(' ');
      const std::string cmd = line.substr(0, space);
      const std::string arg = space == std::string::npos ? std::string() : line.substr(space + 1);
      std::lock_guard lock(current_mutex);
      if (!current_vm) return "error: no machine is running";
      Vm& vm = *current_vm;
      if (cmd == "power") vm.press_power_button();
      else if (cmd == "stop") vm.request_stop();
      else if (cmd == "reset") vm.request_reset();
      else if (cmd == "pause") vm.set_paused(true);
      else if (cmd == "resume") vm.set_paused(false);
      else if (cmd == "status") return vm.paused() ? "paused" : "running";
      else if (cmd == "ctrl-alt-del") {
        // Ctrl, Alt, Del (extended) down, then up in reverse order, scancode set 1.
        static constexpr uint8_t kSequence[] = {0x1D, 0x38, 0xE0, 0x53, 0xE0, 0xD3, 0xB8, 0x9D};
        vm.i8042().push_keyboard(kSequence);
      } else if (cmd == "type") {
        const std::vector<uint8_t> codes = cli::scancodes_for_text(arg);
        if (codes.empty() && !arg.empty()) return "error: no typeable characters";
        vm.i8042().push_keyboard(codes);
      } else if (cmd == "mouse") {
        int x = 0, y = 0;
        unsigned buttons = 0;
        if (std::sscanf(arg.c_str(), "%d %d %u", &x, &y, &buttons) < 2) return "error: mouse <x> <y> [buttons]";
        devices::UsbTablet* tablet = vm.tablet();
        if (!tablet) return "error: this machine has no USB tablet";
        tablet->update(x, y, buttons, 0);
      } else if (cmd == "cdrom" || cmd == "eject") {
        if (!cd_drive) return "error: this machine has no CD/DVD drive";
        std::shared_ptr<block::Disk> media;
        if (cmd == "cdrom") {
          if (arg.empty()) return "error: cdrom needs a file";
          media = block::FileDisk::open(block::widen_utf8(arg), true);
        }
        change_cd(vm, std::move(media));
      } else {
        return "error: unknown command '" + cmd + "'";
      }
      return "ok";
    });
  }

  // A guest reset (reboot) builds a fresh machine with the same configuration; disks keep what the
  // guest wrote. Anything else (power off, halt, Ctrl+C, closing the window, an error) ends the run.
  StopReason reason;
  for (;;) {
    Vm vm(config);
    if (!vga_rom.empty()) vm.set_vga_bios(vga_rom);
    // Declared after vm, so the NAT thread (which delivers into the NIC and raises its IRQ) stops first.
    std::unique_ptr<net::UserNat> nat;
    if (net) {
      devices::E1000* nic = vm.nic();
      nat = std::make_unique<net::UserNat>([nic](std::span<const uint8_t> f) { return nic->receive(f); }, forwards);
      net::UserNat* n = nat.get();
      nic->set_backend([n](std::span<const uint8_t> f) { n->guest_send(f); }, [n] { n->rx_space(); });
    }
    auto com1 = std::make_shared<devices::Serial16550>(
        devices::Serial16550::kCom1Base,
        [](uint8_t byte) {
          std::fputc(byte, stdout);
          std::fflush(stdout);
        },
        [&vm](bool level) { vm.set_irq(4, level); });
    vm.io().register_pio(devices::Serial16550::kCom1Base, devices::Serial16550::kPortCount, com1);
    vm.io().register_pio(devices::DebugCon::kPort, 1, std::make_shared<devices::DebugCon>([](uint8_t byte) {
      std::fputc(byte, stdout);
      if (byte == '\n') std::fflush(stdout);
    }));
    // Primary master and slave, then secondary slave: the secondary master is where PCs traditionally
    // keep the optical drive.
    static constexpr unsigned kDiskSlots[][2] = {{0, 0}, {0, 1}, {1, 1}};
    for (size_t k = 0; k < disk_images.size(); ++k) {
      vm.ide(kDiskSlots[k][0]).attach(kDiskSlots[k][1], disk_images[k]);
    }
    if (cd_drive) {
      std::lock_guard cd_lock(cd_mutex);
      vm.ide(1).attach_cdrom(0, cd_media);
    }

    if (bios) {
      vm.load_firmware(image);
    } else if (kernel) {
      loader::load_linux(vm, image, initrd_image, append);
    } else {
      if (!vm.memory().write(load, image.data(), image.size())) {
        SVM_ERROR("image (%zu bytes) does not fit at 0x%llx", image.size(), static_cast<unsigned long long>(load));
        return 1;
      }
      vm.vcpu(0).set_real_mode_entry(0, static_cast<uint16_t>(load), static_cast<uint16_t>(load));
    }
    {
      CurrentVm current(current_mutex, current_vm, vm);
      StopOnCtrlC stop_on_ctrl_c(vm);
      std::unique_ptr<cli::ControlEvents> control_events;  // destroyed before vm
      if (control) {
        control_events = std::make_unique<cli::ControlEvents>(
            widen(control), [&vm] { vm.press_power_button(); }, [&vm] { vm.request_stop(); });
      }
      std::unique_ptr<cli::DisplayWindow> window;  // destroyed before vm
      if (vga) {
        cli::DisplayWindow::Options options;
        options.show_window = display;
        if (screenshot) options.screenshot_path = widen(screenshot);
        if (name) options.title = widen(name) + L" - SandVM";
        if (devices::UsbTablet* tablet = vm.tablet()) {
          options.on_tablet = [tablet](int x, int y, unsigned buttons, int dz) { tablet->update(x, y, buttons, dz); };
          options.tablet_active = [tablet] { return tablet->in_use(); };
        }
        auto& actions = options.actions;
        actions.ctrl_alt_del = [&vm] {
          static constexpr uint8_t kSequence[] = {0x1D, 0x38, 0xE0, 0x53, 0xE0, 0xD3, 0xB8, 0x9D};
          vm.i8042().push_keyboard(kSequence);
        };
        actions.set_paused = [&vm](bool paused) { vm.set_paused(paused); };
        actions.reset = [&vm] { vm.request_reset(); };
        actions.power_button = [&vm] { vm.press_power_button(); };
        actions.power_off = [&vm] { vm.request_stop(); };
        if (cd_drive) {
          actions.insert_cd = [&vm, &change_cd](const std::wstring& path) {
            try {
              change_cd(vm, block::FileDisk::open(path, true));
            } catch (const std::exception& e) {
              SVM_ERROR("%s", e.what());
            }
          };
          actions.eject_cd = [&vm, &change_cd] { change_cd(vm, nullptr); };
        }
        window = std::make_unique<cli::DisplayWindow>(
            *vm.vga(), std::move(options), [&vm](std::span<const uint8_t> keys) { vm.i8042().push_keyboard(keys); },
            [&vm](int dx, int dy, int dz, unsigned buttons) { vm.i8042().push_mouse(dx, dy, dz, buttons); },
            [&vm] { vm.request_stop(); });
      }
      cli::ConsoleInput input([&com1](uint8_t byte) { com1->push_rx(byte); });  // stopped before vm goes away
      reason = vm.run();
    }
    SVM_INFO("guest stopped: %s", to_string(reason));
    for (auto& d : disk_images) d->flush();
    if (reason != StopReason::Reset) break;
    SVM_INFO("restarting the machine");
  }
  return reason == StopReason::Halted || reason == StopReason::PowerOff ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
  int i = 1;
  std::wstring box;
  for (; i < argc && argv[i][0] == '-'; ++i) {
    if (!std::strcmp(argv[i], "--version")) {
      std::printf("SandVM %s\n", kVersion);
      return 0;
    }
    if (!std::strcmp(argv[i], "-v")) {
      set_log_level(LogLevel::Debug);
    } else if (!std::strcmp(argv[i], "--sandbox") && i + 1 < argc) {
      ++i;
      box = wide_args()[i - 1];
    } else {
      return usage();
    }
  }
  if (i >= argc) return usage();

  try {
    if (!box.empty() && !sbie::running_in_sandbox()) {
      // Forward everything except "--sandbox <box>" to the sandboxed instance.
      std::vector<std::wstring> forwarded;
      auto args = wide_args();
      for (size_t k = 0; k < args.size(); ++k) {
        if (args[k] == L"--sandbox") {
          ++k;
          continue;
        }
        forwarded.push_back(args[k]);
      }
      return sbie::relaunch_in_box(box, forwarded);
    }

    const char* cmd = argv[i];
    if (!std::strcmp(cmd, "version")) {
      std::printf("SandVM %s\n", kVersion);
      return 0;
    }
    if (!std::strcmp(cmd, "caps")) return cmd_caps();
    if (!std::strcmp(cmd, "selftest")) return cmd_selftest(argc - i - 1, argv + i + 1);
    if (!std::strcmp(cmd, "run")) return cmd_run(argc - i - 1, argv + i + 1);
    if (!std::strcmp(cmd, "ctl")) {
      if (argc - i < 3) return usage();
      std::string line;
      for (int k = i + 2; k < argc; ++k) line += (k > i + 2 ? " " : "") + std::string(argv[k]);
      const auto args = wide_args();
      std::string reply;
      if (!cli::send_control(args[i], line, &reply)) {
        std::fprintf(stderr, "ctl: no running machine named %s\n", argv[i + 1]);
        return 1;
      }
      std::printf("%s\n", reply.c_str());
      return reply.rfind("error", 0) == 0 ? 1 : 0;
    }
    if (!std::strcmp(cmd, "disk")) {
      const auto args = wide_args();
      return cli::cmd_disk(std::vector<std::wstring>(args.begin() + i, args.end()));
    }
    return usage();
  } catch (const std::exception& e) {
    SVM_ERROR("%s", e.what());
    return 1;
  }
}
