@echo off
rem FNC Reborn - builds the game from your own Xbox 360 disc image.
rem See README.md for requirements.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0scripts\setup.ps1" %*
