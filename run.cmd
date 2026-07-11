@echo off
rem Double-clickable / execution-policy-proof shim for run.ps1.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0run.ps1" %*
