# Plateau-merge launcher: CP-SAT-merge the current best board with its
# sideways-drift snapshots (the 464-record author's mass same-score harvest,
# done offline on our own drift files). Wraps tools\plateau_merge.py with the
# proven arguments:
#
#   --include <best file>  injects the best board itself into the snapshot
#                          pool so it gets paired against its drift variants
#   --max-window 40        same-lineage drift pairs disagree on 4-35 cells;
#                          >40 means cross-basin, practically undecidable
#   --time-per-pair 60     improve-only CP-SAT windows either decide in
#                          seconds or resist for 300 s+; 60 s catches the
#                          deciders without burning time on the resisters
#                          (lower score groups only)
#   --time-top 600         the TOP score group's pairs get 10x: observed
#                          22-cell top pairs decide in 20-32 s, 21+-cell
#                          pairs go UNKNOWN at 60 s (2026-07-15 P5)
#   --workers 16           the CP-SAT worker count every probe so far used
#
# Improvements are independently e2lib-verified and written to
# runs\candidate_merge_<score>.txt with their bucas URL. Solved pairs are
# remembered in <drift>\.plateau_merge.state, so re-running is cheap.
#
# Usage:
#   .\scripts\merge.ps1                          # runs_scratch\best_c5.txt + runs_scratch\drift
#   .\scripts\merge.ps1 -Clues 1 -OutDir runs    # a record lineage instead
#   .\scripts\merge.ps1 -Loop                    # keep harvesting while a solver runs
#
# run.ps1 -OnStagnation merge calls this automatically when its stagnation
# watchdog ends a run. Needs python 3 with ortools (pip install ortools).
param(
    [int]$Clues = 5,
    [string]$OutDir = 'runs_scratch',
    [double]$TimeBudget = 1800,   # overall wall-clock cap (s); 0 = solve every pending pair
    [double]$TimePerPair = 60,
    [double]$TimeTop = 600,       # per-pair cap for the top score group
    [int]$Workers = 16,
    [int]$MaxWindow = 40,
    [int]$MaxBoards = 500,        # per-score-group board cap (subsampled) so a
                                  # --drift-cap-4096 harvest doesn't make pass
                                  # startup O(n^2) over thousands of boards
    [switch]$Loop
)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
Set-Location $root

if ($Clues -ne 1 -and $Clues -ne 5) { throw "Clues must be 1 or 5" }
$bestFile = Join-Path $OutDir "best_c$Clues.txt"
$driftDir = Join-Path $OutDir 'drift'
if (-not (Test-Path $bestFile)) { throw "$bestFile not found - nothing to merge" }

$snapFiles = @()
if (Test-Path $driftDir) {
    $snapFiles = @(Get-ChildItem $driftDir -Filter 'drift_t*.txt' -ErrorAction SilentlyContinue)
    $snapFiles += @(Get-ChildItem $driftDir -Filter 'plateau.txt' -ErrorAction SilentlyContinue)
}
if ($snapFiles.Count -eq 0) {
    Write-Host "warning: no drift snapshots in $driftDir - only the best board itself is in the pool, so no pairs can form."
    Write-Host "         Drift files appear once a solver run stalls at/near its best (run.ps1 always passes --drift-dir)."
}

# Probe `python` then the `py` launcher (some installs only have `py`); the
# cmd /c wrapper keeps a failed import silent and sidesteps PS 5.1's
# native-stderr wrapping. The Store `python` alias stub fails both probes.
$python = $null
$pyProblem = ''
foreach ($cand in @('python', 'py')) {
    if (-not (Get-Command $cand -ErrorAction SilentlyContinue)) { continue }
    cmd /c "$cand -c `"import ortools`" >nul 2>&1"
    if ($LASTEXITCODE -eq 0) { $python = $cand; break }
    cmd /c "$cand -c `"import sys`" >nul 2>&1"
    if ($LASTEXITCODE -eq 0 -and -not $pyProblem) { $pyProblem = "$cand works but ortools is missing: $cand -m pip install ortools" }
}
if (-not $python) {
    if (-not $pyProblem) { $pyProblem = "no python found on PATH (tried 'python' and 'py'). The offline plateau merge is an OPTIONAL extra (hunts via run.ps1 never need python; the solver has an in-process merge move built in). To use it: install Python 3, then: py -m pip install ortools." }
    throw $pyProblem
}

if (-not (Test-Path $driftDir)) { New-Item -ItemType Directory -Force $driftDir | Out-Null }
$pyArgs = @('tools\plateau_merge.py',
            '--dir', $driftDir, '--clues', "$Clues", '--include', $bestFile,
            '--time', "$TimeBudget", '--time-per-pair', "$TimePerPair",
            '--time-top', "$TimeTop",
            '--workers', "$Workers", '--max-window', "$MaxWindow",
            '--max-boards', "$MaxBoards")
if ($Loop) { $pyArgs += '--loop' }
Write-Host "$python $($pyArgs -join ' ')"
& $python @pyArgs
$code = $LASTEXITCODE

$cand = Get-ChildItem (Join-Path $root 'runs') -Filter 'candidate_merge_*.txt' -ErrorAction SilentlyContinue
if ($cand) {
    Write-Host "candidate merge boards on disk (verify + adopt manually):"
    $cand | ForEach-Object { Write-Host "  $($_.FullName)" }
}
exit $code
