@echo off
rem ---------------------------------------------------------------------------
rem  Yunwei tunnel-config launcher (ivsbox 通道配置)
rem
rem  The real program is the .exe sitting next to this file: a compiled Win32
rem  binary, no PowerShell / python / .NET needed.
rem
rem  This batch file must stay ASCII-only. cmd.exe reads .bat/.cmd in the ANSI
rem  code page, so a Chinese literal saved as UTF-8 would be mis-decoded and the
rem  target "would not be found". The wildcard lookup below avoids naming the
rem  exe at all, so it works under any system locale.
rem ---------------------------------------------------------------------------
setlocal
set "EXE="
for %%f in ("%~dp0*.exe") do set "EXE=%%~ff"
if not defined EXE goto noexe
start "" "%EXE%"
exit /b 0

:noexe
echo [X] No .exe found next to this launcher.
echo     Build it first:  cd ivsbox\tools\tunnelcfg ^&^& sh build.sh
pause
exit /b 1
