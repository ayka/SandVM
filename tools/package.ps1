# Builds the v0.1.0 binary release folder and zip under dist\.
#
#   .\tools\package.ps1
#   .\tools\package.ps1 -IncludeFirmware      # also bundle SeaBIOS/SeaVGABIOS for local use
#   .\tools\package.ps1 -SkipBuild            # reuse build\sandvm.exe
#   .\tools\package.ps1 -SelfContained        # bundle the .NET 8 desktop runtime with the GUI
[CmdletBinding()]
param(
    [string]$Version = "0.1.0",
    [switch]$IncludeFirmware,
    [switch]$SkipBuild,
    [switch]$SelfContained
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$dist = Join-Path $root "dist"
$name = "SandVM-$Version-win64"
$stage = Join-Path $dist $name
$zip = Join-Path $dist "$name.zip"

if (-not $SkipBuild) {
    & (Join-Path $PSScriptRoot "build.ps1") -Out "build/sandvm.exe" -Configuration Release
    if ($LASTEXITCODE -ne 0) { throw "sandvm build failed." }
}

$exe = Join-Path $root "build\sandvm.exe"
if (-not (Test-Path -LiteralPath $exe)) {
    throw "build\sandvm.exe not found; run tools\build.ps1 first."
}

if (Test-Path -LiteralPath $stage) { Remove-Item -LiteralPath $stage -Recurse -Force }
New-Item -ItemType Directory -Path $stage -Force | Out-Null
Copy-Item -LiteralPath $exe -Destination (Join-Path $stage "sandvm.exe")

# GUI: framework-dependent by default, self-contained with -SelfContained.
$guiDir = Join-Path $stage "SandVM.Manager"
$csproj = Join-Path $root "gui\SandVM.Manager\SandVM.Manager.csproj"
$publishArgs = @("publish", $csproj, "-c", "Release", "-o", $guiDir, "--nologo")
if ($SelfContained) {
    $publishArgs += @("-r", "win-x64", "--self-contained", "true")
}
& dotnet @publishArgs
if ($LASTEXITCODE -ne 0) {
    Write-Warning "dotnet publish with restore failed; retrying with the existing restore assets (--no-restore)."
    if (Test-Path -LiteralPath $guiDir) { Remove-Item -LiteralPath $guiDir -Recurse -Force }
    & dotnet publish $csproj -c Release -o $guiDir --nologo --no-restore
    if ($LASTEXITCODE -ne 0) {
        throw "GUI publish failed. Run 'dotnet restore gui\SandVM.Manager\SandVM.Manager.csproj' and retry."
    }
    if ($SelfContained) {
        Write-Warning "Self-contained publish was not possible; the GUI is framework-dependent and needs .NET 8 Desktop Runtime."
    }
}

foreach ($file in @("README.md", "LICENSE", "CHANGELOG.md", "THIRD_PARTY_NOTICES.md")) {
    Copy-Item -LiteralPath (Join-Path $root $file) -Destination $stage
}
Copy-Item -LiteralPath (Join-Path $root "docs") -Destination (Join-Path $stage "docs") -Recurse

# Firmware is user-supplied by default (LGPL source/notice obligations, and it is not in git).
$firmwareDir = Join-Path $stage "firmware"
New-Item -ItemType Directory -Path $firmwareDir -Force | Out-Null
if ($IncludeFirmware) {
    foreach ($fw in @("bios-256k.bin", "vgabios-stdvga.bin")) {
        Copy-Item -LiteralPath (Join-Path $root "firmware\$fw") -Destination $firmwareDir
    }
    @"
SeaBIOS (bios-256k.bin) and SeaVGABIOS (vgabios-stdvga.bin) are LGPLv3 components.
They are bundled here for local use only. If you redistribute this folder, keep this
notice and provide the corresponding source code. Upstream: https://www.seabios.org/
"@ | Set-Content -LiteralPath (Join-Path $firmwareDir "NOTICE.txt") -Encoding UTF8
} else {
    @"
SandVM needs two firmware images for --bios and for the manager GUI:

  bios-256k.bin         SeaBIOS
  vgabios-stdvga.bin    SeaVGABIOS

They are not bundled in this release (LGPLv3 source/notice obligations and size).
Copy them from a QEMU installation into this folder. The command line still works
without them for `sandvm selftest` and `sandvm run --kernel`.
"@ | Set-Content -LiteralPath (Join-Path $firmwareDir "README.txt") -Encoding UTF8
}

$commit = "uncommitted"
$previousErrorAction = $ErrorActionPreference
$ErrorActionPreference = "SilentlyContinue"
$revision = & git -C $root rev-parse --short HEAD 2>$null
if ($LASTEXITCODE -eq 0 -and $revision) { $commit = "$revision".Trim() }
$ErrorActionPreference = $previousErrorAction
@(
    "SandVM $Version",
    "built: $(Get-Date -Format 'yyyy-MM-dd HH:mm:ss')",
    "commit: $commit",
    "configuration: Release (llvm-mingw, x86-64)"
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
