# Değişiklik günlüğü

Bu proje [Anlamsal Sürümleme](https://semver.org/lang/tr/) kullanır.

## [0.1.1-preview.1] - 2026-10-30

Ön sürüm: portable paket düzeltmeleri.

### Değişen

- Sürüm paketi artık portable: SandVM.Manager .NET 8 çalışma zamanıyla birlikte (self-contained)
  gelir, ayrı kurulum gerekmez.
- SeaBIOS/SeaVGABIOS 1.17.0 pakete eklendi (LGPLv3); kaynak arşivi GitHub sürümünde.
- `portable.txt` varken GUI ayarları `data\`, yeni makineleri `machines\` altında, paketin içinde
  tutar; paket içindeki yollar göreli saklanır (USB bellekte sürücü harfi değişse de çalışır).

### Düzeltilen

- Paket firmware içermediği için GUI ilk açılışta "BIOS dosyası bulunamadı" diyordu.
- Self-contained yayımlama başarısız olunca paket sessizce .NET kurulumu isteyen sürüme düşüyordu.
- `VERSION.txt` commit edilmemiş değişikliklerle üretilen paketi `-dirty` olarak işaretler;
  `sandvm --version` paket sürümüyle uyuşmazsa paketleme durur.
- GUI yayımından `.pdb` dosyası çıkarıldı.

## [0.1.0] - 2026-10-30

İlk herkese açık sürüm.

### Eklenen

- WHPX tabanlı çekirdek: partition, çoklu vCPU, konuk belleği, port/MMIO veri yolu.
- SeaBIOS boot: i440FX/PIIX3, PCI yapılandırma uzayı, ACPI/fw_cfg, IDE'den boot.
- Linux desteği: Alpine (ISO ve doğrudan bzImage), Ubuntu 26.04 Server, Lubuntu 26.04 masaüstü.
- ReactOS 0.4.16: LiveCD ve diske kurulum.
- ATAPI CD/DVD-ROM ve PIIX3 bus-master DMA; çalışırken medya değiştirme.
- VGA/VBE ekran, pencere, tam ekran, BMP ekran görüntüsü.
- PS/2 klavye ve fare, yakalamasız USB tablet.
- e1000 ağı ve user-mode NAT: DHCP, DNS, TCP/UDP, ICMP, `--hostfwd`.
- Disk katmanı: raw, VHD (sabit/dinamik/fark), VMDK; `create`, `snapshot`, `info`, `commit`,
  `merge`, `convert`, `--discard-changes`.
- Kontrol borusu: `power`, `stop`, `reset`, `pause`, `resume`, `status`, `cdrom`, `eject`,
  `ctrl-alt-del`, `type`, `mouse`.
- WPF tabanlı SandVM.Manager: VM tanımları, disk/ISO seçimi, çalıştırma ve günlük.
- Windows için `tools/build.ps1` ve `tools/package.ps1`; CMake/MSVC yapılandırması.

### Bilinen sınırlar

- Windows 7 henüz doğrulanmadı.
- HDA ses denetleyicisi yok.
- Sandboxie kutusu içinden WHPX (T-009) doğrulanmadı.
- USB tablet/pencere kodu bağımsız incelemeden geçmedi.
- Firmware ve ISO imajları pakete dahil değil.
