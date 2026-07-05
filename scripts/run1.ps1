# Long hunt, 1-clue variant (record to beat: 470/480).
# Resumes automatically from runs\best_c1.txt. Ctrl+C to stop; progress is
# saved on every new best, so stopping loses nothing.
param([double]$Minutes = 0)  # 0 = run until stopped
Set-Location (Split-Path $PSScriptRoot -Parent)
if ($Minutes -gt 0) { .\solver.exe --clues 1 --minutes $Minutes }
else { .\solver.exe --clues 1 }
