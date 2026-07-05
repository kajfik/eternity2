# Build the Eternity II solver with WinLibs GCC.
$gxx = "C:\Users\kajfj\AppData\Local\Microsoft\WinGet\Packages\BrechtSanders.WinLibs.POSIX.UCRT_Microsoft.Winget.Source_8wekyb3d8bbwe\mingw64\bin\g++.exe"
if (-not (Test-Path $gxx)) { $gxx = (Get-Command g++).Source }
$root = Split-Path $PSScriptRoot -Parent
& $gxx -O3 -march=native -funroll-loops -std=c++20 -static -pthread `
    -o "$root\solver.exe" "$root\src\solver.cpp"
if ($LASTEXITCODE -eq 0) { Write-Host "built $root\solver.exe" } else { exit 1 }
