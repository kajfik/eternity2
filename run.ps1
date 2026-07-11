# Eternity II hunt launcher. Works on a fresh clone: builds solver.exe if
# missing, then asks (or takes as parameters) everything a run needs.
#
#   from-scratch : fresh lineage in runs_scratch\ (gitignored). Continues the
#                  previous scratch best if one exists, unless you archive it.
#   from-best    : record hunt resuming runs\best_c{1,5}.txt (tracked seeds).
#
# Interactive:      .\run.ps1          (or double-click run.cmd)
# Non-interactive:  .\run.ps1 -Mode scratch -Clues 5 -Threads 30 -Minutes 90
#                   .\run.ps1 -Mode best -Clues 1
#                   .\run.ps1 -Mode scratch -Clues 5 -Fresh   # archive old scratch best first
#
# Ctrl+C is always safe: progress is written to <outdir>\best_c<clues>.txt on
# every new best. Console output is teed to <outdir>\run_c<clues>_<ts>.log.
#
# ============================= MAINTENANCE NOTE ==============================
# The $ScratchArgs / $BestArgs blocks below are the SINGLE SOURCE OF TRUTH for
# the current best-known flag sets. Claude instances: whenever a new flag
# configuration is proven better (A/B tested, e2lib-verified), update these
# blocks and the matching note in CLAUDE.md as part of adopting it. Do not add
# unproven flags here.
# =============================================================================
param(
    [ValidateSet('scratch','best')][string]$Mode,
    [int]$Clues = 0,        # 1 or 5
    [int]$Threads = 0,      # 0 = solver default (cores-2)
    [double]$Minutes = 0,   # 0 = run until Ctrl+C
    [switch]$Fresh          # scratch only: archive an existing scratch best/history first
)
$ErrorActionPreference = 'Stop'
Set-Location $PSScriptRoot
$interactive = -not $PSBoundParameters.ContainsKey('Mode')

# --- best-known flag sets (see maintenance note above) -----------------------
# From-scratch, proven 2026-07-10/11 (CLAUDE.md "Verhaard adoptions" +
# theft-guard A/B): graded slip schedule + adaptive restart aborts lift the
# 90-s from-scratch best from 432 to 444-448.
$ScratchArgs = @(
    '--order','model','--model-file','eval\model_weights.txt',
    '--slip-target','460',
    '--abort-points','2000000:60,10000000:120'
)
# From-best (record hunts resume a high seed where hunter output is pool
# food): the long-run-proven set from scripts\run5.ps1 / run1.ps1. Slip is
# only smoke-tested from scratch, not on resumed record lineages.
$BestArgs = @(
    '--order','model','--model-file','eval\model_weights.txt'
)
# -----------------------------------------------------------------------------

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

if (-not (Test-Path .\solver.exe)) {
    Write-Host "solver.exe not found - building via scripts\build.ps1 ..."
    & "$PSScriptRoot\scripts\build.ps1"
    if (-not (Test-Path .\solver.exe)) { throw "build failed - no solver.exe" }
}

if ($Mode -eq 'scratch') {
    $outDir = 'runs_scratch'
    $modeArgs = $ScratchArgs
    New-Item -ItemType Directory -Force $outDir | Out-Null
    $bestFile = Join-Path $outDir "best_c$Clues.txt"
    if (Test-Path $bestFile) {
        $doFresh = [bool]$Fresh
        if (-not $Fresh -and $interactive) {
            Write-Host "$bestFile exists - the solver would resume that scratch lineage."
            do { $sel = Read-Host "[C]ontinue it or [A]rchive and start truly fresh (c/a)" } until ($sel -match '^[caCA]$')
            $doFresh = ($sel -match '^[aA]$')
        }
        if ($doFresh) {
            $ts = Get-Date -Format yyyyMMdd_HHmm
            foreach ($f in @("best_c$Clues.txt", "history_c$Clues.log")) {
                $p = Join-Path $outDir $f
                if (Test-Path $p) { Move-Item $p "$p.$ts.bak"; Write-Host "archived $p -> $p.$ts.bak" }
            }
        } elseif (-not $interactive) {
            Write-Host "note: $bestFile exists, resuming that scratch lineage (pass -Fresh to archive it)."
        }
    }
} else {
    $outDir = 'runs'
    $modeArgs = $BestArgs
}

$solverArgs = @('--clues', "$Clues", '--out', $outDir,
                '--drift-dir', (Join-Path $outDir 'drift')) + $modeArgs
if ($Threads -gt 0) { $solverArgs += @('--threads', "$Threads") }
if ($Minutes -gt 0) { $solverArgs += @('--minutes', "$Minutes") }

$log = Join-Path $outDir ("run_c{0}_{1}.log" -f $Clues, (Get-Date -Format yyyyMMdd_HHmm))
Write-Host ""
Write-Host "solver.exe $($solverArgs -join ' ')"
Write-Host "log: $log   best: $(Join-Path $outDir "best_c$Clues.txt")"
Write-Host ""
.\solver.exe @solverArgs | Tee-Object -FilePath $log
