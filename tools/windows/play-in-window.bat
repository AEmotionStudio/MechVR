@echo off
rem Play MechVR in a window with mouse and keyboard. No headset needed.
cd /d "%~dp0"
mechvr.exe --flat %*
if errorlevel 1 pause
