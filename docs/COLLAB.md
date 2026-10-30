# Claude ↔ Codex İşbirliği Protokolü

SandVM iki ajanla geliştiriliyor: **Claude** (Claude Code) ve **Codex** (OpenAI Codex CLI).
Kullanıcı yön verir, son kararı kullanıcı verir.

## Roller

| Rol | Varsayılan sahip | Not |
|---|---|---|
| Mimari ve çekirdek (vCPU döngüsü, bellek, WHPX) | Claude | Değişiklikler Codex'e inceletilir |
| Bağımsız cihaz modelleri (PIT, CMOS, PIC, ...) | Codex veya Claude | `collab/TASKS.md` üzerinden atanır |
| Kod incelemesi | Karşı taraf | Kodu yazan kendi kodunu onaylamaz |
| Derleme ve test | Görevi alan | Test edilmeyen kısım HANDOFF'ta açıkça yazılır |

## Akış

1. Görev `collab/TASKS.md` dosyasına eklenir: ID, sahip, durum (`todo`, `doing`, `review`, `done`).
2. Sahibi görevi uygular. Claude kendisi yazar. Codex'e verilen görev `tools/codex.sh task <ID>` ile çalıştırılır.
3. Biten görev `review` durumuna geçer ve `collab/HANDOFF.md` dosyasına bir devir notu eklenir.
4. Karşı taraf inceler. Claude, Codex'in kodu için `git diff` okur. Codex, Claude'un kodu için `tools/codex.sh diff` ya da `review` kullanır.
5. Bulgular düzeltilir veya gerekçesiyle reddedilir. Görev `done` olur ve kullanıcı isterse commit atılır.

Codex'in bütün çıktıları `collab/out/` klasöründe saklanır. Bu klasör iki ajan arasındaki kalıcı yazışma kaydıdır.

## Kurallar

- Aynı dosya üzerinde iki ajan aynı anda çalışmaz. Görevin dosya listesi TASKS.md'de yazılır.
- Codex varsayılan olarak `read-only` çalışır. Dosyaya yazma yetkisi (`workspace-write`) yalnızca `task` modunda verilir.
- Commit ve push yalnızca kullanıcı istediğinde yapılır.
- Bir bulgu kabul edilmiyorsa nedeni HANDOFF.md'ye yazılır. Sessizce görmezden gelinmez.
- WHPX ya da x86 davranışından emin olunmayan durumda tahmin yürütülmez. Konu "doğrulanmadı" diye işaretlenir.

## Komutlar

```bash
tools/codex.sh ask    "WHvRunVpExitReasonX64Halt sonrası RIP ilerliyor mu?"
tools/codex.sh review src/core -- "WHPX API kullanımı"
tools/codex.sh diff
tools/codex.sh task T-004
tools/codex.sh resume "2. bulguyu biraz daha açar mısın?"
```
