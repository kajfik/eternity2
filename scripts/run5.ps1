# Long hunt, 5-clue variant (record to beat: 464/480 -> need >=465).
# Resumes automatically from runs\best_c5.txt. Ctrl+C to stop; progress is
# saved on every new best, so stopping loses nothing.
# --order model: learned candidate ordering (SOLVER_EVAL Track A; +5.5-7
# avgdepth, 8/8 paired wins, zero runtime cost).
param([double]$Minutes = 0)  # 0 = run until stopped
Set-Location (Split-Path $PSScriptRoot -Parent)
$solverArgs = @('--clues','5','--order','model','--model-file','eval\model_weights.txt')
if ($Minutes -gt 0) { $solverArgs += @('--minutes',"$Minutes") }
.\solver.exe @solverArgs
