# Eternity II hunt launcher. Works on a fresh clone with NO toolchain: builds
# solver.exe natively if g++ is available, otherwise falls back to the
# committed portable build bin\solver.exe (x86-64-v3, any AVX2 CPU ~2015+).
# Asks (or takes as parameters) everything a run needs. Python is never needed
# for hunts - only for the optional offline plateau merge (scripts\merge.ps1).
#
#   from-scratch : fresh lineage in runs_scratch\ (gitignored). Continues the
#                  previous scratch best if one exists, unless you archive it.
#   from-best    : record hunt resuming runs\best_c{1,5}.txt (tracked seeds).
#
# Interactive:      .\run.ps1          (or double-click run.cmd)
# Non-interactive:  .\run.ps1 -Mode scratch -Clues 5 -Threads 30 -Minutes 90
#                   .\run.ps1 -Mode best -Clues 1
#                   .\run.ps1 -Mode scratch -Clues 5 -Fresh   # archive old scratch best first
#                   .\run.ps1 -Mode scratch -Clues 5 -StagnationHours 3 -OnStagnation restart
#
# Stagnation watchdog (-StagnationHours N, prompt "stop after hours without a
# new best"): the launcher watches the best file's write time and kills the
# solver once N hours pass without a new best -- safe, every new best is
# already on disk. -OnStagnation then picks what happens:
#   stop    (default) just stop
#   merge   run scripts\merge.ps1: CP-SAT plateau-merge of the best board
#           with its drift snapshots (needs python + pip install ortools)
#   restart scratch: archive best/history/drift and start a truly fresh run
#           (loops -- one fresh basin per stagnation cycle, until Ctrl+C);
#           best: relaunch the solver resuming the same best file (pools reset)
#
# Ctrl+C is always safe: progress is written to <outdir>\best_c<clues>.txt on
# every new best. Console output is copied to <outdir>\run_c<clues>_<ts>.log
# (plain ASCII bytes via stdout redirect; the old Tee-Object wrote UTF-16).
#
# The canonical best-known flag sets ($ScratchArgs / $BestArgs) live in
# scripts\flags.ps1 (shared with push460.ps1) -- see the maintenance note there.
param(
    [ValidateSet('scratch','best')][string]$Mode,
    [int]$Clues = 0,        # 1 or 5
    [int]$Threads = 0,      # 0 = solver default (cores-2)
    [double]$Minutes = 0,   # 0 = run until Ctrl+C
    [double]$StagnationHours = 0,   # 0 = no stagnation watchdog
    [ValidateSet('stop','merge','restart')][string]$OnStagnation = 'stop',
    [string]$OutDir = '',   # override output dir (default: runs_scratch / runs by mode)
    [switch]$Fresh          # scratch only: archive an existing scratch best/history first
)
$ErrorActionPreference = 'Stop'
Set-Location $PSScriptRoot
$interactive = -not $PSBoundParameters.ContainsKey('Mode')

# --- best-known flag sets: $ScratchArgs / $BestArgs (single source of truth,
# shared with push460.ps1) ----------------------------------------------------
. (Join-Path $PSScriptRoot 'scripts\flags.ps1')

# Archive a scratch lineage: best/history -> <file>.<ts>.bak, drift dir ->
# drift.<ts>.bak (stale drift snapshots would poison the next lineage's
# plateau merges -- plateau_merge.py groups by top score, and the old
# lineage's high boards would shadow the new one's).
function Archive-ScratchState([string]$dir, [int]$clues) {
    $ts = Get-Date -Format yyyyMMdd_HHmmss
    foreach ($f in @("best_c$clues.txt", "history_c$clues.log")) {
        $p = Join-Path $dir $f
        if (Test-Path $p) { Move-Item $p "$p.$ts.bak"; Write-Host "archived $p -> $p.$ts.bak" }
    }
    $d = Join-Path $dir 'drift'
    if ((Test-Path $d) -and (Get-ChildItem $d -ErrorAction SilentlyContinue)) {
        Move-Item $d "$d.$ts.bak"; Write-Host "archived $d -> $d.$ts.bak"
    }
}

if (-not $Mode) {
    Write-Host ""
    Write-Host "  [1] from-scratch : fresh lineage in runs_scratch\ (slip schedule + restart aborts)"
    Write-Host "  [2] from-best    : resume record hunt from runs\best_c{1,5}.txt"
    do { $sel = Read-Host "Run type (1/2)" } until ($sel -eq '1' -or $sel -eq '2')
    if ($sel -eq '1') { $Mode = 'scratch' } else { $Mode = 'best' }
}

if ($Clues -ne 1 -and $Clues -ne 5) {
    do {
        $sel = Read-Host "Clues: 5 (record 464, need >=465) or 1 (record 470, need >=471) [5]"
        if ($sel -eq '') { $sel = '5' }
    } until ($sel -eq '1' -or $sel -eq '5')
    $Clues = [int]$sel
}

if ($Threads -le 0 -and $interactive) {
    $def = [Environment]::ProcessorCount - 2
    if ($def -lt 1) { $def = 1 }
    do { $sel = Read-Host "Threads (Enter = solver default cores-2 = $def)" } until ($sel -eq '' -or $sel -match '^\d+$')
    if ($sel -ne '') { $Threads = [int]$sel }
}

if ($Minutes -le 0 -and $interactive) {
    do { $sel = Read-Host "Minutes (Enter = run until Ctrl+C)" } until ($sel -eq '' -or $sel -match '^\d+(\.\d+)?$')
    if ($sel -ne '') { $Minutes = [double]$sel }
}

if ($StagnationHours -le 0 -and $interactive) {
    do { $sel = Read-Host "Stop after hours without a new best (Enter = never)" } until ($sel -eq '' -or $sel -match '^\d+(\.\d+)?$')
    if ($sel -ne '') { $StagnationHours = [double]$sel }
}

if ($StagnationHours -gt 0 -and $interactive -and -not $PSBoundParameters.ContainsKey('OnStagnation')) {
    Write-Host ""
    Write-Host "  [s] stop    : just stop the solver"
    Write-Host "  [m] merge   : stop, then plateau-merge best + drift snapshots (scripts\merge.ps1;"
    Write-Host "                optional extra - needs Python 3 + pip install ortools; hunts never need python)"
    if ($Mode -eq 'scratch') {
        Write-Host "  [a] archive-and-restart : archive best/history/drift, start a truly fresh run (loops)"
    } else {
        Write-Host "  [a] restart : relaunch the solver on the same best file (pools reset; loops)"
    }
    do { $sel = Read-Host "On stagnation (s/m/a) [s]" } until ($sel -eq '' -or $sel -match '^[smaSMA]$')
    if ($sel -match '^[mM]$') { $OnStagnation = 'merge' }
    elseif ($sel -match '^[aA]$') { $OnStagnation = 'restart' }
    else { $OnStagnation = 'stop' }
}

# Resolve the solver exe. Preference order:
#   1. .\solver.exe            (native -march=native build, this machine only, gitignored)
#   2. build it with g++       (scripts\build.ps1, if a toolchain is present)
#   3. .\bin\solver.exe        (committed portable -march=x86-64-v3 build: any AVX2 CPU
#                               ~2015+; lets a fresh clone run with no gcc installed)
$exe = Join-Path $PSScriptRoot 'solver.exe'
if (-not (Test-Path $exe)) {
    Write-Host "solver.exe not found - trying a native build via scripts\build.ps1 ..."
    try { & "$PSScriptRoot\scripts\build.ps1" } catch { Write-Host "build unavailable: $_" }
    if (-not (Test-Path $exe)) {
        $portable = Join-Path $PSScriptRoot 'bin\solver.exe'
        if (Test-Path $portable) {
            Write-Host "using the committed portable build bin\solver.exe (x86-64-v3; needs an AVX2 CPU, ~2015+)."
            Write-Host "for a machine-tuned exe later: install g++ and run scripts\build.ps1"
            $exe = $portable
        } else {
            throw "no solver exe: install g++ and run scripts\build.ps1, or restore bin\solver.exe from the repo"
        }
    }
}

if ($Mode -eq 'scratch') {
    if ($OutDir -eq '') { $OutDir = 'runs_scratch' }
    $modeArgs = $ScratchArgs
    New-Item -ItemType Directory -Force $OutDir | Out-Null
    $bestFile = Join-Path $OutDir "best_c$Clues.txt"
    if (Test-Path $bestFile) {
        $doFresh = [bool]$Fresh
        if (-not $Fresh -and $interactive) {
            Write-Host "$bestFile exists - the solver would resume that scratch lineage."
            do { $sel = Read-Host "[C]ontinue it or [A]rchive and start truly fresh (c/a)" } until ($sel -match '^[caCA]$')
            $doFresh = ($sel -match '^[aA]$')
        }
        if ($doFresh) {
            Archive-ScratchState $OutDir $Clues
        } elseif (-not $interactive) {
            Write-Host "note: $bestFile exists, resuming that scratch lineage (pass -Fresh to archive it)."
        }
    }
} else {
    if ($OutDir -eq '') { $OutDir = 'runs' }
    $modeArgs = $BestArgs
    New-Item -ItemType Directory -Force $OutDir | Out-Null
}

$solverArgs = @('--clues', "$Clues", '--out', $OutDir,
                '--drift-dir', (Join-Path $OutDir 'drift')) + $modeArgs
if ($Threads -gt 0) { $solverArgs += @('--threads', "$Threads") }
if ($Minutes -gt 0) { $solverArgs += @('--minutes', "$Minutes") }

$bestFile = Join-Path $OutDir "best_c$Clues.txt"
# PS 5.1 Start-Process joins -ArgumentList with spaces and no quoting; quote
# any element containing whitespace so paths with spaces survive.
$argLine = ($solverArgs | ForEach-Object { if ($_ -match '\s') { '"' + $_ + '"' } else { $_ } }) -join ' '

while ($true) {
    $log = Join-Path $OutDir ("run_c{0}_{1}.log" -f $Clues, (Get-Date -Format yyyyMMdd_HHmmss))
    $errLog = "$log.err"
    Write-Host ""
    Write-Host "$exe $argLine"
    Write-Host "log: $log   best: $bestFile"
    if ($StagnationHours -gt 0) {
        Write-Host ("stagnation watchdog: {0} h without a new best -> {1}" -f $StagnationHours, $OnStagnation)
    }
    Write-Host ""

    $runStart = Get-Date
    $proc = Start-Process -FilePath $exe -ArgumentList $argLine -WorkingDirectory $PSScriptRoot `
                          -NoNewWindow -PassThru `
                          -RedirectStandardOutput $log -RedirectStandardError $errLog
    while (-not (Test-Path $log)) { Start-Sleep -Milliseconds 100 }
    # Tail the log to the console while watching the best file for progress.
    $fs = [IO.File]::Open($log, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::ReadWrite)
    $sr = New-Object IO.StreamReader($fs)
    $stagnated = $false
    try {
        while ($true) {
            $chunk = $sr.ReadToEnd()
            if ($chunk) { Write-Host -NoNewline $chunk }
            if ($proc.HasExited) {
                Start-Sleep -Milliseconds 200
                $chunk = $sr.ReadToEnd()
                if ($chunk) { Write-Host -NoNewline $chunk }
                break
            }
            if ($StagnationHours -gt 0) {
                $last = $runStart
                if (Test-Path $bestFile) {
                    $mt = (Get-Item $bestFile).LastWriteTime
                    if ($mt -gt $last) { $last = $mt }
                }
                if (((Get-Date) - $last).TotalHours -ge $StagnationHours) {
                    $stagnated = $true
                    Stop-Process -Id $proc.Id -Force
                    $proc.WaitForExit()
                    $chunk = $sr.ReadToEnd()
                    if ($chunk) { Write-Host -NoNewline $chunk }
                    Write-Host ""
                    Write-Host ("[launcher] no new best for {0} h - solver stopped (best board is safe in {1})" -f $StagnationHours, $bestFile)
                    break
                }
            }
            Start-Sleep -Milliseconds 500
        }
    } finally {
        $sr.Close()
    }
    if (Test-Path $errLog) {
        $err = Get-Content $errLog -Raw
        if ($err -and $err.Trim()) { Write-Host "--- solver stderr ---"; Write-Host $err }
        else { Remove-Item $errLog -Force }
    }

    if (-not $stagnated) { break }   # solver ended on its own (--minutes, Ctrl+C, crash)

    if ($OnStagnation -eq 'merge') {
        Write-Host "[launcher] plateau-merging $bestFile + $(Join-Path $OutDir 'drift') ..."
        & (Join-Path $PSScriptRoot 'scripts\merge.ps1') -Clues $Clues -OutDir $OutDir
        break
    } elseif ($OnStagnation -eq 'restart') {
        if ($Mode -eq 'scratch') {
            Write-Host "[launcher] archive-and-restart:"
            Archive-ScratchState $OutDir $Clues
        } else {
            Write-Host "[launcher] restarting solver on the same best file (pools reset)"
        }
        continue
    } else {
        break
    }
}
