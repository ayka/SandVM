# Builds the portable binary release folder and zip under dist\.
#
#   .\tools\package.ps1 -Version 0.1.1-preview.1
#   .\tools\package.ps1 -FrameworkDependent    # GUI needs an installed .NET 8 Desktop Runtime (smaller zip)
#   .\tools\package.ps1 -NoFirmware            # leave SeaBIOS/SeaVGABIOS out
#   .\tools\package.ps1 -SkipBuild             # reuse build\sandvm.exe
#
# The default package is portable: self-contained GUI, firmware bundled, and a portable.txt marker that
# makes the manager keep its settings and machines inside the package folder.
[CmdletBinding()]
param(
    [ValidatePattern('^[0-9]+\.[0-9]+\.[0-9]+(?:-[A-Za-z0-9.-]+)?$')]
    [string]$Version = "0.1.1-preview.1",
    [switch]$NoFirmware,
    [switch]$SkipBuild,
    [switch]$FrameworkDependent
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$dist = Join-Path $root "dist"
$name = "SandVM-$Version-win64"
$stage = Join-Path $dist $name
if ([IO.Path]::GetDirectoryName([IO.Path]::GetFullPath($stage)) -ne [IO.Path]::GetFullPath($dist)) { throw "Invalid package path." }
$zip = Join-Path $dist "$name.zip"

# sandvm --version must match the package name, or the release would mislabel the binary.
$versionHeader = Get-Content -LiteralPath (Join-Path $root "src\core\version.h") -Raw
if ($versionHeader -notmatch "kVersion\[\] = `"$([regex]::Escape($Version))`"") {
    throw "src\core\version.h kVersion does not match -Version $Version."
}

if (-not $SkipBuild) {
    & (Join-Path $PSScriptRoot "build.ps1") -Out "build/sandvm.exe" -Configuration Release
    if ($LASTEXITCODE -ne 0) { throw "sandvm build failed." }
}

$exe = Join-Path $root "build\sandvm.exe"
if (-not (Test-Path -LiteralPath $exe)) {
    throw "build\sandvm.exe not found; run tools\build.ps1 first."
}
$reported = (& $exe --version | Out-String).Trim()
if ($reported -ne "SandVM $Version") { throw "build\sandvm.exe reports '$reported', expected 'SandVM $Version'." }

if (-not $NoFirmware) {
    foreach ($fw in @("bios-256k.bin", "vgabios-stdvga.bin")) {
        if (-not (Test-Path -LiteralPath (Join-Path $root "firmware\$fw"))) {
            throw "firmware\$fw not found; copy it from QEMU or pass -NoFirmware."
        }
    }
}

if (Test-Path -LiteralPath $stage) { Remove-Item -LiteralPath $stage -Recurse -Force }
New-Item -ItemType Directory -Path $stage -Force | Out-Null
Copy-Item -LiteralPath $exe -Destination (Join-Path $stage "sandvm.exe")

# GUI: self-contained by default so the folder runs on a machine without .NET.
$guiDir = Join-Path $stage "SandVM.Manager"
$csproj = Join-Path $root "gui\SandVM.Manager\SandVM.Manager.csproj"
$publishArgs = @("publish", $csproj, "-c", "Release", "-o", $guiDir, "--nologo", "-p:Version=$Version",
                 "-p:DebugType=none")
if ($FrameworkDependent) {
    $publishArgs += @("--self-contained", "false")
} else {
    $publishArgs += @("-r", "win-x64", "--self-contained", "true")
}
& dotnet @publishArgs
if ($LASTEXITCODE -ne 0) {
    if (-not $FrameworkDependent) { throw "Self-contained publish failed; refusing to create a runtime-dependent portable package." }
    Write-Warning "dotnet publish with restore failed; retrying with the existing restore assets (--no-restore)."
    if (Test-Path -LiteralPath $guiDir) { Remove-Item -LiteralPath $guiDir -Recurse -Force }
    & dotnet @publishArgs --no-restore
    if ($LASTEXITCODE -ne 0) {
        throw "GUI publish failed. Run 'dotnet restore gui\SandVM.Manager\SandVM.Manager.csproj' and retry."
    }
}

foreach ($file in @("README.md", "LICENSE", "CHANGELOG.md", "THIRD_PARTY_NOTICES.md")) {
    Copy-Item -LiteralPath (Join-Path $root $file) -Destination $stage
}
Copy-Item -LiteralPath (Join-Path $root "docs") -Destination (Join-Path $stage "docs") -Recurse

@"
This file turns on portable mode. SandVM.Manager keeps its settings in data\ and new
virtual machines in machines\ next to this file instead of %APPDATA% and Documents.
Paths inside this folder are stored relative to it, so the folder can be moved or run
from a USB stick. Delete this file to use the per-user locations instead.
"@ | Set-Content -LiteralPath (Join-Path $stage "portable.txt") -Encoding UTF8

$firmwareDir = Join-Path $stage "firmware"
New-Item -ItemType Directory -Path $firmwareDir -Force | Out-Null
if (-not $NoFirmware) {
    foreach ($fw in @("bios-256k.bin", "vgabios-stdvga.bin")) {
        Copy-Item -LiteralPath (Join-Path $root "firmware\$fw") -Destination $firmwareDir
    }
    # LGPLv3 section 4 / GPLv3 section 6: the release page carries the matching source archive.
    @"
bios-256k.bin       SeaBIOS 1.17.0 (rel-1.17.0-0-gb52ca86e094d, QEMU prebuilt)
vgabios-stdvga.bin  SeaVGABIOS 1.17.0 (same source tree)

Both are licensed under the GNU Lesser General Public License v3 (LGPL-3.0-only).
They are unmodified. The corresponding source code is published as a separate asset
of this SandVM release (seabios-1.17.0.tar.gz) and upstream at
https://www.seabios.org/downloads/seabios-1.17.0.tar.gz
License texts: COPYING and COPYING.LESSER inside that archive, and
https://www.gnu.org/licenses/lgpl-3.0.txt
"@ | Set-Content -LiteralPath (Join-Path $firmwareDir "NOTICE.txt") -Encoding UTF8
} else {
    @"
SandVM needs two firmware images for --bios and for the manager GUI:

  bios-256k.bin         SeaBIOS
  vgabios-stdvga.bin    SeaVGABIOS

They are not bundled in this package. Copy them from a QEMU installation into this
folder. The command line still works without them for `sandvm selftest` and
`sandvm run --kernel`.
"@ | Set-Content -LiteralPath (Join-Path $firmwareDir "README.txt") -Encoding UTF8
}

$commit = "uncommitted"
$previousErrorAction = $ErrorActionPreference
$ErrorActionPreference = "SilentlyContinue"
$revision = & git -C $root rev-parse --short HEAD 2>$null
if ($LASTEXITCODE -eq 0 -and $revision) {
    $commit = "$revision".Trim()
    $dirty = & git -C $root status --porcelain --untracked-files=no 2>$null
    if ($dirty) { $commit += "-dirty" }
}
$ErrorActionPreference = $previousErrorAction
$runtime = if ($FrameworkDependent) { "framework-dependent (.NET 8 Desktop Runtime required)" } else { "self-contained win-x64" }
@(
    "SandVM $Version",
    "built: $(Get-Date -Format 'yyyy-MM-dd HH:mm:ss')",
    "commit: $commit",
    "configuration: Release (llvm-mingw, x86-64)",
    "gui: $runtime",
    "firmware: $(if ($NoFirmware) { 'not bundled' } else { 'SeaBIOS 1.17.0 bundled' })"
) | Set-Content -LiteralPath (Join-Path $stage "VERSION.txt") -Encoding ASCII

if (Test-Path -LiteralPath $zip) { Remove-Item -LiteralPath $zip -Force }
Compress-Archive -LiteralPath $stage -DestinationPath $zip -CompressionLevel Optimal

$zipHash = (Get-FileHash -LiteralPath $zip -Algorithm SHA256).Hash.ToLower()
$exeHash = (Get-FileHash -LiteralPath $exe -Algorithm SHA256).Hash.ToLower()
@(
    "$zipHash  $name.zip",
    "$exeHash  sandvm.exe"
) | Set-Content -LiteralPath (Join-Path $dist "SHA256SUMS.txt") -Encoding ASCII

Write-Host "package: $zip"
Write-Host "hashes : $(Join-Path $dist 'SHA256SUMS.txt')"
