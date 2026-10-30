# Task board

Status: `todo` → `doing` → `review` → `done`. Owner: `claude`, `codex`, `user`.

| ID | Title | Owner | Status | Files | Notes |
|---|---|---|---|---|---|
| T-001 | Core skeleton: WHPX loader, partition, memory, I/O bus, vCPU loop | claude | done | src/core/* | Codex reviewed, selftest PASS on hardware |
| T-002 | 16550 UART (COM1) | claude | done | src/core/devices/serial16550.* | IRQ output added in T-006 |
| T-003 | Toolchain: build with MSVC Build Tools or llvm-mingw, run `sandvm selftest` | user/claude | done | CMakeLists.txt, tools/build.sh | llvm-mingw; `sandvm selftest` PASS 2026-10-03 |
| T-004 | CMOS/RTC device (ports 0x70/0x71), incl. NMI mask bit and memory size registers 0x34/0x35, 0x5B-0x5D | codex | done | src/core/devices/cmos.*, CMakeLists.txt | Claude reviewed (accepted, notes in HANDOFF), registered in Vm, builds clean. IRQ8 is a follow-up |
| T-005 | i8254 PIT (ports 0x40-0x43, 0x61 speaker gate) | claude | done | src/core/devices/pit8254.*, src/core/vm.*, src/cli/selftest.cpp | Taken over from Codex. Codex-reviewed, findings fixed, `selftest` pit + pit-spin PASS 2026-10-03 |
| T-006 | Dual i8259 PIC + interrupt injection into WHPX (interrupt window exits, HLT wakeup) | claude | done | src/core/devices/pic8259.*, src/core/vcpu.*, src/core/vm.*, src/core/io_bus.*, src/core/devices/serial16550.*, src/cli/* | Incl. first review #1 #4 #5 #6. Codex diff review fixed, `selftest` polled/irq/pit/pit-spin PASS 2026-10-03 |
| T-007 | Load SeaBIOS at 0xE0000-0xFFFFF and 0xFFFE0000 alias, boot to BIOS banner on serial | claude | done | src/core/vm.*, src/core/vcpu.*, src/core/devices/debugcon.h, src/cli | Milestone 2. User approved using a prebuilt SeaBIOS (QEMU `bios-256k.bin`, LGPLv3) from `firmware/`, not committed. Boots to "No bootable device" 2026-10-03 (`collab/out/20261003-seabios-first-boot.log`). Codex approved |
| T-008 | Linux direct boot (bzImage boot protocol, 64-bit entry) | claude | review | src/core/loader/*, src/cli/console_input.*, src/cli/main.cpp | Milestone 3. Alpine 6.18 boots to a shell on COM1 (stdin forwarded), 2026-10-03 |
| T-009 | Sandboxie: verify WHPX works from inside a box; required box settings | user/claude | todo | docs/ARCHITECTURE.md | Unknown, research |
| T-010 | i8042 keyboard controller (ports 0x60/0x64), PS/2 keyboard, IRQ1; port 0x61 bits it shares with the PIT stay in the PIT | claude | done | src/core/devices/i8042.*, src/core/vm.*, src/core/vcpu.* | SeaBIOS: "PS2 keyboard initialized". Codex reviewed: #4 #5 fixed, #1 #2 -> T-013, #3 (A20) deferred |
| T-011 | IDE/ATA controller (0x1F0/0x3F6, IRQ14) with a raw disk image backend, so SeaBIOS can boot a boot sector | claude | done | src/core/devices/ide.*, src/core/block/*, src/core/vm.*, src/cli/*, tools/build.sh | SeaBIOS already probes 1f0/170 |
| T-012 | i440FX host bridge: PCI config space 0xCF8/0xCFC, PAM shadow registers; fw_cfg (0x510/0x511) | claude | review | src/core/pci/*, src/core/io_bus.*, src/core/vm.*, src/core/devices/fw_cfg.* | Part 1 in review: PCI bus, i440FX, PIIX3 ISA + IDE (Linux ata_piix reads/writes the disk). Left: fw_cfg, memory BARs + PCI hole |
| T-014 | Local APIC (WHPX xAPIC emulation mode), IOAPIC at 0xFEC00000, interrupt routing through it; then SMP (INIT/SIPI) | claude | review | src/core/vm.*, src/core/vcpu.*, src/core/devices/ioapic.* | Needed for Windows 7 x64 (APIC HAL) and SMP |
| T-015 | ACPI tables (RSDP/RSDT/FADT/MADT/DSDT) for direct Linux boot and via fw_cfg for SeaBIOS | claude | review | src/core/acpi/*, src/core/pci/piix.*, src/core/vm.* | Direct boot done (Linux uses IOAPIC, PM timer, S5). fw_cfg path left (T-012 part 2) |
| T-016 | IDE bus-master DMA (PIIX3 BAR4) + ATAPI CD-ROM (boot/installer ISOs) | claude | review | src/core/devices/ide*, src/core/pci/piix.* | ATAPI + DMA done (Alpine ISO boots via SeaBIOS to login; DMA ~35x PIO) |
| T-017 | VGA/VBE display (Bochs VBE / std VGA) with a host window | claude | review | src/core/devices/vga.*, src/core/pci/std_vga.*, src/core/pci/pci.*, src/core/vm.*, src/cli/display_window.* | Text, mode 12h/13h, VBE LFB verified by screenshots; Codex reviewed (#1 #2 #4 #5 #7 #9 fixed) |
| T-013 | Host input: abstract key/mouse events -> PS/2 set 2 (+ i8042 translation) and mouse packets; console/window input source | claude | review | src/core/devices/i8042.*, src/cli/display_window.* | Window keyboard (set 1) + PS/2 mouse (IntelliMouse ID 3/4, raw input capture). Set 2 with translation off still open. Codex reviewed |
| T-018 | RTC interrupts (IRQ8: periodic, update-ended, alarm) | claude | review | src/core/devices/cmos.*, src/core/vm.* | Fixed ReactOS hanging in SeaBIOS INT 15h/86h. Codex reviewed |
| T-027 | v0.1.0 release: build parity, docs, packaging, first commit/tag | claude | done | CMakeLists.txt, README.md, LICENSE, CHANGELOG.md, tools/*, docs/RELEASE.md | Build + selftest 5/5 PASS; GUI published; zip + SHA256 in dist/; MIT license; no push |
