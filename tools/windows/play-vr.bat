@echo off
rem Play MechVR in the headset. Start SteamVR (or your OpenXR runtime) first.
cd /d "%~dp0"
mechvr.exe %*
if errorlevel 1 pause
