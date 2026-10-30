# Sürüm çıkarma

Bu proje `vMAJOR.MINOR.PATCH` biçiminde etiketlenir. v0.1.0 için hazırlanmış akış:

1. **Sürüm numarasını güncelle**
   - `CMakeLists.txt` → `project(SandVM VERSION ...)`
   - `src/core/version.h` → `kVersion`
   - `gui/SandVM.Manager/SandVM.Manager.csproj` → `<Version>`
   - `tools/package.ps1` varsayılan `-Version`
   - `CHANGELOG.md` yeni bölüm
2. **Derle ve test et**
   ```powershell
   .\tools\build.ps1
   .\build\sandvm.exe --version
   .\build\sandvm.exe caps
   .\build\sandvm.exe selftest
   ```
3. **GUI'yı derle**
   ```powershell
   dotnet build gui\SandVM.Manager\SandVM.Manager.csproj -c Release
   ```
   `dotnet restore` bu makinede NuGet yapılandırması nedeniyle başarısız olursa, mevcut
   `obj\project.assets.json` ile `dotnet publish --no-restore` kullanılabilir.
4. **Paketi üret**
   ```powershell
   .\tools\package.ps1
   ```
   Yerel kullanımda firmware eklemek için `-IncludeFirmware`, GUI'yi kendi kendine
   yeterli yapmak için `-SelfContained` kullanın. Çıktı `dist\SandVM-<sürüm>-win64.zip`
   ve `dist\SHA256SUMS.txt` olur.
5. **Git**
   ```powershell
   git add -A
   git commit -m "SandVM v0.1.0"
   git tag -a v0.1.0 -m "SandVM v0.1.0"
   ```
   Push yalnızca kullanıcı istediğinde yapılır.
6. **Paket içeriğini doğrula**: `sandvm.exe`, `SandVM.Manager\`, `docs\`,
   `firmware\README.txt`, `README.md`, `LICENSE`, `CHANGELOG.md`,
   `THIRD_PARTY_NOTICES.md`, `VERSION.txt`.
