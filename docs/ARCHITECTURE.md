# SandVM Mimarisi

## Neden hibrit?

Sandboxie-Plus bir **uygulama sandbox'ıdır**. Programlar host çekirdeği üzerinde çalışır. `SbieDrv.sys` ve
`SbieDll.dll` dosya, kayıt defteri, IPC ve ağ çağrılarını kutuya yönlendirir. Ayrı bir çekirdek boot edemez.
Windows 7 veya Linux çalıştırmak için CPU sanallaştırması (AMD-V / VT-x) gerekir. SandVM bu yüzden iki katmanlıdır:

```
┌──────────────────────── Sandboxie-Plus kutusu (isteğe bağlı, --sandbox) ────────────────────────┐
│  sandvm.exe (VMM, user mode)                                                                     │
│   ├─ Vm         : WHPX partition, bellek haritası                                                │
│   ├─ Vcpu × N   : WHvRunVirtualProcessor döngüsü + WinHvEmulation (I/O, MMIO)                    │
│   ├─ IoBus      : port I/O ve MMIO yönlendirme                                                   │
│   └─ devices/   : UART, (PIC, PIT, CMOS, VGA, AHCI/IDE, virtio ...)                              │
└──────────────────────────────────────────────────────────────────────────────────────────────────┘
                 │ WinHvPlatform.dll  (Windows Hypervisor Platform API)
┌────────────────▼───────────────────────────┐
│  Microsoft Hyper-V hipervizörü (ring -1)    │  ← konuk işletim sistemi burada donanım hızında çalışır
└─────────────────────────────────────────────┘
```

- **Hipervizör katmanı (SandVM core):** konuk işletim sistemini çalıştırır. Kendi kodumuz, WHPX API'si üzerine kurulu.
- **İzolasyon katmanı (Sandboxie-Plus):** VMM sürecini kutuya alır. Disk imajları, loglar ve ağ arka uçları
  host'a sızmaz, kutu silinince iz kalmaz. Ayrıca tam VM gerektirmeyen uygulamalar için hafif mod sağlar.

## Sandboxie kaynak kodunun kullanımı

- Sandboxie-Plus **GPLv3** lisanslıdır. Kodunu kopyalarsak SandVM de GPLv3 olur. Bu kararı kullanıcı verecek.
- Şu an Sandboxie'yi yalnızca kurulu uygulamanın `Start.exe` arayüzüyle kullanıyoruz (`src/sbie/launcher.cpp`).
- Planlanan derin entegrasyon: `SbieApi` / `SbieDll` üzerinden kutu oluşturma, ayar yazma (`SbieIniSet`) ve
  süreç durumunu sorgulama. Bunun için `third_party/` altına Sandboxie-Plus kaynakları eklenip başlıklar incelenecek.
- **Doğrulanmamış (T-009):** WHPX, `vid.sys` cihazına IOCTL ile erişir. Sandboxie kutusunun bu cihaza erişimi
  engelleyip engellemediği test edilmedi. Gerekirse kutu ayarına `OpenFilePath` veya `NormalFilePath` istisnası eklenecek.

## Çekirdek bileşenler (`src/core`)

| Dosya | Görev |
|---|---|
| `whpx.*` | DLL'leri çalışma zamanında yükler (özellik kapalıysa anlaşılır hata verir) |
| `vm.*` | Partition yaşam döngüsü. Yıkım sırası: vCPU → bellek eşlemeleri → partition |
| `guest_memory.*` | `VirtualAlloc` + `WHvMapGpaRange`. ROM yazma izni olmadan eşlenir |
| `vcpu.*` | Çalıştırma döngüsü. I/O ve MMIO çıkışları WinHvEmulation ile işlenir (string I/O dahil) |
| `io_bus.*` | Port ve adres aralıklarını cihazlara yönlendirir |

## Yol haritası

1. **M1: çekirdek iskeleti** (bitti, derleniyor): gerçek modda konuk, COM1 çıktısı, `sandvm selftest`.
2. **M2: BIOS boot:** PIC, PIT, CMOS, kesme enjeksiyonu, SeaBIOS yükleme, BIOS banner'ının seri porta düşmesi.
3. **M3: Linux:** bzImage doğrudan boot, initramfs, seri konsol, ardından virtio-blk ve virtio-net.
4. **M4: Windows 7:** ACPI tabloları, LAPIC/IOAPIC, HPET, AHCI veya IDE disk, VGA/VBE, PS/2 klavye ve fare, e1000 ağ.
5. **M5: Sandboxie derin entegrasyonu ve arayüz:** kutu yönetimi, VM yapılandırma dosyaları, snapshot.
