@echo off
rem Double-clickable / execution-policy-proof shim for push460.ps1.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0push460.ps1" %*
