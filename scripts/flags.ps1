# Canonical best-known solver flag sets. Dot-sourced by run.ps1 and
# push460.ps1 so every launcher uses the same proven configuration.
#
# ============================= MAINTENANCE NOTE ==============================
# This file is the SINGLE SOURCE OF TRUTH for the current best-known flag
# sets. Claude instances: whenever a new flag configuration is proven better
# (A/B tested, e2lib-verified), update these blocks and the matching note in
# CLAUDE.md as part of adopting it. Do not add unproven flags here.
# =============================================================================

# From-scratch, proven 2026-07-10/11 (CLAUDE.md "Verhaard adoptions" +
# theft-guard A/B): graded slip schedule + adaptive restart aborts lift the
# 90-s from-scratch best from 432 to 444-448. --scan-orient 1 (2026-07-11 A/B,
# +3.5 mean at 90 s, two all-time 451s) puts the center clue at scan row 7;
# proven on 5-clue hunts, mechanism-aligned but untested for clues=1.
$ScratchArgs = @(
    '--order','model','--model-file','eval\model_weights.txt',
    '--slip-target','460',
    '--abort-points','2000000:60,10000000:120',
    '--scan-orient','1'
)

# From-best (record hunts resume a high seed where hunter output is pool
# food): the long-run-proven set from scripts\run5.ps1 / run1.ps1. Slip is
# only smoke-tested from scratch, not on resumed record lineages.
$BestArgs = @(
    '--order','model','--model-file','eval\model_weights.txt'
)
