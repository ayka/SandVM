# Yeni oturum için devam noktası

> Yeni bir Claude Code oturumu bu dosyayı okuyarak kaldığı yerden devam eder.
> Son güncelleme: 2026-10-30 (v0.1.0 sürümü). Ayrıntılar `collab/HANDOFF.md`'nin en üstünde.

## 2026-10-30 oturumu: v0.1.0 sürüm hazırlığı (tamamlandı)
Kullanıcı "projeyi tamamla ve yayınlanabilir sürüm haline getir" dedi. Kapsam: v0.1.0 sürüm
hijyeni (HDA ve Windows 7 hariç), lisans MIT, çıktı yerel zip + git etiketi.
Yapılanlar:
- `CMakeLists.txt` eksik 10 kaynak dosya ve `comdlg32` bağlantısıyla hizalandı.
- `src/core/version.h` + `sandvm --version`; yardım metni gerçek seçeneklerle güncellendi.
- `tools/build.ps1`, `tools/package.ps1`, `README.md`, `LICENSE` (MIT), `CHANGELOG.md`,
  `THIRD_PARTY_NOTICES.md`, `docs/RELEASE.md`, `.gitattributes` eklendi.
- `dist/SandVM-0.1.0-win64.zip` üretildi; firmware varsayılan olarak paket dışında.
- İlk commit + `v0.1.0` etiketi atıldı (push yok).
Doğrulama: `tools/build.ps1` başarılı; `sandvm caps` hipervizör var; `selftest --bios` 5/5 PASS;
GUI publish başarılı. Bu makinede `dotnet restore` NuGet hatası verdi; mevcut `obj` varlıklarıyla
`--no-restore` kullanıldı. Paket 3.1 MB.
Sıradaki: HDA (T-024), USB/pencere Codex incelemesi + elle test, T-025 GUI snapshot,
T-009 Sandboxie+WHPX, Windows 7.

## 2026-10-04 oturumu: kullanıcı "ilerlemeleri kaydet ve dur" dedi
Kullanıcının isteği: eksikleri gider, başka dağıtımlarda da çalışsın, VMware'deki özellikleri ekle.
Yapılanlar (hepsi derleniyor, `build/sandvm.exe` güncel, selftest 5/5):
- **Disk:** VHD (sabit/dinamik/fark), VMDK okuma-yazma, `sandvm disk create|snapshot|info|commit|merge|convert`,
  `--disk` 3 kez, `--boot dc`, `--discard-changes`. Ubuntu snapshot'tan açıldı; Windows VHD'lerimizi açıyor.
- **Kontrol:** `--control <ad>` + `sandvm ctl <ad> power|stop|reset|pause|resume|status|cdrom <iso>|eject|
  ctrl-alt-del|type <metin>|mouse x y [düğme]`. Çalışırken CD değiştirme guest'te doğrulandı.
- **USB tablet** (fare entegrasyonu, yakalamasız), varsayılan açık (`--no-usb` kapatır). Linux'ta doğrulandı.
- **Pencere:** menü (Makine/Aygıtlar/Görünüm), tam ekran Ctrl+Alt+Enter, Ctrl+Alt+Insert = Ctrl+Alt+Del,
  en-boy oranı korunuyor, kapatırken soruyor, `--name`.
Sıradaki adımlar (sırayla):
1. USB/pencere kodu için Codex incelemesi yarıda durduruldu: `tools/codex.sh review src/core/devices/uhci.cpp src/core/devices/usb.cpp src/core/pci/uhci_pci.cpp src/cli/display_window.cpp src/cli/main.cpp` yeniden çalıştır, bulguları işle.
2. Kullanıcı pencereyi elle denesin: Lubuntu/ReactOS'ta fare entegrasyonu, menü, tam ekran.
3. T-024 HDA ses: `src/core/devices/hda.h` hazır, `hda.cpp` + `pci/hda_pci` (00:04.0, 8086:2668) + winmm
   arka ucu yazılacak.
4. T-026 dağıtım testleri: `images/` altında Debian 13.7 netinst, Fedora 44 netinst, FreeBSD 15.1 bootonly
   (SHA256 henüz doğrulanmadı; script: oturum scratchpad'indeki download.sh mantığı). Arch indirmesi yarım
   kaldı, `images/archlinux-x86_64.iso.part` varsa silinebilir.
5. T-025 GUI: snapshot yönetimi (VHD fark zinciri), yeni disk için VHD, açılış sırası, kontrol borusu komutları.
> Kullanıcıyla Türkçe konuş. Kullanıcı komutları ayrı PowerShell'de çalıştırıyor; önce
> `cd C:\Users\kayaa\Desktop\proje\sandboxie` gerekiyor.

## Proje bir bakışta
- **SandVM:** WHPX üzerine C++20 VMM. Hedef: Linux ve Windows 7 boot etmek. Sandboxie-Plus VMM sürecini
  kutuya almak için (`src/sbie/`). Mimari: `docs/ARCHITECTURE.md`, Codex kuralları: `docs/COLLAB.md`.

## Çalışanlar (gerçek donanımda test edildi)
- `sandvm selftest [--bios firmware/bios-256k.bin]`: 5 test PASS.
- SeaBIOS: PCI (i440FX, PIIX3 ISA/IDE, PIIX4 PM), fw_cfg, ACPI tablolarımızı yayımlıyor, IDE diskten boot.
- **ATAPI CD/DVD-ROM** (`run --bios firmware/bios-256k.bin --cdrom images/alpine-virt-3.24.2-x86_64.iso
  [--apic --cpus 2] [--ram 512]`): Alpine ISO SeaBIOS -> ISOLINUX -> Linux ile giriş ekranına kadar açılıyor
  (kullanıcı `root`, şifresiz). fw_cfg ACPI yolu da böylece uçtan uca doğrulandı.
- Linux doğrudan boot (`run --kernel images/vmlinuz-virt --initrd images/initramfs-virt
  [--append "console=ttyS0 rdinit=/bin/sh"] [--cpus 4] [--disk img]`): Alpine 6.18 kabuğa kadar,
  stdin COM1'e gidiyor, ACPI + IOAPIC + LAPIC timer + SMP (4 CPU), ata_piix ile disk okuma/yazma, S5 kapanma.
- **VGA/VBE ekran** (`--display` pencere, `--screenshot f.bmp` penceresiz BMP): SeaVGABIOS metin modu,
  mode 12h/13h, VBE LFB. Alpine ISO pencerede bochs-drm ile 1024x768 konsol + tty1 girişi. Pencere
  klavyesi i8042'ye gidiyor: kullanıcı pencereden `root` ile giriş yaptı (2026-10-03).
- **Fare** (pencerede tıkla = yakala, Ctrl+Alt = bırak): PS/2 IntelliMouse, Linux "ImExPS/2 Explorer Mouse".
- **RTC IRQ8** (T-018) eklendi. **ReactOS 0.4.16 LiveCD** GUI'ye kadar açılıyor (800x600, fare imleci):
  `run --bios firmware/bios-256k.bin --cdrom images/ReactOS-0.4.16-i386.iso --disk images/reactos-disk.img
  --apic --ram 1024 --display`.
- **Ubuntu 26.04.1 Server** (`images/ubuntu-26.04.1-live-server-amd64.iso`, SHA256 doğrulandı): SeaBIOS ->
  GRUB -> casper -> subiquity dil seçim ekranına ~2 dk'da geliyor (`--apic --cpus 2 --ram 3072`).
  **Kullanıcı diske kurdu** (`images/ubuntu-disk.img`, 20 GiB, LVM kök 9.75 GB), diskten açılıp `ayka` ile
  giriş yapıldı (kernel 7.0.0-30-generic, 2026-10-03). Açılış: `run --bios firmware/bios-256k.bin
  --disk images/ubuntu-disk.img --apic --cpus 2 --ram 3072 --display`. Ağsız ilk açılışta cloud-init /
  networkd-wait-online ~2 dk bekletiyor (normal).
- **RAM 4 GiB üstü** (`--ram` en çok 64512): 3.5 GiB üstünde 3 GiB altta, kalanı 4 GiB'de.
- **Lubuntu 26.04.1 masaüstü** `--net` ile canlı ortamda açılıyor (Ethernet bağlı). Kurulum:
  `run --bios firmware/bios-256k.bin --cdrom images/lubuntu-26.04.1-desktop-amd64.iso
  --disk images/lubuntu-disk.img --cpus 4 --ram 4096 --net --display`. Kullanıcı kuracak.
- **Yeniden başlatma:** misafir reset isteyince `run` makineyi yeniden kurup açıyor. ReactOS diske kuruldu
  (`images/reactos-disk.img`), diskten 2. aşama kurulum sihirbazına açılıyor.
- `images/` (gitignore): Alpine netboot kernel/initramfs, Alpine virt ISO ve ondan çıkarılan
  `iso-vmlinuz-virt`/`iso-initramfs-virt` (sd_mod içerir), `test.img`.

## Görev durumu (collab/TASKS.md)
- done: T-001..T-007, T-010, T-011. review (Codex incelemesi yapıldı, bulgular işlendi): T-008, T-012, T-014, T-015.
- T-016 review: ATAPI + bus-master DMA bitti (DMA, PIO'dan ~35 kat hızlı), Codex bulguları düzeltildi.
- T-017 review: VGA/VBE + pencere bitti, Codex bulguları işlendi (#3 reddedildi, #6 #8 ertelendi; HANDOFF).
- T-013 review (fare), T-018 review (RTC IRQ8); Codex bulguları işlendi.

## Şu an yarım kalan iş (kullanıcı "dur" dedi)
- T-019 e1000 + T-020 NAT yazıldı, `--net` ile Alpine'de DHCP/ping/DNS/HTTP/HTTPS çalışıyor.
- Uzun indirmede donma hatası güncel derlemede tekrarlanamadı (6 x 66 MB sorunsuz). Gözlemeye devam.
- `tools/build.sh` bozulmuştu, yeniden yazıldı (HANDOFF).

## Sıradaki adımlar (Windows 7'ye giden yol)
1. Kullanıcı ReactOS'u `--display` ile elle denesin (fare/klavye), sonra ReactOS'u diske kurmayı dene
   (Windows yolunun iyi bir provası). Windows 7 ISO yok: yasal kaynak yok, kullanıcı bulursa denenir.
2. Ağ (e1000 veya virtio-net), HPET (isteğe bağlı), RAM'i 4 GiB üstüne bölme (PCI deliği).
3. T-009: Sandboxie kutusu içinden WHPX'in çalıştığını test et.

## Bilinen sınırlamalar
- `--apic` modunda IF=0 ile HLT hipervizör içinde kalıyor; ACPI'siz `poweroff` VM'i durdurmuyor (Ctrl+C çalışır).
- MTRR MSR'leri yok (Linux uyarı veriyor, zararsız). PM timer taşma kesmesi yok.
- WHPX gözlemi: APIC modunda ExtINT olayı HaltSuspend'deki VP'yi uyandırmıyor, biz temizliyoruz (vcpu.cpp).

## Kullanıcının bekleyen kararları
- Sandboxie kaynak kodu GPLv3: kod kopyalanırsa SandVM de GPLv3 olur. Cevap bekleniyor.
- Commit yok; kullanıcı isterse atılır.

## Ortam notları
- Windows 10 Pro 19045, AMD Ryzen 5 5600, hipervizör açık. Derleme: `tools/build.sh` (llvm-mingw).
- Codex CLI: `tools/codex.sh`. Codex derleyemez, kodu Claude derler.
- Python ile dosya yamalarken `encoding='utf-8'` kullan (HANDOFF'ta UTF-8 karakterler var).
