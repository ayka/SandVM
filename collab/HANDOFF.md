# Handoff log

Newest entry on top. Format: date, author, task, what changed, what is untested, open questions.

---

## 2026-10-30 · claude · T-029: portable ön sürüm v0.1.1-preview.1
- **Portable paket incelemesi, bulunan hatalar:** (1) firmware pakette yoktu, GUI ilk açılışta "BIOS
  bulunamadı" diyordu; (2) GUI ayarları `%APPDATA%`, makineleri `Belgeler` altına yazıyordu (portable
  değil); (3) `-SelfContained` dışında paket .NET kurulumu istiyordu; (4) GUI `.pdb` pakete giriyordu;
  (5) `VERSION.txt` commit edilmemiş değişikliklerle üretilen paketi temiz gösteriyordu; (6) `TASKS.md`
  T-028 satırının başında PowerShell'den kalma `` `n `` vardı.
- **Düzeltmeler:** `Store.cs` portable mod (`<paket>\portable.txt` varsa `data\` + `machines\`, paket
  içindeki yollar göreli kaydedilir, yüklerken pakete göre çözülür). `package.ps1` varsayılanı portable:
  self-contained GUI, firmware + `firmware\NOTICE.txt` (SeaBIOS 1.17.0, rel-1.17.0-0-gb52ca86e094d),
  `portable.txt`, `-FrameworkDependent`/`-NoFirmware`, `version.h` ve `sandvm --version` denetimi,
  `-dirty` işareti, `DebugType=none`. Sürüm 0.1.1-preview.1 (CMake 0.1.1). README, CHANGELOG,
  THIRD_PARTY_NOTICES, docs/RELEASE.md güncellendi. LGPL için SeaBIOS kaynak arşivi
  (`seabios-1.17.0.tar.gz`, sha256 5159a35f...7712) GitHub sürümüne eklendi.
- **Test:** zip temiz klasöre açıldı: `--version`, `selftest --bios firmware\bios-256k.bin` PASS, paketteki
  firmware ile Alpine ISO `localhost login:`'e kadar açıldı (`ctl status/stop`), GUI portable modda
  "Hipervizör hazır", `%APPDATA%` listesini okumadı, `data\manager.json` oluştu; göreli disk yolu
  yüklemede mutlak oldu, kaydederken göreli kaldı.
- **Codex** `collab/out/20261030-103938-review-gui-SandVM-Manager-Services-Store-cs.md`: #2 düzeltildi
  (kaydederken yollar önce mutlak yapılıyor). #1 reddedildi: `dist\<ad>` hazırlık klasörü yeniden
  üretilebilen derleme çıktısı; zip ancak tüm adımlar bittikten sonra yeniden yazılıyor, yani
  başarısız paketleme yayımlanmış bir şeyi bozmaz.
- **Not tested:** .NET'siz temiz makinede GUI, GUI'den VM başlatma (CLI ile denendi).

## 2026-10-30 · codex · T-028
Fresh llvm-mingw Release build succeeded. Five selftests including bios-disk PASS. Self-contained win-x64 GUI publish succeeded; dist/SandVM-0.1.0-win64.zip regenerated with bundled .NET runtime. Packaging refuses framework-dependent fallback when SelfContained is requested; validates version and stage path before deletion. Firmware excluded. Windows 7, Sandboxie integration, clean-machine GUI interaction and independent review remain unverified. No commit or publication performed.

## 2026-10-30 · claude · T-027: v0.1.0 sürüm hazırlığı
- **Derleme:** `CMakeLists.txt` 10 eksik kaynakla (`block/host_file,image,vhd,vmdk`, `devices/uhci,usb`,
  `pci/uhci_pci`, `cli/control_pipe,disk_cmd,keymap`) ve `comdlg32` ile güncellendi. `tools/build.ps1`
  eklendi (llvm-mingw; `build.sh` korunuyor). CMake bu makinede kurulu olmadığı için MSVC/CMake derlemesi
  denenmedi.
- **CLI:** `src/core/version.h` (0.1.0), `--version`, `version`; yardım metni `--name`, `--no-usb`,
  `ctl type/mouse`, `--boot a c d n` ile hizalandı.
- **Yayın:** `README.md`, MIT `LICENSE`, `CHANGELOG.md`, `THIRD_PARTY_NOTICES.md`, `docs/RELEASE.md`,
  `.gitattributes`; `tools/package.ps1` (dist zip + SHA256, `-IncludeFirmware`, `-SelfContained`).
- **Test:** `tools/build.ps1` başarılı; `sandvm --version` = 0.1.0; `caps` hipervizör var;
  `selftest --bios firmware/bios-256k.bin` 5/5 PASS (polled, irq, pit, pit-spin, bios-disk).
  GUI `dotnet publish --no-restore` başarılı.
- **Ortam notu:** `dotnet restore`/`dotnet nuget list source` bu makinede "Value cannot be null
  (path1)" veriyor; mevcut `gui/.../obj/project.assets.json` ile `--no-restore` kullanıldı. Temiz bir
  makinede normal `dotnet restore` gerekir.
- **Paket:** `dist/SandVM-0.1.0-win64.zip` (3.1 MB), `dist/SHA256SUMS.txt`. Firmware/ISO paket dışı.
- **Git:** ilk commit + `v0.1.0` etiketi; push yok.
- **Not tested:** CMake/MSVC derlemesi, Windows 7, HDA, USB/pencere Codex incelemesi, Sandboxie+WHPX.

## 2026-10-04 · claude · T-021..T-024: disk images, control pipe, USB tablet, window menu (HDA started)
- **Disk images (T-021):** `block/host_file` (positioned I/O), `block/vhd` (fixed / dynamic / differencing,
  2 MiB blocks, sector bitmaps, parent via W2ru/W2ku locators), `block/vmdk` (descriptor + SPARSE/FLAT/ZERO
  extents, grain tables with redundant copy, snapshots via parentFileNameHint + CID check, new CID on first
  write), `block/image` (format probe, `open_image`, create, chain info, commit, merge, convert).
  `sandvm disk create|snapshot|info|commit|merge|convert`. `run`: `--disk` up to 3 (pm, ps, ss),
  `--discard-changes` (temp differencing VHD per disk, deleted at exit), `--boot dc` (CMOS 0x3D/0x38 as
  SeaBIOS reads), empty CD drive by default with `--bios`.
  Tested: randomized consistency test (scratchpad blocktest: dynamic VHD, diff chain + merge + commit,
  raw->vhd->vmdk->raw convert, sparse VMDK) PASS; Windows virtdisk `OpenVirtualDisk` opens our dynamic and
  differencing VHDs and resolves the parent; we read Windows-made ones; Ubuntu 26.04 boots from a
  differencing VHD over `ubuntu-disk.img` (base untouched, 72 MB in the child), ACPI shutdown via `ctl`.
- **Codex review** `collab/out/20261004-155101-review-src-core-block.md`: fixed #1 (raw parent mtime is
  checked as its identity; commit rotates a VHD parent's unique ID), #2 (flush before publishing a BAT
  entry), #3 (VMDK unclean shutdown: primary/redundant GT repair), #4 (VMDK parentCID needs a VMDK parent
  with matching CID and capacity), #5 (commit refuses data past the parent end), #9 (reparent records the
  new parent's mtime), #10 (CID set only after it is on disk), #11 (media change aborts an in-flight
  ATAPI transfer with UNIT ATTENTION), #12 (client: overlapped I/O, 5 s timeout, full line required).
  Rejected #6: a read-only open fails with a sharing violation while a VM has the image open for
  writing (FILE_SHARE_READ only), so convert cannot read a running image. Rejected #7 with evidence:
  Windows' own CreateVirtualDisk writes PlatformDataSpace in bytes (4096 / 65536). #8 kept by design
  (raw/VMDK parents are a SandVM extension; `disk snapshot` prints a note). Known: differencing-VHD
  bitmap updates and VMDK grain/GTE writes have no flush barrier between data and metadata (guest
  FLUSH CACHE flushes; same as QEMU's vpc/vmdk).
- **Control (T-022):** `cli/control_pipe` (`\\.\pipe\sandvm.<name>`, line protocol) + `sandvm ctl <name> <cmd>`:
  power, stop, reset, pause, resume, status, cdrom <iso>, eject, ctrl-alt-del, type <text> (US layout,
  `cli/keymap`), mouse <x> <y> [buttons]. The old named events stay for the GUI. `Vm::set_paused`,
  `Vm::request_reset`, `Vcpu::set_paused`. IDE `change_media` (UNIT ATTENTION, GESN media events), guest
  eject (START STOP UNIT LoEj, PREVENT ALLOW). Tested on Alpine: eject -> "No medium found", insert ReactOS
  ISO -> guest reads the new volume.
- **USB (T-023):** `devices/usb` (EP0 control state machine, standard requests; `UsbTablet` with QEMU's
  report layout, VID:PID 1209:0001), `devices/uhci` (1 ms frame thread, QH/TD walk, depth-first bit,
  QH loop guard, PORTSC, USBSTS/USBINT), `pci/uhci_pci` (00:01.2, 8086:7020, INTD -> IRQ10).
  `Vm::set_pci_irq` is now a wired-OR per PIRQ. `--no-usb`; tablet on by default with a display.
  Bug found + fixed: USBINT was not set in USBSTS -> Linux disabled IRQ10 after 100000 spurious IRQs.
  Tested on Alpine: "SandVM USB Tablet" bound by hid-generic, 10 `ctl mouse` updates -> 50 evdev events and
  10 interrupts. **Not tested:** the window interactively, Windows/ReactOS guests.
- **Window:** menu bar (Makine / Aygıtlar / Görünüm: Ctrl+Alt+Del, pause, reset, ACPI off, power off,
  insert/eject ISO, full screen, save screenshot), Ctrl+Alt+Enter full screen, Ctrl+Alt+Insert sends
  Ctrl+Alt+Del, aspect-ratio letterboxing, close asks (ACPI / power off / cancel), pointer integration
  (absolute tablet input, no capture, host cursor hidden) while the guest polls the tablet. `--name` sets
  the title. Links comdlg32.
- **In progress:** `src/core/devices/hda.h` only (Intel HDA design in its comment); no .cpp yet, nothing
  uses it. The Codex review of the USB/window code was stopped by the user;
  it has to be run again.
- Downloaded (checksums not verified yet): Debian 13.7 netinst, Fedora 44 Everything netinst, FreeBSD
  15.1 bootonly into `images/`. Arch download was stopped (a `.part` file may be left in images/).

## 2026-10-03 · claude · RAM above 4 GiB, build.sh restored, Lubuntu desktop with --net
- **RAM split:** `--ram` up to 64512 MiB. Up to 0xE0000000 RAM stays in one piece below 4 GiB as before;
  above that 3 GiB stays low (PCI window from 0xC0000000, as QEMU's i440FX) and the rest is mapped at 4 GiB.
  `Vm::ram_below_4g()/ram_above_4g()` replace `ram_bytes()`; CMOS 0x34/0x35 + 0x5B-0x5D, Linux e820 (extra
  entry), ACPI `ram_below_4g`. Tested: selftest, direct Linux 6 and 8 GiB (e820 + "Memory:"), SeaBIOS e820
  with 6 GiB (RamSizeOver4G 3 GiB), Ubuntu from disk with 6 GiB, Lubuntu live with 4 GiB.
- Codex review (`collab/out/20261003-213951-review-src-core-vm-cpp.md`): fixed #1 (64 GiB ended past 2^36: the limit is now
  the end address, 63 GiB total) and #2 (high RAM must be 64 KiB aligned, the CMOS unit).
- **`tools/build.sh` was corrupted** (a PowerShell transcript pasted over its top, the rest an old version
  without pci/net/acpi/loader/block). Rewritten: `find src -name '*.cpp'`, links user32 gdi32 ws2_32
  iphlpapi shell32, `OUT=` kept.
- **Net freeze (T-019/T-020) did not reproduce** with the current build: Alpine ISO `--apic --cpus 2 --net`,
  one 66 MB wget, then 6 x 66 MB back to back (~11 MB/s), guest ticks never stalled, clean poweroff.
  Repro script idea: pipe `root` + commands into stdin with a tick loop on the serial console. Keep watching.
- Lubuntu 26.04.1 desktop ISO (`images/lubuntu-26.04.1-desktop-amd64.iso`, SHA256 ok) boots headless to the
  Calamares welcome screen with "Ethernet 1: Connected" (`--cpus 4 --ram 4096 --net`). Disk
  `images/lubuntu-disk.img` 30 GiB. The user installs it through the window.

## 2026-10-03 · claude · Ubuntu 26.04.1 Server installer
- Downloaded `images/ubuntu-26.04.1-live-server-amd64.iso` (SHA256 matches releases.ubuntu.com), created
  `images/ubuntu-disk.img` (20 GiB). Headless boot (`--apic --cpus 2 --ram 3072 --screenshot`) reaches the
  subiquity language screen: GRUB, bochs-drm fbcon 1024x768, ImExPS/2 mouse, casper squashfs loops all fine.
  No code changes. Desktop ISO not tried (needs > our 3584 MiB RAM cap to be comfortable).
- The user installed it through the window (no network) and it boots from disk to a tty1 login
  (kernel 7.0.0-30-generic); login as the installer user works. "Reboot Now" at the end of the install
  went through our reset loop and booted the disk (SeaBIOS tries the disk before the CD).
  The first boot waits ~2 min in cloud-init-network / networkd-wait-online because there is no NIC.
- Not yet: Ubuntu with `--net` (avoid until the sustained-download freeze in T-019/T-020 is fixed).

## 2026-10-03 · claude · T-019 e1000 + T-020 user-mode NAT (in progress, user said stop)
- New `devices/E1000` (82540EM: EEPROM microwire + EERD, M88 PHY via MDIC, legacy RX, legacy/context/data
  TX with checksum offload and TSO, ICR/IMS INTx), `pci/E1000Pci` (8086:100E, subsys 8086:001E, BAR0
  128 KiB MMIO, BAR1 IOADDR/IODATA) at 00:03.0 -> PIRQD -> IRQ 11 (`Vm::set_pci_irq`).
- New `net/UserNat` (10.0.2.0/24, ARP, DHCP, DNS to the host resolver, UDP NAT, TCP proxy with its own
  TCP, ICMP via IcmpSendEcho, `--hostfwd tcp:H:G` on 127.0.0.1). CLI `--net`. `tools/build.sh` takes OUT=.
- Works in Alpine: DHCP lease 10.0.2.15, ping 10.0.2.2 and 1.1.1.1, nslookup, wget HTTP and HTTPS.
- **Open bug:** a sustained download (wget of the 60 MB ISO) freezes the guest: no serial output, 0 % CPU.
  lldb on the frozen process: no deadlock, both vCPUs inside WHvRunVirtualProcessor, NAT thread in
  select(). Suspect a lost interrupt (serial THRE IRQ4 or NIC IRQ11 / IOAPIC Remote IRR) under high IRQ
  rate from the NAT thread. Already fixed on the way: wake-up datagrams coalesced + non-blocking.
  Next: run the test with output backgrounded (does the guest still power off?), and without --apic.
- Not yet: Codex review of T-019/T-020, ReactOS with --net.

## 2026-10-03 · claude · reboot loop in `run`, ReactOS installed to disk
- CLI `run`: a guest reset (StopReason::Reset) rebuilds a fresh Vm with the same configuration and runs
  it again (window reopens); other stop reasons end the run. Tested: Alpine `reboot` -> second boot.
- The user installed ReactOS 0.4.16 (Turkish) to `images/reactos-disk.img` (4 GiB); the setup's reboot
  used to exit sandvm. The disk now boots by itself (SeaBIOS "Booting from Hard Disk") into the
  second-stage setup wizard at 800x600.

## 2026-10-03 · claude · T-013 (mouse), T-018 (RTC IRQ8), first ReactOS boot
- **Mouse:** `I8042::push_mouse(dx, dy, dz, buttons)`: 3-byte packets, IntelliMouse ID 3 (wheel byte)
  and ID 4 (wheel nibble + buttons 4/5) through the 200,100,80 / 200,200,80 sample-rate knocks; one
  packet queued at a time (motion accumulates, bounded, while the guest has not read the last one);
  remote mode (0xF0) polls with 0xEB; counters reset on the commands that reset them; 0xE9 reports
  remote/reporting/scaling/buttons. Window: click captures (raw input, ClipCursor, hidden cursor),
  Ctrl+Alt or focus loss releases; no capture when raw input registration failed.
  Linux: "ImExPS/2 Generic Explorer Mouse". Moving the mouse in a guest not yet tried by the user.
- **RTC IRQ8 (T-018):** `CmosRtc` gets a timer thread: PF at the status A rate (RS 1/2 = 256/128 Hz,
  n>=3 = 32768>>(n-1)), UF each host second, AF on alarm match (0xC0+ = don't care), IRQF/IRQ8 while
  flag & enable, acknowledged by reading C. SET clears UIE and hides UIP. Stopped by `Vm` before the
  vCPUs like the PIT. Linux shows `rtc0` on IOAPIC pin 8.
- **ReactOS 0.4.16** (`images/ReactOS-0.4.16-i386.iso`, official GitHub release; SourceForge blocks curl):
  without IRQ8 it hung on a black text screen (SeaBIOS INT 15h AH=86h waits for the RTC periodic
  interrupt; CPU halted in real mode at b757). With IRQ8 it boots to the LiveCD GUI (800x600, cursor).
  Note: FreeLDR reads physical 0xFFDFF124 with paging off thousands of times (its own bug, harmless,
  floods `-v` logs as "unclaimed MMIO read").
- VGA: mode changes are logged at debug level ("vga: text/graphics/vbe/blank ...").
- **Codex reviews:** mouse `collab/out/*-review-src-core-devices-i8042-h.md`: fixed #1 (remote mode,
  counters kept while not reporting), #2 (ID 3 full-byte wheel), #3 (64-bit accumulation; overflow bits
  deliberately not set: drivers handle them inconsistently), #4 (no capture without raw input).
  Deferred: #5 (0xEB while a stream packet is queued), #6 (AUX bytes not held back while the port is
  disabled), #7 (absolute raw-input devices, e.g. RDP: buttons only).
  RTC `collab/out/20261003-195905-review-src-core-devices-cmos-h.md`: fixed #3 #4 (mouse status/counters),
  #5 (UIP gated), #6 documented (PC RTCs are 32.768 kHz only). Deferred: #1 (time written under SET is
  lost, alarm compares host time; pre-existing T-004 gap), #2 (alarm second skipped by a late wake-up).

## 2026-10-03 · claude · T-017 (VGA/VBE display + host window)
- New `devices/Vga`: standard VGA registers (misc, seq, GC, CRTC with write protect, attribute, DAC, input
  status toggling retrace on every read), the 0xA0000-0xBFFFF window with QEMU's planar pipeline (write
  modes 0-3, read modes 0-1, set/reset, rotate, logical ops, bit mask, odd/even, chain 4, memory map
  select) and Bochs DISPI at 0x1CE/0x1CF (ID B0C5, GETCAPS 2560x1600x32, banks, 8-bit DAC, virtual
  width / offsets, NOCLEARMEM). VRAM uses QEMU's interleaved layout. render(): text (9/8-dot, 40-col,
  fonts A/B, cursor, blink, line graphics), planar 16 colours, CGA 4 colours, 256 colours, double scan,
  line compare, VBE 8/15/16/24/32 bpp; 4 bpp VBE goes through the planar path like QEMU.
- `PciFunction::add_mem_bar` (32-bit, optional prefetchable, below 0xFEC00000, follows command bit 1).
- New `pci::StdVga` 1234:1111 class 0300 at 00:02.0, BAR0 = 16 MiB LFB: registered as MMIO first, then
  mapped with WHvMapGpaRange (one Lfb object per placement). Option ROM via fw_cfg "pci1234,1111.rom"
  (`Vm::set_vga_bios`), no ROM BAR.
- `Vm`: RAM now leaves out 0xA0000-0xBFFFF (two regions), with or without VGA. `VmConfig::vga_vram_bytes`.
- CLI: `--display` (Win32 window on its own thread, StretchDIBits at 60 Hz, keys -> set 1 scancodes into
  the i8042, closing the window stops the VM), `--vgabios` (default: vgabios-stdvga.bin next to the BIOS),
  `--screenshot f.bmp` (BMP about once a second; without --display no window).
- Tested (screenshots): selftest 5/5; SeaBIOS + SeaVGABIOS ("VBE DISPI: lfb_addr=fd000000, size 16 MB")
  text-mode screen; a boot-sector test (mode 12h bars via write mode 2 + BIOS pixel diagonal, then mode
  13h gradient) renders right; Alpine ISO boots with bochs-drm fbcon at 1024x768 to the tty1 login.
- Not tested: the window interactively (keyboard typing, resize, close), Windows guests, mode X, CGA
  modes, VBE 4 bpp, banked VBE.
- **Codex review** `collab/out/20261003-191033-review-src-core-devices-vga-h.md`:
  fixed #1 (MMIO claimed before mapping; overlap -> not decoded), #2 (failed unmap freezes the BAR
  instead of leaving an alias), #4 (emulated LFB accesses locked, overflow-safe bounds), #5 (immutable
  base per mapping), #7 (PEL mask applied), #9 (SetTimer failure logged).
  Rejected for now #3: our odd/even/chain-4 addressing is QEMU's older pipeline, internally consistent
  with the renderer and tested with SeaVGABIOS; porting the newer one changes the VRAM layout. Revisit if a
  guest shows corrupted text/fonts. Deferred: #6 (separate VBE sequencer state, `sr_vbe`), #8 (PEL panning).

## 2026-10-03 · claude · T-016 part 2 (bus-master DMA)
- New `ide_dma.cpp`. Each `IdeChannel` owns its 8 bus-master registers (`bm_read/bm_write`); `Piix3Ide`'s
  BAR4 decoder forwards offsets 0-7 / 8-15 to the two channels (old register-only stub removed). DMA memory
  access through an `IdeChannel::DmaMemory` callback (Vm: `GuestMemory::read/write`, RAM only).
- READ DMA / READ DMA EXT / WRITE DMA (EXT) (0xC8/C9/25, 0xCA/CB/35) and ATAPI PACKET with the DMA feature
  bit park the transfer; it runs synchronously when the engine's start bit is (or becomes) set: PRD walk with
  EOT, 0 = 64 KiB, 64 KiB host chunks; table longer than the transfer -> Active stays set; shorter or a
  memory/disk error -> BM Error + ATA abort (UNC|ABRT) or ATAPI MEDIUM ERROR. Completion raises INTRQ.
- Every INTRQ rising edge sets the BM Interrupt bit (PIIX behaviour; Windows' pciidex ISR checks it).
- SET FEATURES 03h records the transfer mode; IDENTIFY (ATA and PACKET) report DMA, MWDMA0-2 and UDMA0-2
  in words 49/53/63/88 with the selected mode flagged.
- Measured (Linux 6.18 ata_piix, `dd`): CD read 22 -> 211 MB/s, disk write 5.7 -> 200 MB/s (DMA vs
  `libata.dma=0`); 66 MB copied CD -> disk and read back: md5 equals the ISO's; IRQ14 count 298442 -> 159.
  selftest 5/5, SeaBIOS (PIO) ISO boot still fine.
- Install test: `setup-disk` from the virt ISO cannot finish (no syslinux / linux-virt apks on the ISO, and
  no NIC yet); it partitioned and formatted the 1 GiB disk fine. `images/alpine-disk.img` is now scratch.
- Not tested: Windows, DMA writes from the ATAPI side (none), PRD tables crossing non-RAM.
- **Codex review** `collab/out/20261003-175113-review-src-core-devices-ide-dma-cpp.md`, all five accepted and fixed:
  #1 DMA cannot write read-only (ROM) regions; #2 outcomes split per SFF-8038i: table too short -> Active=0,
  Interrupt=0, no device completion, the command stays parked (restart continues it, reset drops it); bus
  error (PRD/data not RAM, direction mismatch) -> BM Error, no synthesized INTRQ; disk error -> ATA abort
  UNC|ABRT / ATAPI MEDIUM ERROR; #3 ATA DMA updates the task file with the next LBA and residual count on
  completion and on error; #4 the BM Interrupt bit latches on the visible INTRQ rising edge (set_irq), so nIEN
  suppresses it; #5 the BM direction bit is checked against the command. Retested: selftest 5/5, DMA bench
  220/200 MB/s, md5 OK.

## 2026-10-03 · claude · T-016 part 1 (ATAPI CD/DVD-ROM)
- `IdeChannel::attach_cdrom(index, media)` (media = ISO via `FileDisk`, null = empty tray); CLI `--cdrom <iso>`
  puts it on the secondary master. New `ide_atapi.cpp`; shared register bits moved to `ide_regs.h`.
- Drives now have `present` / `atapi` flags. Signatures: after SRST / EXECUTE DIAGNOSTIC every present device
  "shows" its signature (ATAPI 01 01 14 EB); selecting a device loads its own into the shared task file until
  any task file write or command. IDENTIFY DEVICE to an ATAPI device aborts with the signature.
- ATA commands on ATAPI: PACKET (PIO only; DMA bit -> abort), IDENTIFY PACKET DEVICE (0x85C0, accelerated DRQ,
  no DMA modes), DEVICE RESET, diag, power/set features no-ops.
- PACKET protocol: CDB phase DRQ with CoD and no IRQ; replies cut to the CDB allocation length, sent in DRQ
  chunks of the byte count limit (lcyl/hcyl, even, 0/0xFFFF -> 0xFFFE) with IO reason + IRQ each; completion
  IRQ with IO|CoD; errors: ERR, error = sense key << 4, sense kept for REQUEST SENSE. READ streams 16-sector
  batches from the image.
- MMC subset: TEST UNIT READY, REQUEST SENSE, INQUIRY, MODE SENSE 6/10 (pages 01, 2A, 3F), START STOP,
  PREVENT ALLOW, SEEK, SYNC CACHE, SET CD SPEED, READ CAPACITY, READ 10/12, READ TOC formats 0/1 (LBA and MSF),
  GET CONFIGURATION (profile list, DVD-ROM if > 80 min of sectors), GET EVENT STATUS (polled, media class),
  READ DISC INFORMATION, MECHANISM STATUS. Others: ILLEGAL REQUEST / 0x20. No media: NOT READY / 0x3A.
- Tested end to end: SeaBIOS "DVD/CD [ata1-0: SandVM DVD-ROM ATAPI-6 DVD/CD]" -> El Torito -> ISOLINUX ->
  Alpine 3.24 virt ISO boots to the login prompt on ttyS0 (with `--apic --cpus 2`). In the guest: ACPI tables
  are the ones SeaBIOS placed from fw_cfg (RSDP 0xF5320, FACP high), IOAPIC routing, "ata2.00: ATAPI: SandVM
  DVD-ROM", "sr0: scsi3-mmc drive", /dev/sr0 mounted as iso9660, md5 of /media/cdrom/boot/vmlinuz-virt
  matches the host copy; IDE disk on the primary channel works next to it; `poweroff` -> power off.
  Note: with fw_cfg present SeaBIOS puts PMBA at 0x600 (no longer 0xB000), the FADT agrees.
- Not tested: empty tray, media errors, Windows (atapi.sys / cdrom.sys), READ CD (0xBE) and other packets
  Windows may send (they get ILLEGAL REQUEST), DEVICE RESET under Linux error handling.
- **Codex review** `collab/out/20261003-172622-review-src-core-devices-ide-atapi-cpp.md`, all six accepted and fixed:
  #1 task file writes / commands clear only the selected device's signature (HDD master + CD slave keeps the
  slave's 14/EB visible); #2 byte count limit 0 aborts data commands at the ATA level (only 0xFFFF -> 0xFFFE);
  #3 the limit is applied before rounding, only non-final chunks are rounded to even (minimum 2), odd final
  chunks allowed; #4 GET CONFIGURATION honours request type 3 (error) and the starting feature; #5 MODE SENSE
  page control: changeable -> zeroed parameters, saved -> ILLEGAL REQUEST 0x39; #6 INQUIRY VPD pages 0x00 and
  0x80 (serial). fw_cfg / table-loader: no findings. Retested: selftest 5/5, ISO boot + disk + md5 OK.
  Still untested: master HDD + slave CD on one channel (the CLI only puts the CD on the secondary master).

## 2026-10-03 · claude · SMP, fw_cfg (T-012 part 2), Codex review of T-014/T-015
- **SMP:** `VmConfig::cpu_count` 1..15 (more than 1 needs `local_apic`; CLI `--cpus N`). APs start with
  `InternalActivityState.StartupSuspend` and the hypervisor's LAPIC handles INIT/SIPI. One thread per vCPU;
  a vCPU stopping on its own stops the VM; PIC INTR goes to the BSP only. Linux: "Brought up 1 node, 4 CPUs",
  IOAPIC interrupts spread over CPUs, LAPIC timers on all, S5 power off. CLI exit code 0 on power off.
- **ACPI tables relocatable:** `acpi::Tables` = rsdp + tables blob + pointer/checksum lists; `link()` for
  direct boot (RSDP 0xE0000, tables 0xE0040), `loader_script()` emits QEMU "etc/table-loader" commands.
  `MachineInfo::pm_base`.
- **`devices/FwCfg`** (0x510/0x511, no DMA): signature, ID, RAM size, CPU counts, file directory, files with a
  refresh callback. `Vm` publishes etc/acpi/rsdp, etc/acpi/tables, etc/table-loader, rebuilt on select from the
  PMBA SeaBIOS programmed (0xB000). Tested: SeaBIOS "Found QEMU fw_cfg", "Found 2 cpu(s)", "table(FACP) via
  xsdt", "ACPI: parse DSDT". Not yet tested: an OS booted through SeaBIOS reading those tables.
- **Codex review** `collab/out/20261003-170930-review-src-core-devices-ioapic-h.md`:
  - Fixed #1: IOAPIC ID passed in (= cpu count, matches MADT); max 15 CPUs so it fits the 4-bit ID.
  - Fixed #2: the PM timer is sampled once per access (no torn 32-bit reads).
  - Fixed #3: FADT PWR_BUTTON flag cleared, the power button is the fixed-feature one we implement.
  - Rejected #4 (SCI active-low in the MADT ISO): QEMU's i440fx uses the same 0x000D (active high, level),
    the PIIX4 SCI line is active high, Linux accepts it and Windows 7 runs on QEMU with it.
  - Deferred #5 (TMR_STS on PM timer carry): needs a host timer thread to raise SCI while all vCPUs are
    halted; neither Linux nor Windows 7 enables TMR_EN. Documented in piix.h.
  - Fixed #6: `IoApic::Deliver` returns success; on WHvRequestInterrupt failure the pin stays pending and
    Remote IRR is not set.
  - Fixed #7: Remote IRR only for fixed / lowest-priority delivery.
  - Fixed #8: PIIX3 PIRQ registers read-only (10,10,11,11), so they cannot disagree with the static _PRT.

## 2026-10-03 · claude · T-014 (LAPIC + IOAPIC), T-015 (ACPI), review of T-008/T-012
- **Codex review** `collab/out/20261003-165056-review-src-core-loader.md`, all five accepted and fixed:
  #1 BAR remap is two-pass (unmap all changed, then map); #2 `IoBus` MMIO accesses that do not fit the
  device range are split into bytes (like PIO); #3 config cycles reach any attached function (no
  multifunction gating); #4 PAM write masks 0x30 / 0x33; #5 `ConsoleInput` restores console modes and closes
  the event if the reader thread cannot be created.
- **T-014** `VmConfig::local_apic` (CLI `--apic`): `WHvPartitionPropertyCodeLocalApicEmulationMode = XApic`.
  Observed on WHPX (not documented): with APIC emulation on, HLT never exits to us; the VP sits in
  `InternalActivityState.HaltSuspend` inside the hypervisor, and a pending ExtINT event does NOT wake it.
  So PIC interrupts go in as `WHvRegisterPendingEvent` (ExtInt, like QEMU's whpx), and `deliver_interrupt`
  clears HaltSuspend itself when injecting. Without that, the first PIC interrupt after an idle HLT stays
  in-service forever and the guest freezes.
  New `devices/IoApic` (82093AA, 24 pins, MMIO 0xFEC00000, ID = cpu count): messages go out through
  `WHvRequestInterrupt`; level pins keep Remote IRR until `WHvRunVpExitReasonX64ApicEoi` -> `Vm::ioapic_eoi`.
  `Vm::set_irq` feeds both PIC and IOAPIC (IRQ0 -> pin 2).
- **T-015** `acpi/aml.*` (small AML encoder) and `acpi/tables.*`: RSDP v2, RSDT+XSDT, FADT rev 3 (PM block,
  SCI 9, SMI_CMD 0xB2 with F1/F0, reset reg 0xCF9=6, IAPC 8042), FACS, DSDT (PCI0 with _CRS and a static
  _PRT: PIRQ = (slot+pin)&3 -> IRQ 10,10,11,11; ISA devices KBD/MOU/COM1/RTC/PIC/TMR/SPKR; \_PR processors;
  \_S0 and \_S5 = SLP_TYP 0), MADT (LAPICs, IOAPIC, ISO 0->2 and 9 level/high, LINT1 NMI).
  `Vm::install_acpi_tables()` writes them at 0xE0000 and presets PMBA; the Linux loader sets
  boot_params.acpi_rsdp_addr.
  New `pci/Piix4Pm` at 00:01.3 (8086:7113): PM1 STS/EN/CNT, PM timer (QPC based, 24 bit), GPE0, SCI on IRQ 9,
  power button (`Vm::press_power_button`), S5 -> new `StopReason::PowerOff`; APM 0xB2/0xB3 without SMM.
  `PciBus` handles the reset control register 0xCF9 (-> `StopReason::Reset`). PIIX3 PIRQ registers preset.
- Tested: Linux 6.18 with `--apic`: "ACPI: Using IOAPIC for interrupt routing", all interrupts on IO-APIC
  (timer 2-edge, acpi 9-fasteoi, ttyS0, i8042, ata_piix), LAPIC timer running (LOC counts), PM timer
  clocksource, every DSDT device enumerated, `poweroff -f` -> "guest stopped: power off". Without `--apic`
  the same ACPI tables (no MADT) work in PIC mode. selftest all PASS; SeaBIOS with the PIIX4 PM now finds 4
  PCI devices, uses the PM timer, gets through its SMM init (APM status trick) and still boots.
- Known: SeaBIOS programs PMBA to 0xB000, our static tables say 0x600 -> tables for the firmware path must be
  built from the programmed PMBA (fw_cfg, T-012 part 2). No HPET. PM timer overflow (TMR_STS) never set.
  With `--apic`, a guest HLT with IF=0 stays in the hypervisor, so `poweroff` without ACPI never stops the VM
  (Ctrl+C does). SMP not tried yet.

## 2026-10-03 · claude · T-008 (Linux direct boot), T-012 part 1 (PCI + PIIX3 IDE), T-011 review fixes
- **T-011 Codex review** `collab/out/20261003-163845-review-src-core-devices-ide-h.md`:
  - Fixed #1: `IdeChannel::active_` keeps the drive that accepted the command; the data phase, error LBA
    encoding and CHS math use it, so reselecting the other drive mid-transfer cannot crash or misdirect I/O.
  - Fixed #2: `report_progress()` after every block (and on error) writes the next LBA and the residual sector
    count (incl. HOB for LBA48) into the task file, like QEMU's PIO path.
  - Fixed #3: READ VERIFY reads the range from the backing disk in 64 KiB chunks.
  - Rejected for now #4 (async disk I/O): the vCPU blocks on the image file while holding the channel lock.
    Documented in `ide.h` that images belong on local storage. Revisit with bus-master DMA, which needs a
    worker anyway.
  - New: a channel with no drives at all reads 0xFF (floating bus), which Linux libata expects.
- **T-008** `src/core/loader/linux.cpp`: 64-bit boot protocol (2.12+, XLF_KERNEL_64). Kernel at 1 MiB, boot_params
  at 0x7000 (setup header copied, e820: low RAM / 0x9FC00-0xFFFFF reserved / RAM from 1 MiB), cmdline at 0x20000,
  initrd at the top of RAM below initrd_addr_max, GDT at 0x500 (0x10 code64, 0x18 data), identity map of 4 GiB
  with 2 MiB pages at 0x9000-0xEFFF, long mode registers set directly (CR0 PG|PE, CR4 PAE, EFER LME|LMA, TR busy
  TSS, LDTR present), RIP = kernel + 0x200, RSI = boot_params.
- CLI: `run --kernel <bzImage> [--initrd f] [--append s]` (default 512 MiB RAM, default cmdline
  `console=ttyS0 earlyprintk=serial,ttyS0 panic=-1`). New `cli/ConsoleInput`: stdin -> COM1 (console in raw mode,
  arrows as VT100 sequences, VT output enabled; a pipe is forwarded until EOF, the thread is cancellable).
- **T-012 part 1** `src/core/pci/`: `PciFunction` (config space with write masks, I/O BAR sizing and decoding that
  follows BAR + command register through new `IoBus::unregister_pio/mmio`), `PciBus` (mechanism #1, 0xCF8/0xCFC,
  bus 0, multi-function aware). `piix.*`: i440FX host bridge 00:00.0 (PAM/SMRAM storage only), PIIX3 ISA 00:01.0
  (PIRQ routing registers, `route()`), PIIX3 IDE 00:01.1 (compat mode, both channels owned and registered here,
  IDETIM decode enable preset, BAR4 bus-master register block without DMA). `Vm` owns them; `Vm::ide(n)` replaces
  the CLI/selftest-made IdeChannel.
- Tested with Alpine 3.24 `linux-virt` 6.18.52 (images/, gitignored): boots to the initramfs shell over COM1 with
  stdin; `/proc/interrupts` shows timer (PIT, XT-PIC), i8042, ttyS0, ata_piix; `poweroff -f` -> halted.
  ata_piix finds "ATA-6: SandVM HARDDISK", sd_mod gives /dev/sda, reads of sector 0 and 4096 match, a write to
  sector 100 lands in the image file. selftest (all 5, incl. SeaBIOS bios-disk now with i440FX present) PASS.
- Known, not fixed: no MTRR MSRs (kernel warns on RDMSR 0xC0010010 / WRMSR 0xC001001F, harmless); the CPU
  advertises an APIC but WHPX's LAPIC emulation is off, so Linux reads all-ones at 0xFEE00000 ("Stale ISR"),
  falls back to the PIC and works. Proper fix = T-014 (LAPIC/IOAPIC). No ACPI/MP tables (T-015).
- Not tested: CHS-only guests, LBA48 beyond 2^28, multiple mode with Linux, SRST under libata error handling,
  serial input of non-ASCII keys, `--append` quoting with spaces from PowerShell.

## 2026-10-03 · claude · T-011 (IDE disk)
- `block::Disk` with `FileDisk` (raw image, positioned I/O via OVERLAPPED offsets, FILE_SHARE_READ only) and
  `MemoryDisk` (tests). `tools/build.sh` now also compiles `src/core/block/*.cpp`.
- `devices::IdeChannel`: one legacy channel, two drive slots, PIO only. IDENTIFY (ATA-6, LBA48, 16-sector
  multiple), READ/WRITE SECTORS (+EXT, +MULTIPLE), READ VERIFY, SET MULTIPLE, INITIALIZE DEVICE PARAMETERS (CHS),
  READ NATIVE MAX, FLUSH, CHECK POWER MODE, EXECUTE DIAGNOSTIC, SET FEATURES / power / seek / recalibrate as no-ops,
  SRST via device control, nIEN, HOB. ATAPI/DMA/SMART abort. Absent slave reads 0. Registered by the CLI, not Vm
  (like COM1), on 0x1F0-0x1F7 / 0x3F6, IRQ14. The secondary channel is left unclaimed.
- CLI: `run --bios <fw> --disk <img> [--disk-ro]`; `selftest --bios <fw>` adds `bios-disk`: SeaBIOS boots a
  hand-assembled boot sector from an in-memory IDE disk ("Booted from disk!").
- Tested: build clean; selftest all 5 PASS; `run --disk` with a 4 MiB file image boots ("ata0-0: SandVM HARDDISK
  ATA-6 Hard-Disk (4 MiBytes)" → "Booted from a file disk!"). Not tested: writes, LBA48, CHS-only guests, multiple
  mode, SRST, read errors, Linux/Windows drivers. Codex review running.

## 2026-10-03 · claude · T-010 (i8042)
- T-007: Codex review `collab/out/20261003-153813-diff-uncommitted.md` approved with no findings → `done`.
- New `devices/I8042`: ports 0x60/0x64 plus System Control Port A 0x92. Controller commands 0x20/0x60 (CCB),
  0xA7-0xAB, 0xAD/0xAE, 0xC0, 0xD0-0xD4, 0xE0, 0xF0-0xFF (reset pulse); output port bit 0 = reset, bit 1 = A20.
  PS/2 keyboard (FF, F2 with translated ID, F4/F5/F6, ED/F3/F0 with parameter, EE) and a PS/2 mouse that answers
  commands but never moves. One output queue; IRQ1/IRQ12 drop on every 0x60 read so each byte is a fresh edge.
  `push_keyboard()` takes set-1 bytes (assumes translation on). A20 is tracked, not enforced (WHPX has no A20 mask).
- Guest reset (0xFE, output port bit 0 low, 0x92 bit 0) → `Vm` stops with new `StopReason::Reset`; the CLI just
  reports it for now (restart = build a new Vm, later).
- Tested: builds clean, selftest all PASS, SeaBIOS now logs "PS2 keyboard initialized" and the i8042_flush
  timeout is gone (`collab/out/20261003-seabios-i8042.log`).
- Codex review `collab/out/*review-src-core-devices-i8042*.md`:
  - Fixed #4: interface-disable bits gate IRQ1/IRQ12; writing to the keyboard (0x60) / mouse (0xD4) re-enables
    its interface. (Recalculation on 0xA7/0xA8/0xAD/0xAE was already there: `controller_command` ends with
    `update_irq()`.)
  - Fixed #5: 0xF6 re-enables scanning; invalid 0xF0 parameter → 0xFE; set query is translated (2 → 0x41) when
    CCB translation is on.
  - Deferred #1 (set-2 / untranslated host input) and #2 (mouse movement API) to new task T-013: there is no host
    input path yet, and the right API (abstract key/mouse events) belongs with it. Limitation documented in i8042.h.
  - Deferred #3 (A20 enforcement): WHPX has no A20 mask; emulating wrap needs aliasing GPA ranges. Few guests
    need A20 off; documented in i8042.h as tracked, not enforced.

## 2026-10-03 · claude · T-007 step 1 (firmware loading, debugcon)
- Decision (user): use a prebuilt SeaBIOS, QEMU's `pc-bios/bios-256k.bin` (LGPLv3), kept in `firmware/`
  (gitignored), passed with `sandvm run --bios firmware/bios-256k.bin`. QEMU is not installed on the machine.
- `Vm::load_firmware`: ROM mapped so it ends at 4 GiB, last 128 KiB copied into RAM at 0xE0000, BSP set to the reset
  vector (`Vcpu::set_reset_entry`: CS F000/base FFFF0000, IP FFF0). The low copy is writable RAM, so SeaBIOS's PAM
  shadowing (no i440FX modeled yet) is a harmless no-op. RAM is now capped at 3.5 GiB (no PCI hole split yet).
- `devices/DebugCon` on port 0x402: QEMU's SeaBIOS builds (CONFIG_DEBUG_IO) log there, not to COM1.
- Expectation, from reading SeaBIOS behavior (not verified): without an i440FX at PCI 00:00.0 it reports
  "No QEMU Northbridge found (isapc?)" and takes RAM size from CMOS 0x30/0x34; without fw_cfg (0x510) it skips
  ACPI/SMBIOS tables; with no disks it ends at "No bootable device." Possible trouble spots: MTRR/other MSR writes
  under WHPX, LAPIC MMIO at 0xFEE00000 (unclaimed: reads all-ones), TSC calibration on PIT channel 2.
- First run crashed: `bios-256k.bin` also executes code at 0xC0000-0xDFFFF, which SeaBIOS normally copies there
  from the high ROM after unlocking PAM on the i440FX. No bridge → no copy → jump into zeros, ESP wrapped, failed
  MMIO write at 0xFFFFFFD0. Fix: `load_firmware` copies up to 256 KiB to end at 1 MiB (the post-copy state).
  Real PAM handling belongs to T-012. MMIO failures now log access type, CS:RIP, CPU mode and instruction bytes.
- Result: boots to "No bootable device. Retrying in 60 seconds." Log: `collab/out/20261003-seabios-first-boot.log`.
  Confirms CMOS RAM size, PIT channel 2 TSC calibration ("CPU Mhz=3502"), COM1 detection, e820 map.
  Missing (new tasks): i8042 (T-010), IDE disk (T-011), i440FX/PAM/fw_cfg (T-012). selftest still all PASS.

## 2026-10-03 · claude · response to Codex diff review `collab/out/20261003-151548-diff-uncommitted.md`
All four findings accepted and fixed:
- #1 (kick vs. run entry race): no longer relies on WHPX remembering a pre-entry cancel. Kick protocol in
  `vcpu.h`: the loop sets `in_run_`, snapshots `kick_gen_` into `seen_gen_`, then checks stop/PIC and enters.
  `kick()`/`cancel()` bump `kick_gen_` and, off the vCPU thread, repeat `WHvCancelRunVirtualProcessor` (+
  `SwitchToThread`) while `in_run_ && seen_gen_ < gen`. Cancel failures are logged. The spin only lasts until the
  run call returns, never across device emulation (`in_run_` is cleared right after the run call).
  New stress test `selftest pit-spin`: the PIT guest idles in an exit-free loop, so every IRQ0 must cancel a
  running VP from the PIT thread.
- #2 (PIT gate in modes 0/4): `Channel` now has `base_ticks`/`running`; GATE low freezes the count, GATE high
  resumes it; a count loaded while GATE is low waits. Modes 1/5 trigger and 2/3 restart on a rising GATE.
- #3 (SFNM): ISR2 is ignored only when the candidate request is IR2 itself.
- #4 (straddling PIO): `IoBus` passes a wide access to a device only if it lies entirely inside the device's
  range; otherwise it splits it into bytes, each routed separately. `Serial16550` no longer wraps with `& 7`.
- Tested (user ran it): builds clean, `sandvm selftest` → polled, irq, pit and pit-spin all PASS. pit-spin shows
  that IRQ0 reaches a guest spinning without VM exits. T-005 and T-006 marked `done`.
  Still untested: #2 (channel 2 gate) and #3 (SFNM) have no guest test yet; #4 only by inspection.

## 2026-10-03 · claude · T-005 (PIT), taken over from Codex
- Why Claude: the Codex review of T-006 could not be started from this session (shell blocked by the auto-mode
  check), so T-005 was written meanwhile. Codex should review it together with the T-006 diff.
- `devices/Pit8254`: ports 0x40-0x43 and port B 0x61 (gate 2, speaker enable, refresh toggle, OUT2). Counts are
  derived from QPC at read time, not ticked. Modes 0-5 (6/7 aliased), LSB/MSB/LSB+MSB access, counter latch,
  read-back (count + status, null count). BCD accepted but counted in binary (warns once).
- IRQ0: a timer thread computes the next channel 0 OUT rising edge, sleeps on a high-resolution waitable timer
  (falls back to a normal one before Win10 1803) and pulses IRQ0. Missed periods are coalesced into one pulse.
  Owned by `Vm`, stopped explicitly before the vCPUs are destroyed (its IRQ path kicks vcpus_[0]).
- Simplifications (same as QEMU): gate low does not pause modes 0/4; a new count in modes 2/3 takes effect at
  once instead of at the end of the period; no speaker audio.
- `selftest`: new `pit` guest (channel 0 mode 2 at ~100 Hz, 5 ticks of IRQ0, expects "TTTTT"), and a 5 s watchdog
  so a guest waiting for a lost interrupt fails instead of hanging.
- Tested (user ran it): builds clean, `sandvm selftest` → polled, irq and pit all PASS ("TTTTT" from 5 IRQ0 ticks).
  Not tested: channel 2 / port B (Linux TSC calibration path), read-back, modes 0/1/4/5 on IRQ0, 1 kHz rates,
  BCD. Next: `tools/codex.sh diff` covering T-005 + T-006.

## 2026-10-03 · claude · T-006 (PIC + interrupt injection) and review of T-004
- New `devices/Pic8259`: cascaded master/slave, ELCR (0x4D0/1), edge latch / level mode, ISR/EOI incl. specific,
  rotate and auto-EOI, special mask, special fully nested, poll mode, spurious IRQ7. Cascade IR2 follows the slave
  INT as a level so a second slave request is not lost waiting for an edge. Owned and registered by `Vm`.
- `Vcpu`: before every entry, if PIC INTR is up and IF=1, no interrupt shadow, no pending event → `acknowledge()` +
  `WHvRegisterPendingInterruption`; otherwise arm `DeliverabilityNotifications.InterruptNotification` and handle
  `X64InterruptWindow`. HLT with IF=1 waits on an auto-reset event until INTR (Codex #4). `kick()` = SetEvent +
  `WHvCancelRunVirtualProcessor` when called off the vCPU thread; a `Canceled` exit without a stop request just loops.
- Codex #1: `Vm` now owns the vCPU thread (`start()` / `wait()` / `run()`), destructor stops and joins it.
- Codex #5/#6: `IoBus` holds `shared_ptr` devices, looks up under a `shared_mutex`, calls devices without any bus lock.
- `Serial16550`: IRQ output callback, level = pending IIR source && MCR.OUT2 && !loopback; COM1 wired to IRQ4 in CLI.
- CLI: `run` stops on Ctrl+C. `selftest` gained an `irq` guest (hand-assembled, ofsets checked by `static_assert`).
- T-004 review: accepted, `CmosRtc(ram_bytes)` registered in `Vm`. Notes, not blockers: guest writes to time
  registers (SET bit) are lost when SET clears, the clock always follows host time; 0x34/0x35 counts RAM up to 4 GiB
  but `Vm` maps RAM flat with no PCI hole (fix with T-007); no checksum at 0x2E/0x2F. IRQ8 → follow-up after T-006.
- Tested (user ran it, shell was blocked for Claude by the auto-mode check): `tools/build.sh` builds clean,
  `sandvm selftest` → `polled` PASS, `irq` PASS ("IRQ OK" over IRQ4 on real WHPX/AMD).
  Not tested: Ctrl+C stop of `run`, slave PIC path, level-triggered (ELCR) lines, poll mode, host-thread IRQs.
  Next: `tools/codex.sh diff`.
- Not verified on WHPX: whether a cancel issued just before `WHvRunVirtualProcessor` starts is remembered. If not,
  an IRQ raised by a host thread at that moment waits for the next exit (delay, not loss).

## 2026-10-03 · codex · T-004
- Added a locked `CmosRtc` PIO device for ports 0x70/0x71 with index selection, NMI-mask tracking, host-local
  BCD/binary and 12/24-hour RTC reads, MC146818 status registers, writable CMOS RAM, and read-to-clear status C.
- Initialized conventional PC memory-size fields 0x15-0x18, 0x30/0x31, the requested 0x34/0x35 (64 KiB blocks
  above 16 MiB, 16-bit saturated), and 0x5B-0x5D (64 KiB blocks above 4 GiB, 24-bit saturated). Added the source
  to CMake; `tools/build.sh` already includes all `src/core/devices/*.cpp` files and did not require a change.
- Untested: compilation and runtime. The managed sandbox denied execution of both the bundled `clang++` driver
  and `clang-23.exe` (`Permission denied`). RTC alarm/periodic/update interrupts are intentionally not generated.
- Open questions for Claude: please register `CmosRtc(config.ram_bytes)` for ports 0x70-0x71 in the VM-owning code
  and confirm whether IRQ8 delivery should become a separate follow-up after T-006.

## 2026-10-03 · claude · response to Codex review `collab/out/20261003-144421-review-src-core.md`
- Fixed: #2 (Serial16550 now has its own mutex; TX callback invoked outside the lock), #3 (emulator callbacks
  catch all exceptions and return E_FAIL), #7 (THRE latch: set on THR write / ETBEI enable, cleared on IIR read),
  #8 (launcher checks wait/exit-code), #9 (FreeLibrary + reset on failed load), #10 (x64-only `#error` + CMake check).
- Deferred to T-006: #4 (HLT with IF=1 must wait for interrupts), #5 (IoBus should not hold its lock while calling
  devices once all devices lock themselves), #6 (device ownership / unregister).
- Partially accepted #1: `Vm::run()` is synchronous on the caller thread today, so the race needs a caller bug.
  Documented the lifetime contract in `vm.h`; Vm will own and join vCPU threads in T-006.
- Built clean with llvm-mingw (`tools/build.sh`). Runtime still untested: hypervisor was not launched
  (`hypervisorlaunchtype` was off, now set to auto, reboot pending).

## 2026-10-03 · claude · T-001, T-002
- Wrote core skeleton: `whpx` runtime loader, `Vm` (partition, 1 vCPU, RAM at GPA 0), `GuestMemory`, `IoBus`,
  `Vcpu` run loop with WinHvEmulation for I/O and MMIO exits, `Serial16550`, Sandboxie `launcher`, CLI.
- **Untested: nothing has been compiled yet** (no compiler on the machine, see T-003).
- Open questions for Codex: is the HLT/RIP handling right? Is the emulator callback usage (Direction, AccessSize, GVA translation) correct?
