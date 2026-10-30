# Builds sandvm.exe with the portable llvm-mingw toolchain in .tools\llvm-mingw.
# This is the Windows-native equivalent of tools/build.sh (which needs bash).
#
#   .\tools\build.ps1
#   .\tools\build.ps1 -Configuration Debug
#   .\tools\build.ps1 -Out build\sandvm-test.exe
[CmdletBinding()]
param(
    [string]$Out = "build/sandvm.exe",
    [ValidateSet("Debug", "Release")]
    [string]$Configuration = "Release"
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$cxx = Join-Path $root ".tools\llvm-mingw\bin\clang++.exe"
if (-not (Test-Path -LiteralPath $cxx)) {
    $fallback = Get-Command clang++ -ErrorAction SilentlyContinue
    if (-not $fallback) {
        throw "clang++ not found. Put llvm-mingw in .tools\llvm-mingw or add clang++ to PATH."
    }
    $cxx = $fallback.Source
}

$outPath = if ([System.IO.Path]::IsPathRooted($Out)) { $Out } else { Join-Path $root $Out }
$outDir = Split-Path -Parent $outPath
if ($outDir -and -not (Test-Path -LiteralPath $outDir)) {
    New-Item -ItemType Directory -Path $outDir -Force | Out-Null
}

$sources = Get-ChildItem -LiteralPath (Join-Path $root "src") -Recurse -Filter *.cpp |
    Sort-Object FullName |
    ForEach-Object { $_.FullName }
if ($sources.Count -eq 0) {
    throw "No C++ sources found under src."
}

$optimization = if ($Configuration -eq "Debug") { "-O0" } else { "-O2" }
$cxxArgs = @(
    "-std=c++20", $optimization, "-g", "-Wall", "-Wextra", "-Werror=return-type",
    "-DWIN32_LEAN_AND_MEAN", "-DNOMINMAX", "-DUNICODE", "-D_UNICODE", "-D_WIN32_WINNT=0x0A00",
    "-I$(Join-Path $root 'src')"
) + $sources + @(
    "-o", $outPath, "-static",
    "-lshell32", "-luser32", "-lgdi32", "-lws2_32", "-liphlpapi", "-lcomdlg32"
)

& $cxx @cxxArgs
if ($LASTEXITCODE -ne 0) {
    throw "Build failed with exit code $LASTEXITCODE."
}
Write-Host "built $outPath"
