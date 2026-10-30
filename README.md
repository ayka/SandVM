# SandVM

SandVM, Windows Hypervisor Platform (WHPX) üzerine kurulu, C++20 ile yazılmış bir sanal makine
monitörüdür (VMM). Amacı gerçek işletim sistemlerini — Linux ve Windows 7 — donanım hızında
çalıştırmaktır. SeaBIOS, i440FX/PIIX3, PCI, ACPI, IDE/ATAPI ve VGA gibi PC donanımını taklit eder;
konuk işletim sistemi Microsoft hipervizöründe çalışır.

Sandboxie-Plus isteğe bağlı bir **dış izolasyon katmanıdır**; hipervizör değildir. `--sandbox`
seçeneği VMM sürecini bir Sandboxie kutusu içinde başlatır, ancak WHPX'in kutu içinden çalışması
henüz doğrulanmamıştır (bkz. [Bilinen sınırlar](#bilinen-sınırlar)).

## Durum (v0.1.1-preview.1)

| Alan | Durum |
|---|---|
| Çekirdek | WHPX partition, çoklu vCPU, konuk belleği, port/MMIO veri yolu |
| Firmware | SeaBIOS, i440FX/PIIX3, PCI, ACPI/fw_cfg, IDE'den boot |
| Linux | Alpine (ISO ve doğrudan kernel), Ubuntu 26.04 Server, Lubuntu 26.04 masaüstü |
| ReactOS | 0.4.16 LiveCD ve diske kurulum |
| Windows 7 | Henüz doğrulanmadı (yasal ISO yok) |
| Disk | raw, VHD (sabit/dinamik/fark), VMDK; snapshot, commit, merge, convert |
| CD/DVD | ATAPI + bus-master DMA; çalışırken medya takma/çıkarma |
| Ekran | VGA/VBE, pencere, BMP ekran görüntüsü |
| Giriş | PS/2 klavye/fare, USB tablet (yakalamasız fare) |
| Ağ | e1000 + user-mode NAT, DHCP/DNS/TCP/UDP, port yönlendirme |
| Ses | Yok (HDA henüz yazılmadı) |
| Arayüz | WPF tabanlı SandVM.Manager (VM, disk ve ISO yönetimi) |

## Gereksinimler

- Windows 10 veya 11, 64-bit
- CPU sanallaştırması ve **Windows Hypervisor Platform** açık olmalı
  (Windows Özellikleri > Windows Hypervisor Platform; açtıktan sonra yeniden başlatın)
- Portable sürüm başka bir şey gerektirmez: GUI .NET çalışma zamanıyla, SeaBIOS/SeaVGABIOS
  firmware ile birlikte gelir
- Kaynaktan derlemek için llvm-mingw veya MSVC + CMake 3.20+

## Hızlı başlangıç

`SandVM-<sürüm>-win64.zip` portable bir pakettir; kurulum gerekmez. Arşivi bir klasöre (veya USB
belleğe) çıkarın. GUI için `SandVM.Manager\SandVM.Manager.exe` dosyasını açın. Paketteki
`portable.txt` dosyası sayesinde ayarlar `data\`, yeni sanal makineler `machines\` klasörüne,
yani paketin içine yazılır; paket içindeki yollar göreli saklanır, klasör taşınabilir. Bu dosyayı
silerseniz kullanıcı başına konumlar (`%APPDATA%\SandVM`, `Belgeler\SandVM`) kullanılır.

Komut satırından denemek için:

```bat
sandvm.exe --version
sandvm.exe caps
sandvm.exe selftest
```

`caps` hipervizörün kullanılabilir olduğunu, `selftest` ise yerleşik konuk testlerini gösterir
(`--bios` verilirse SeaBIOS disk boot testi de eklenir). GUI için
`SandVM.Manager\SandVM.Manager.exe` dosyasını açın.

### Firmware

`--bios` ve GUI, iki firmware dosyasına ihtiyaç duyar:

```text
firmware\bios-256k.bin        SeaBIOS
firmware\vgabios-stdvga.bin   SeaVGABIOS
```

Sürüm paketinde SeaBIOS 1.17.0 ikilileri (LGPLv3, değiştirilmemiş) bulunur; karşılık gelen
kaynak kodu GitHub sürümüne `seabios-1.17.0.tar.gz` olarak eklenir. Depoya dahil değildir:
kaynaktan derlerken bir QEMU kurulumundan `firmware\` klasörüne kopyalayın.
`sandvm selftest` ve `sandvm run --kernel` onlarsız da çalışır.

## Kullanım örnekleri

Alpine ISO'sunu BIOS ile başlatma:

```bat
sandvm.exe run --bios firmware\bios-256k.bin --cdrom images\alpine-virt-3.24.2-x86_64.iso
```

Pencereli, 2 vCPU ve 3 GiB RAM ile Ubuntu diski:

```bat
sandvm.exe run --bios firmware\bios-256k.bin --disk images\ubuntu-disk.img ^
  --apic --cpus 2 --ram 3072 --display
```

Linux'u doğrudan kernel ile başlatma (seri konsol):

```bat
sandvm.exe run --kernel images\vmlinuz-virt --initrd images\initramfs-virt ^
  --append "console=ttyS0" --cpus 4
```

Ağ ve port yönlendirme:

```bat
sandvm.exe run --bios firmware\bios-256k.bin --cdrom images\lubuntu-26.04.1-desktop-amd64.iso ^
  --disk images\lubuntu-disk.img --cpus 4 --ram 4096 --net --hostfwd tcp:8080:80 --display
```

Çalışan makineyi dışarıdan yönetme:

```bat
sandvm.exe run --bios firmware\bios-256k.bin --cdrom images\alpine-virt-3.24.2-x86_64.iso --control demo
sandvm.exe ctl demo status
sandvm.exe ctl demo cdrom images\ReactOS-0.4.16-i386.iso
sandvm.exe ctl demo type "root\n"
sandvm.exe ctl demo mouse 16000 12000
```

Disk araçları:

```bat
sandvm.exe disk create images\test.vhd 20G --format vhd
sandvm.exe disk info images\test.vhd
sandvm.exe disk snapshot images\child.vhd images\test.vhd
sandvm.exe disk commit images\child.vhd
sandvm.exe disk convert images\test.vhd images\test.raw --format raw
```

`--discard-changes`, her diskin üzerine geçici bir fark VHD'si koyar ve çıkışta siler
(VMware'in kalıcı olmayan modu). `--boot` sırası `a` (disket), `c` (disk), `d` (CD/DVD),
`n` (ağ) harflerini kabul eder.

## Derleme

Windows'ta (llvm-mingw, ek kurulum gerekmez):

```powershell
.\tools\build.ps1
```

bash/WSL ile:

```bash
tools/build.sh
```

MSVC + CMake ile:

```bat
cmake -S . -B build-msvc -G "Visual Studio 17 2022" -A x64
cmake --build build-msvc --config Release
```

GUI:

```powershell
dotnet build gui\SandVM.Manager\SandVM.Manager.csproj -c Release
```

## Sürüm paketi

```powershell
.\tools\package.ps1                       # portable: .NET dahil GUI, firmware, portable.txt
.\tools\package.ps1 -FrameworkDependent   # GUI için kurulu .NET 8 Desktop Runtime gerekir
.\tools\package.ps1 -NoFirmware           # SeaBIOS/SeaVGABIOS pakete eklenmez
```

Çıktı: `dist\SandVM-<sürüm>-win64.zip` ve `dist\SHA256SUMS.txt`.

## Proje yapısı

| Yol | İçerik |
|---|---|
| `src/core/` | WHPX yükleyici, partition, vCPU döngüsü, bellek, I/O bus |
| `src/core/devices/` | UART, PIC, PIT, CMOS/RTC, VGA, IDE/ATAPI, USB, e1000 |
| `src/core/block/` | raw/VHD/VMDK disk katmanı |
| `src/core/pci/` | i440FX/PIIX3, VGA ve e1000 PCI işlevleri |
| `src/core/acpi/` | AML ve ACPI tabloları |
| `src/core/net/` | user-mode NAT |
| `src/sbie/` | Sandboxie-Plus başlatıcı |
| `src/cli/` | `sandvm` komut satırı, pencere, kontrol borusu, disk araçları |
| `gui/SandVM.Manager/` | WPF yönetici arayüzü |
| `docs/` | mimari, işbirliği ve sürüm notları |
| `tools/` | derleme ve paketleme betikleri |

## Bilinen sınırlar

- **Windows 7** hedefi henüz doğrulanmadı; yasal bir ISO bulunmadığı için test edilemedi.
- **HDA ses** yoktur; yalnızca `hda.h` taslağı vardır.
- `--apic` modunda IF=0 ile HLT hipervizörde kalabilir; ACPI'siz `poweroff` VM'i durdurmaz
  (Ctrl+C çalışır).
- MTRR MSR'leri ve PM timer taşma kesmesi yoktur (Linux uyarı verir, zararsızdır).
- Sandboxie kutusu içinden WHPX kullanımı (T-009) doğrulanmadı.
- USB tablet ve pencere kodunun bağımsız incelemesi ve Windows/ReactOS'ta elle testi tamamlanmadı.
- GUI'de snapshot yönetimi ve açılış sırası düzenleme henüz yoktur.
- ISO/disk imajları pakete dahil değildir; firmware depoda yoktur (yalnızca sürüm paketinde).

## Lisans

MIT — bkz. [LICENSE](LICENSE). Üçüncü taraf bileşenler ve lisansları için
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) dosyasına bakın.

## Dokümantasyon

- [Mimari](docs/ARCHITECTURE.md)
- [İşbirliği protokolü](docs/COLLAB.md)
- [Sürüm çıkarma](docs/RELEASE.md)
- [Değişiklik günlüğü](CHANGELOG.md)
