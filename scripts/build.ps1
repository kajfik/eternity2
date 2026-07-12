# Build the Eternity II solver with WinLibs GCC.
#
#   .\scripts\build.ps1            # native build -> solver.exe (fastest on THIS machine;
#                                  # -march=native, do not ship to other PCs)
#   .\scripts\build.ps1 -Portable  # portable build -> bin\solver.exe (-march=x86-64-v3,
#                                  # runs on any AVX2 CPU ~2015+; this is the exe that
#                                  # gets committed so fresh clones run without gcc)
param([switch]$Portable)
$gxx = "C:\Users\kajfj\AppData\Local\Microsoft\WinGet\Packages\BrechtSanders.WinLibs.POSIX.UCRT_Microsoft.Winget.Source_8wekyb3d8bbwe\mingw64\bin\g++.exe"
if (-not (Test-Path $gxx)) { $gxx = (Get-Command g++).Source }
$root = Split-Path $PSScriptRoot -Parent
if ($Portable) {
    $arch = 'x86-64-v3'
    $out = Join-Path $root 'bin\solver.exe'
    New-Item -ItemType Directory -Force (Join-Path $root 'bin') | Out-Null
} else {
    $arch = 'native'
    $out = Join-Path $root 'solver.exe'
}
& $gxx -O3 "-march=$arch" -funroll-loops -std=c++20 -static -pthread `
    -o $out "$root\src\solver.cpp"
if ($LASTEXITCODE -eq 0) { Write-Host "built $out (-march=$arch)" } else { exit 1 }
