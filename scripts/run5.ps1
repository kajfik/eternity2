# Long hunt, 5-clue variant (record to beat: 461/480).
# Resumes automatically from runs\best_c5.txt. Ctrl+C to stop; progress is
# saved on every new best, so stopping loses nothing.
param([double]$Minutes = 0)  # 0 = run until stopped
Set-Location (Split-Path $PSScriptRoot -Parent)
if ($Minutes -gt 0) { .\solver.exe --clues 5 --minutes $Minutes }
else { .\solver.exe --clues 5 }
