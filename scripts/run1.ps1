# Long hunt, 1-clue variant (record to beat: 470/480 -> need >=471).
# Resumes automatically from runs\best_c1.txt. Ctrl+C to stop; progress is
# saved on every new best, so stopping loses nothing.
# --order model: learned candidate ordering (SOLVER_EVAL Track A; +5.5-7
# avgdepth, 8/8 paired wins, zero runtime cost).
param([double]$Minutes = 0)  # 0 = run until stopped
Set-Location (Split-Path $PSScriptRoot -Parent)
$solverArgs = @('--clues','1','--order','model','--model-file','eval\model_weights.txt')
if ($Minutes -gt 0) { $solverArgs += @('--minutes',"$Minutes") }
.\solver.exe @solverArgs
