# push460.ps1 -- the "reach 460 from scratch on our own" pipeline, in one
# console. Runs three engines side by side and glues them into a feedback
# loop, based on the 2026-07-12 strategy analysis (see CLAUDE.md):
#
#   1. SOLVER   solver.exe resuming <OutDir>\best_c<Clues>.txt with the proven
#               from-scratch flags (scripts\flags.ps1 $ScratchArgs) and a big
#               drift harvest (--drift-cap, default 4096 snapshots/thread
#               instead of the old 64) -- mass same-score plateau harvest, the
#               464 record author's method.
#   2. MERGE    tools\plateau_merge.py --loop: continuously CP-SAT-merges
#               pairs of drift snapshots (+ the best board itself); each
#               improvement lands in runs\candidate_merge_<score>_<hash>.txt.
#   3. SWEEP    tools\window_sweep.py, rotated over the best board and every
#               near-best candidate: exact improve-only CP-SAT windows over
#               fault cells ("does ANY strictly better arrangement of this
#               window's pieces exist?"). Runs in resumable chunks; pass 1
#               uses default shapes at --time-per <SweepTimePer>, pass 2
#               retries UNKNOWN windows at >=600 s each.
#
#   GLUE (this script's poll loop, every 30 s):
#   - ADOPT: any merge/sweep candidate scoring above the current best is fed
#     back into the solver (restart with --seed-edges <candidate>; the solver
#     verifies conformance itself and writes the new best file). The improved
#     board then gets drifted, merged, and swept in turn.
#   - STAGNATION: no new best for -StagnationHours -> restart the solver on
#     the same best file (pools reset; the lineage continues).
#   - EXHAUSTION: no new best for -ExhaustedHours AND the sweep queue is fully
#     drained (every near-best target has both passes done, no sweep running,
#     no adoptable candidate -- any new merge candidate re-fills the queue, so
#     a drained queue really means merge+sweep have nothing left on this
#     basin). -OnExhausted picks what happens: 'notify' (default; beeps and
#     prints a banner every hour, run keeps going) or 'restart' (archive the
#     lineage like -Fresh and start a new basin automatically).
#   - VICTORY: best >= -Target (default 460) -> stop everything, print the
#     bucas URL.
#
# Everything is safe to Ctrl+C: the solver writes best_c*.txt on every new
# best, sweep/merge state files make their work resumable, and the finally
# block stops all children. Re-running the script continues where it left off.
#
# Usage:
#   .\push460.ps1                          # INTERACTIVE: prompts for lineage
#                                          # (continue/archive), thread budget,
#                                          # target, minutes, stagnation, and
#                                          # native-build choice (= push460.cmd)
#   .\push460.ps1 -Target 460              # any parameter -> scripted, no prompts
#   .\push460.ps1 -SolverThreads 24        # more solver, less CP-SAT headroom
#   .\push460.ps1 -TotalThreads 16         # cap the whole pipeline's budget
#   .\push460.ps1 -Minutes 10              # timed smoke run
#   .\push460.ps1 -Fresh                   # archive the lineage, start over
#   .\push460.ps1 -NoSweep                 # solver + merge only (less python load)
#   .\push460.ps1 -OnExhausted restart     # auto-archive + fresh basin when the
#                                          # lineage is exhausted (see above)
#   .\push460.ps1 -ExhaustedHours 10       # be more patient before declaring it
#
# Needs python 3 + `pip install ortools` for MERGE/SWEEP (pass -NoMerge
# -NoSweep to run the solver alone without python).
#
# Thread budget: -TotalThreads (or the interactive prompt) sets the budget for
# all three engines, default = logical cores - 2 (headroom for the OS and this
# orchestrator); it is split 62/19/19 % solver/merge/sweep (18/5/5 on the
# 32-thread reference box; 8/2/2 on 16 threads).
# -SolverThreads/-MergeWorkers/-SweepWorkers override individual slices.
# CP-SAT workers are bursty, so mild oversubscription is fine; lower the
# budget if the box feels sluggish.
param(
    [int]$Clues = 5,
    [int]$Target = 460,
    [int]$TotalThreads = 0,    # 0 = logical cores - 2; the 62/19/19 split divides this budget
    [int]$SolverThreads = 0,   # 0 = auto (~62% of the budget)
    [int]$MergeWorkers = 0,    # 0 = auto (~19%)
    [int]$SweepWorkers = 0,    # 0 = auto (~19%)
    [double]$StagnationHours = 2,
    [double]$ExhaustedHours = 6,       # no new best this long + sweep drained = basin exhausted (0 = never)
    [ValidateSet('notify','restart')]
    [string]$OnExhausted = 'notify',   # notify: hourly beep+banner; restart: auto -Fresh
    [int]$DriftCap = 4096,
    [string]$OutDir = 'runs_scratch',
    [double]$SweepChunkMinutes = 45,   # one sweep child invocation's budget
    [double]$SweepTimePer = 300,       # CP-SAT cap per window, pass 1
    [double]$Minutes = 0,              # overall wall-clock cap (0 = until Ctrl+C/target)
    [switch]$Fresh,                    # archive best/history/drift/sweep, start a new basin
    [switch]$NoMerge,
    [switch]$NoSweep
)
$ErrorActionPreference = 'Stop'
Set-Location $PSScriptRoot
. (Join-Path $PSScriptRoot 'scripts\flags.ps1')   # $ScratchArgs (canonical)

# --- interactive startup (run.ps1 style): no parameters -> prompt for the
# choices a run needs; ANY parameter -> scripted mode, no prompts ---------------
$interactive = ($PSBoundParameters.Count -eq 0)
$nCores = [Environment]::ProcessorCount
if ($interactive) {
    Write-Host ""
    Write-Host "push460: from-scratch push to $Target/480 (solver + plateau-merge + CP-SAT sweep)"
    Write-Host "(Enter accepts the [default] on every prompt)"
    Write-Host ""
    # lineage: continue or archive-and-start-fresh
    $bf = Join-Path $OutDir "best_c$Clues.txt"
    if (Test-Path $bf) {
        $sc = ''
        try {
            $first = Get-Content $bf -TotalCount 1 -ErrorAction Stop
            if ($first -match 'score=(\d+)/480') { $sc = " at $($Matches[1])/480" }
        } catch {}
        Write-Host "$bf exists$sc."
        do { $sel = Read-Host "[C]ontinue that lineage or [A]rchive it and start truly fresh (c/a) [c]" } until ($sel -eq '' -or $sel -match '^[caCA]$')
        if ($sel -match '^[aA]$') { $Fresh = $true }
    } else {
        Write-Host "no $bf yet - the solver starts a fresh from-scratch lineage."
    }
    # thread budget (split 62/19/19 solver/merge/sweep below)
    $defBudget = [Math]::Max(4, $nCores - 2)
    do { $sel = Read-Host "Total threads for all three engines [$defBudget = logical cores - 2]" } until ($sel -eq '' -or $sel -match '^\d+$')
    if ($sel -ne '') { $TotalThreads = [int]$sel }
    # target score
    do { $sel = Read-Host "Stop at score [$Target]" } until ($sel -eq '' -or $sel -match '^\d+$')
    if ($sel -ne '') { $Target = [int]$sel }
    # wall-clock cap
    do { $sel = Read-Host "Minutes [run until Ctrl+C or target]" } until ($sel -eq '' -or $sel -match '^\d+(\.\d+)?$')
    if ($sel -ne '') { $Minutes = [double]$sel }
    # stagnation restart (solver only; merge/sweep keep their own state)
    do { $sel = Read-Host "Restart the solver after hours without a new best (0 = never) [$StagnationHours]" } until ($sel -eq '' -or $sel -match '^\d+(\.\d+)?$')
    if ($sel -ne '') { $StagnationHours = [double]$sel }
    # basin exhaustion (no new best for hours AND the sweep queue drained)
    do { $sel = Read-Host "Declare the basin exhausted after hours without a new best, once sweep has nothing left (0 = never) [$ExhaustedHours]" } until ($sel -eq '' -or $sel -match '^\d+(\.\d+)?$')
    if ($sel -ne '') { $ExhaustedHours = [double]$sel }
    if ($ExhaustedHours -gt 0) {
        do { $sel = Read-Host "On exhaustion: [N]otify (beep + banner, keep running) or [R]estart a fresh basin automatically (n/r) [n]" } until ($sel -eq '' -or $sel -match '^[nrNR]$')
        if ($sel -match '^[rR]$') { $OnExhausted = 'restart' }
    }
}

# --- thread auto-split (see header) -------------------------------------------
$threadBudget = [Math]::Max(4, $nCores - 2)
if ($TotalThreads -gt 0) { $threadBudget = $TotalThreads }
if ($SolverThreads -le 0) { $SolverThreads = [Math]::Max(2, [int][Math]::Floor($threadBudget * 0.625)) }
if ($MergeWorkers  -le 0) { $MergeWorkers  = [Math]::Max(2, [int][Math]::Floor($threadBudget * 0.1875)) }
if ($SweepWorkers  -le 0) { $SweepWorkers  = [Math]::Max(2, [int][Math]::Floor($threadBudget * 0.1875)) }

# --- paths -------------------------------------------------------------------
New-Item -ItemType Directory -Force $OutDir | Out-Null
$OutDir   = (Resolve-Path $OutDir).Path
$DriftDir = Join-Path $OutDir 'drift'
$SweepDir = Join-Path $OutDir 'sweep'      # sweep targets + their state files
$LogDir   = Join-Path $OutDir 'push460'    # child stdout/stderr logs
foreach ($d in @($DriftDir, $SweepDir, $LogDir)) { New-Item -ItemType Directory -Force $d | Out-Null }
$BestFile = Join-Path $OutDir "best_c$Clues.txt"
$RunsDir  = Join-Path $PSScriptRoot 'runs'
New-Item -ItemType Directory -Force $RunsDir | Out-Null

function Log([string]$msg) {
    Write-Host ("[push460 {0}] {1}" -f (Get-Date -Format HH:mm:ss), $msg)
}

# --- fresh start: archive the whole lineage (same idea as run.ps1, plus the
# sweep dir -- stale targets/candidates from the old basin would otherwise be
# adopted right back over the fresh one). Also called by the exhaustion
# auto-restart (-OnExhausted restart) mid-run. ----------------------------------
function Archive-Lineage {
    $ts = Get-Date -Format yyyyMMdd_HHmmss
    foreach ($f in @("best_c$Clues.txt", "history_c$Clues.log")) {
        $p = Join-Path $OutDir $f
        if (Test-Path $p) { Move-Item $p "$p.$ts.bak"; Log "archived $p" }
    }
    foreach ($d in @($DriftDir, $SweepDir)) {
        if ((Test-Path $d) -and (Get-ChildItem $d -ErrorAction SilentlyContinue)) {
            Move-Item $d "$d.$ts.bak"; New-Item -ItemType Directory -Force $d | Out-Null
            Log "archived $d"
        }
    }
}
if ($Fresh) { Archive-Lineage }
# candidates in runs\ older than this are ignored (only matters after -Fresh:
# they belong to the archived basin and would out-score the new one instantly)
if ($Fresh) { $CandidateCutoff = Get-Date } else { $CandidateCutoff = [datetime]::MinValue }

# --- solver exe (same preference order as run.ps1: native solver.exe -> build
# it -> committed portable bin\solver.exe). Interactive runs get a choice when
# there is no native exe yet ----------------------------------------------------
$exe = Join-Path $PSScriptRoot 'solver.exe'
if (-not (Test-Path $exe)) {
    $portable = Join-Path $PSScriptRoot 'bin\solver.exe'
    $tryBuild = $true
    if ($interactive -and (Test-Path $portable)) {
        Write-Host "no native solver.exe on this machine."
        do { $sel = Read-Host "[B]uild it with g++ (machine-tuned, needs a toolchain) or use the committed [P]ortable bin\solver.exe (b/p) [b]" } until ($sel -eq '' -or $sel -match '^[bpBP]$')
        $tryBuild = ($sel -notmatch '^[pP]$')
    }
    if ($tryBuild) {
        Log "trying a native build via scripts\build.ps1 ..."
        try { & "$PSScriptRoot\scripts\build.ps1" } catch { Log "build unavailable: $_" }
    }
    if (-not (Test-Path $exe)) {
        $exe = $portable
        if (-not (Test-Path $exe)) { throw "no solver exe: install g++ and run scripts\build.ps1, or restore bin\solver.exe from the repo" }
        Log "using portable bin\solver.exe (x86-64-v3; needs an AVX2 CPU, ~2015+)"
    }
}

# --- python + ortools (needed by MERGE and SWEEP). Windows exposes the
# interpreter as `python` and/or the `py` launcher -- sometimes ONLY `py`
# (python.org installer without the add-to-PATH option), so probe both and
# remember which one worked ($Python is what Start-Merge/Start-Sweep launch).
# Probes go through cmd /c so a failed import stays silent (PS 5.1 wraps native
# stderr in ErrorRecords, which $ErrorActionPreference=Stop would escalate);
# the Microsoft Store `python` alias stub fails both probes and falls through. --
$Python = $null
$UseMerge = -not $NoMerge
$UseSweep = -not $NoSweep
if ($UseMerge -or $UseSweep) {
    $pyProblem = ''
    foreach ($cand in @('python', 'py')) {
        if (-not (Get-Command $cand -ErrorAction SilentlyContinue)) { continue }
        cmd /c "$cand -c `"import ortools`" >nul 2>&1"
        if ($LASTEXITCODE -eq 0) { $Python = $cand; break }
        # interpreter runs but ortools is missing?
        cmd /c "$cand -c `"import sys`" >nul 2>&1"
        if ($LASTEXITCODE -eq 0 -and -not $pyProblem) { $pyProblem = "$cand works but ortools is missing ($cand -m pip install ortools)" }
    }
    if (-not $Python) {
        if (-not $pyProblem) { $pyProblem = "no python found on PATH (tried 'python' and 'py'; install Python 3, then: py -m pip install ortools)" }
        if ($interactive) {
            Write-Host "$pyProblem - the merge/sweep engines need it."
            do { $sel = Read-Host "Run [S]olver-only without them, or [Q]uit to install (s/q) [s]" } until ($sel -eq '' -or $sel -match '^[sqSQ]$')
            if ($sel -match '^[qQ]$') { exit 1 }
            $UseMerge = $false; $UseSweep = $false
            Log "merge/sweep disabled for this run (solver-only)"
        } else {
            throw "$pyProblem - needed for merge/sweep; or pass -NoMerge -NoSweep."
        }
    }
}

# --- helpers -------------------------------------------------------------------
function Quote-Args([string[]]$a) {
    ($a | ForEach-Object { if ($_ -match '\s') { '"' + $_ + '"' } else { $_ } }) -join ' '
}

function Get-BestScore {
    # try/catch: the solver may be mid-rewrite of the best file; a transient
    # sharing violation just means "ask again next poll"
    if (-not (Test-Path $BestFile)) { return -1 }
    try { $first = Get-Content $BestFile -TotalCount 1 -ErrorAction Stop } catch { return -1 }
    if ($first -match 'score=(\d+)/480') { return [int]$Matches[1] }
    return -1
}

function Stop-Child($proc, [string]$name) {
    if ($proc -and -not $proc.HasExited) {
        try { Stop-Process -Id $proc.Id -Force -ErrorAction Stop } catch {}
        Log "stopped $name (pid $($proc.Id))"
    }
}

# --- child: SOLVER --------------------------------------------------------------
$script:SolverProc = $null
$script:SolverLog = $null
$script:SolverStarted = Get-Date
function Start-Solver([string]$seedFile) {
    $a = @('--clues', "$Clues", '--out', $OutDir,
           '--drift-dir', $DriftDir, '--drift-cap', "$DriftCap") + $ScratchArgs
    if ($SolverThreads -gt 0) { $a += @('--threads', "$SolverThreads") }
    if ($seedFile) { $a += @('--seed-edges', $seedFile) }
    $script:SolverLog = Join-Path $LogDir ("solver_{0}.log" -f (Get-Date -Format yyyyMMdd_HHmmss))
    $line = Quote-Args $a
    Log "solver: $exe $line"
    Log "solver log: $($script:SolverLog)"
    $script:SolverProc = Start-Process -FilePath $exe -ArgumentList $line `
        -WorkingDirectory $PSScriptRoot -NoNewWindow -PassThru `
        -RedirectStandardOutput $script:SolverLog -RedirectStandardError "$($script:SolverLog).err"
    $script:SolverStarted = Get-Date
}

# --- child: MERGE loop -----------------------------------------------------------
$script:MergeProc = $null
function Start-Merge {
    $a = @((Join-Path $PSScriptRoot 'tools\plateau_merge.py'),
           '--dir', $DriftDir, '--clues', "$Clues", '--include', $BestFile,
           '--loop', '--time', '900', '--time-per-pair', '60',
           '--workers', "$MergeWorkers", '--max-window', '40', '--max-boards', '500')
    $log = Join-Path $LogDir ("merge_{0}.log" -f (Get-Date -Format yyyyMMdd_HHmmss))
    Log "merge loop: $Python $(Quote-Args $a)"
    $script:MergeProc = Start-Process -FilePath $Python -ArgumentList (Quote-Args $a) `
        -WorkingDirectory $PSScriptRoot -NoNewWindow -PassThru `
        -RedirectStandardOutput $log -RedirectStandardError "$log.err"
}

# --- SWEEP management -------------------------------------------------------------
# Targets are immutable copies in $SweepDir named target_<score>_<hash8>.txt.
# Per-target markers: .pass1.done / .pass2.done (retired when both exist),
# window state lives in window_sweep's own .sweep_c*.state next to the copy.
$script:SweepProc = $null
$script:SweepTargetFile = $null
$script:SweepPass = 0
$script:SweepLog = $null

function Sync-SweepTargets {
    # candidate_merge boards (from the MERGE loop, or older sessions)
    Get-ChildItem $RunsDir -Filter 'candidate_merge_*.txt' -ErrorAction SilentlyContinue |
        Where-Object { $_.LastWriteTime -gt $CandidateCutoff } | ForEach-Object {
        if ($_.Name -match '^candidate_merge_(\d+)_([0-9a-f]+)\.txt$') {
            $t = Join-Path $SweepDir ("target_{0}_{1}.txt" -f $Matches[1], $Matches[2])
            if (-not (Test-Path $t)) { Copy-Item $_.FullName $t; Log "new sweep target: $(Split-Path $t -Leaf)" }
        }
    }
    # the current best board itself (try/catch: solver may be mid-rewrite;
    # skipped copies are retried on the next poll)
    $score = Get-BestScore
    if ($score -ge 0) {
        try {
            $h = (Get-FileHash $BestFile -Algorithm MD5 -ErrorAction Stop).Hash.Substring(0, 8).ToLower()
            $t = Join-Path $SweepDir ("target_{0}_{1}.txt" -f $score, $h)
            if (-not (Test-Path $t)) {
                Copy-Item $BestFile $t -ErrorAction Stop
                Log "new sweep target: $(Split-Path $t -Leaf)"
            }
        } catch {}
    }
}

function Pick-SweepTarget([int]$curBest) {
    $list = @()
    Get-ChildItem $SweepDir -Filter 'target_*.txt' -ErrorAction SilentlyContinue |
        Where-Object { $_.Name -match '^target_(\d+)_[0-9a-f]+\.txt$' } | ForEach-Object {
        $null = $_.Name -match '^target_(\d+)_'
        $score = [int]$Matches[1]
        if ($score -lt $curBest - 1) { return }   # stale plateau, skip
        $p1 = Test-Path "$($_.FullName).pass1.done"
        $p2 = Test-Path "$($_.FullName).pass2.done"
        if ($p1 -and $p2) { return }              # retired
        if ($p1) { $pass = 2 } else { $pass = 1 }
        $list += [pscustomobject]@{ File = $_.FullName; Score = $score; Pass = $pass; MTime = $_.LastWriteTime }
    }
    if ($list.Count -eq 0) { return $null }
    $list | Sort-Object Pass, @{Expression = 'Score'; Descending = $true}, MTime | Select-Object -First 1
}

function Start-Sweep($tgt) {
    $tp = $SweepTimePer
    $extra = @()
    if ($tgt.Pass -eq 2) {
        $tp = [math]::Max($SweepTimePer, 600)
        $extra = @('--retry-unknown')
    }
    $a = @((Join-Path $PSScriptRoot 'tools\window_sweep.py'),
           '--board', $tgt.File, '--clues', "$Clues",
           '--time-per', "$tp", '--minutes', "$SweepChunkMinutes",
           '--workers', "$SweepWorkers", '--out', "$($tgt.File).improved.txt") + $extra
    $script:SweepLog = Join-Path $LogDir ("sweep_{0}.log" -f (Get-Date -Format yyyyMMdd_HHmmss))
    $script:SweepTargetFile = $tgt.File
    $script:SweepPass = $tgt.Pass
    Log ("sweep: {0} pass {1} ({2}-min chunk, {3} s/window)" -f (Split-Path $tgt.File -Leaf), $tgt.Pass, $SweepChunkMinutes, $tp)
    $script:SweepProc = Start-Process -FilePath $Python -ArgumentList (Quote-Args $a) `
        -WorkingDirectory $PSScriptRoot -NoNewWindow -PassThru `
        -RedirectStandardOutput $script:SweepLog -RedirectStandardError "$($script:SweepLog).err"
}

function Finish-Sweep {
    # called after the sweep child exits: classify the outcome
    $t = $script:SweepTargetFile
    $script:SweepProc = $null
    if (-not $t) { return }
    if (Test-Path "$t.improved.txt") {
        # retire the target; the improvement is adopted by the poll loop and
        # the improved board becomes its own (better) target
        New-Item -ItemType File -Force "$t.pass1.done" | Out-Null
        New-Item -ItemType File -Force "$t.pass2.done" | Out-Null
        Log "sweep IMPROVEMENT on $(Split-Path $t -Leaf) - see $t.improved.txt"
        return
    }
    $tail = @()
    if ($script:SweepLog -and (Test-Path $script:SweepLog)) { $tail = Get-Content $script:SweepLog -Tail 3 }
    # NB: a budget-exhausted chunk prints BOTH "time budget exhausted" AND the
    # final "done:" summary -- check the budget marker first, else a pass would
    # be retired after its first chunk
    if ($tail -match 'time budget exhausted') {
        # chunk over, state saved; the target stays queued and resumes later
    } elseif ($tail -match '\[sweep\] done:') {
        New-Item -ItemType File -Force ("$t.pass{0}.done" -f $script:SweepPass) | Out-Null
        Log ("sweep pass {0} finished on {1}: no improvement (windows proven/undecided; see log)" -f $script:SweepPass, (Split-Path $t -Leaf))
    } else {
        Log "sweep child ended unexpectedly; last lines of $($script:SweepLog):"
        $tail | ForEach-Object { Write-Host "    $_" }
    }
}

# --- ADOPTION: merge/sweep candidates better than the current best ---------------
$script:AdoptTried = @{}
function Get-BestCandidate([int]$curBest) {
    $cands = @()
    Get-ChildItem $RunsDir -Filter 'candidate_merge_*.txt' -ErrorAction SilentlyContinue |
        Where-Object { $_.LastWriteTime -gt $CandidateCutoff } | ForEach-Object {
        if ($_.Name -match '^candidate_merge_(\d+)_') {
            $cands += [pscustomobject]@{ File = $_.FullName; Score = [int]$Matches[1]; MTime = $_.LastWriteTime }
        }
    }
    Get-ChildItem $SweepDir -Filter '*.improved.txt' -ErrorAction SilentlyContinue | ForEach-Object {
        try { $head = Get-Content $_.FullName -TotalCount 1 -ErrorAction Stop } catch { $head = '' }
        if ($head -match '->\s*(\d+)') {
            $cands += [pscustomobject]@{ File = $_.FullName; Score = [int]$Matches[1]; MTime = $_.LastWriteTime }
        }
    }
    $cands = $cands | Where-Object {
        $_.Score -gt $curBest -and (-not $script:AdoptTried.ContainsKey($_.File) -or $script:AdoptTried[$_.File] -lt 2)
    }
    if (-not $cands) { return $null }
    $cands | Sort-Object @{Expression = 'Score'; Descending = $true}, @{Expression = 'MTime'; Descending = $true} | Select-Object -First 1
}

# --- main ------------------------------------------------------------------------
$startScore = Get-BestScore
if ($startScore -ge 0) { Log "resuming lineage: best $startScore/480 ($BestFile)" }
else { Log "no best file yet - the solver starts a fresh from-scratch lineage" }
Log "target: $Target/480 | drift cap $DriftCap/thread | stagnation restart after $StagnationHours h"
if ($ExhaustedHours -gt 0) { Log "exhaustion: after $ExhaustedHours h without a new best + drained sweep queue -> $OnExhausted" }
Log "threads (budget $threadBudget, $nCores logical cores): solver $SolverThreads | merge $MergeWorkers | sweep $SweepWorkers"

$deadline = $null
if ($Minutes -gt 0) { $deadline = (Get-Date).AddMinutes($Minutes) }

try {
    Start-Solver $null
    if ($UseMerge) { Start-Merge }
    $lastStatus = Get-Date
    $lastBest = $startScore
    # exhaustion clock: last time the lineage's best actually improved (survives
    # solver restarts, unlike $SolverStarted which the stagnation check uses)
    $exhaustBest = $startScore
    $lastBestChange = Get-Date
    $lastExhaustNotify = [datetime]::MinValue

    while ($true) {
        Start-Sleep -Seconds 30
        $cur = Get-BestScore
        if ($cur -gt $exhaustBest) { $exhaustBest = $cur; $lastBestChange = Get-Date }

        # -- victory --
        if ($cur -ge $Target) {
            Log ("TARGET REACHED: {0}/480 >= {1}" -f $cur, $Target)
            $url = (Select-String -Path $BestFile -Pattern 'bucas' | Select-Object -First 1)
            if ($url) { Write-Host $url.Line }
            break
        }
        # -- overall time cap --
        if ($deadline -and (Get-Date) -gt $deadline) {
            Log "time cap ($Minutes min) reached - stopping (best $cur/480)"
            break
        }
        # -- solver died on its own? (crash or --minutes inside args) --
        if ($script:SolverProc.HasExited) {
            $err = "$($script:SolverLog).err"
            if ((Test-Path $err) -and (Get-Item $err).Length -gt 0) {
                Log "solver exited; stderr:"
                Get-Content $err -Tail 5 | ForEach-Object { Write-Host "    $_" }
            }
            Log "restarting solver"
            Start-Solver $null
        }
        # -- merge loop died? --
        if ($UseMerge -and $script:MergeProc -and $script:MergeProc.HasExited) {
            Log "merge loop exited - restarting it"
            Start-Merge
        }
        # -- adoption: feed better candidates back into the solver --
        $cand = Get-BestCandidate $cur
        # -- exhaustion test: lineage stagnant for -ExhaustedHours AND the sweep
        # queue drained (no chunk running, no target left at >= best-1; any new
        # merge candidate becomes a target and un-drains it, so this really
        # means merge+sweep have nothing left on this basin) --
        $exhausted = $false
        if (-not $cand -and $ExhaustedHours -gt 0 -and
            ((Get-Date) - $lastBestChange).TotalHours -ge $ExhaustedHours) {
            $exhausted = $true
            if ($UseSweep) {
                if ($script:SweepProc -and -not $script:SweepProc.HasExited) { $exhausted = $false }
                else { Sync-SweepTargets; if (Pick-SweepTarget $cur) { $exhausted = $false } }
            }
        }
        if ($cand) {
            if ($script:AdoptTried.ContainsKey($cand.File)) { $script:AdoptTried[$cand.File]++ }
            else { $script:AdoptTried[$cand.File] = 1 }
            Log ("ADOPTING {0} ({1} > {2}) - restarting solver seeded from it" -f (Split-Path $cand.File -Leaf), $cand.Score, $cur)
            Stop-Child $script:SolverProc 'solver'
            Start-Solver $cand.File
        }
        elseif ($exhausted -and $OnExhausted -eq 'restart') {
            Log ("BASIN EXHAUSTED at {0}/480: no new best for {1} h and the sweep queue is drained - archiving the lineage and starting a fresh basin" -f $cur, $ExhaustedHours)
            # children first: they hold open handles inside drift/ (Move-Item on
            # a dir with open handles fails on Windows)
            Stop-Child $script:SolverProc 'solver'
            Stop-Child $script:MergeProc 'merge'
            $script:SweepProc = $null; $script:SweepTargetFile = $null
            Archive-Lineage
            $CandidateCutoff = Get-Date   # runs\candidate_merge_* from the old basin must not be re-adopted
            $script:AdoptTried = @{}
            $exhaustBest = -1; $lastBestChange = Get-Date; $lastBest = -1
            Start-Solver $null
            if ($UseMerge) { Start-Merge }
            continue   # $cur is stale (pre-archive); re-read on the next poll
        }
        # -- stagnation: restart the solver on the same best (pools reset).
        # Runs even while an exhaustion notify is pending, so the solver keeps
        # getting pool resets until the user acts on the banner --
        elseif ($StagnationHours -gt 0) {
            $last = $script:SolverStarted
            if (Test-Path $BestFile) {
                $mt = (Get-Item $BestFile).LastWriteTime
                if ($mt -gt $last) { $last = $mt }
            }
            if (((Get-Date) - $last).TotalHours -ge $StagnationHours) {
                Log ("no new best for {0} h - restarting solver (pools reset, same lineage)" -f $StagnationHours)
                Stop-Child $script:SolverProc 'solver'
                Start-Solver $null
            }
        }
        # -- exhaustion notify (independent of the restart chain above) --
        if ($exhausted -and $OnExhausted -eq 'notify') {
            if (((Get-Date) - $lastExhaustNotify).TotalMinutes -ge 60) {
                $lastExhaustNotify = Get-Date
                Write-Host ""
                Write-Host ("=" * 78) -ForegroundColor Yellow
                Write-Host ("  BASIN EXHAUSTED at {0}/480: no new best for {1}+ h and the CP-SAT sweep" -f $cur, $ExhaustedHours) -ForegroundColor Yellow
                Write-Host "  queue is drained - merge and sweep have nothing left to try here." -ForegroundColor Yellow
                Write-Host "  Recommended: Ctrl+C, then restart with -Fresh (or answer [A]rchive) to" -ForegroundColor Yellow
                Write-Host "  hunt a new basin. Or rerun with -OnExhausted restart to automate this." -ForegroundColor Yellow
                Write-Host ("=" * 78) -ForegroundColor Yellow
                Write-Host ""
                try { 1..3 | ForEach-Object { [console]::Beep(880, 300); Start-Sleep -Milliseconds 150 } } catch {}
            }
        }
        # -- sweep rotation --
        if ($UseSweep) {
            if ($script:SweepProc -and $script:SweepProc.HasExited) { Finish-Sweep }
            if (-not $script:SweepProc -and $cur -ge 0) {
                Sync-SweepTargets
                # NB: not named $target -- that would collide (case-insensitively)
                # with the [int]$Target script parameter and force an int cast
                $pick = Pick-SweepTarget $cur
                if ($pick) { Start-Sweep $pick }
            }
        }
        # -- status line (once a minute) --
        if (((Get-Date) - $lastStatus).TotalSeconds -ge 60) {
            $lastStatus = Get-Date
            if ($cur -ne $lastBest) {
                Log ("NEW BEST {0}/480 (was {1})" -f $cur, $lastBest)
                $lastBest = $cur
            }
            $sw = 'idle'
            if ($script:SweepProc -and -not $script:SweepProc.HasExited) {
                $sw = "{0} p{1}" -f (Split-Path $script:SweepTargetFile -Leaf), $script:SweepPass
            } elseif (-not $UseSweep) { $sw = 'off' }
            $mg = 'off'
            if ($UseMerge) { if ($script:MergeProc.HasExited) { $mg = 'DEAD' } else { $mg = 'ok' } }
            $solverLine = ''
            if ($script:SolverLog -and (Test-Path $script:SolverLog)) {
                try {
                    $sl = Get-Content $script:SolverLog -Tail 1 -ErrorAction Stop
                    if ($sl) { $solverLine = ' | ' + ($sl -replace '\s+', ' ').Trim() }
                } catch {}
            }
            Log ("best={0}/480 target={1} | merge={2} | sweep={3}{4}" -f $cur, $Target, $mg, $sw, $solverLine)
        }
    }
} finally {
    Log "shutting down children..."
    Stop-Child $script:SolverProc 'solver'
    Stop-Child $script:MergeProc 'merge'
    Stop-Child $script:SweepProc 'sweep'
    Log ("final best: {0}/480 (board safe in {1})" -f (Get-BestScore), $BestFile)
}
