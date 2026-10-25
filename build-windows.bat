@echo off
rem Builds parasite-eve.exe on Windows through WSL (Ubuntu).
rem Double-click it, or drop your Disc 1 .cue (and Disc 2 .cue) onto it. The
rem exe and a settings.ini naming both discs are written next to this file. Needs WSL with Ubuntu ("wsl --install" once), and Git
rem and Python on Windows (offered for install with winget if missing):
rem downloads are made on Windows into the downloads folder next to this
rem file, as WSL networking can be much slower.
rem Set PE_WSL_DISTRO to use a distro other than the default one.
rem Paths are only echoed in quotes: folder names may contain & or ( ).
setlocal
set "HERE=%~dp0"
set "HERE=%HERE:~0,-1%"
set "DL=%HERE%\downloads"
set "WSL=wsl"
if not "%PE_WSL_DISTRO%"=="" set "WSL=wsl -d %PE_WSL_DISTRO%"

where wsl >nul 2>nul || (
    echo WSL is not installed. Open PowerShell as administrator, run
    echo   wsl --install
    echo restart Windows, open Ubuntu once to create your user, then run this again.
    pause & exit /b 1
)
%WSL% -e true <nul >nul 2>nul || (
    echo WSL has no Linux distribution yet. Open PowerShell as administrator, run
    echo   wsl --install -d Ubuntu
    echo then open Ubuntu once to create your user, and run this again.
    pause & exit /b 1
)
rem Git and Python make the downloads. A missing one is offered for install
rem with winget, Windows' package manager.
call :find_git
if defined HAVE_GIT goto git_ok
set "WINGET_EXTRA="
call :install Git Git.Git && call :find_git
if defined HAVE_GIT goto git_ok
echo Git for Windows is needed for the downloads. Install it from
echo   https://git-scm.com/download/win
echo then run this again.
pause & exit /b 1
:git_ok
call :find_python
if defined PY goto python_ok
rem Per-user install (no administrator), added to PATH.
set WINGET_EXTRA=--override "/quiet InstallAllUsers=0 PrependPath=1 Include_launcher=1 Include_pip=1"
call :install Python Python.Python.3.12 && call :find_python
if defined PY goto python_ok
echo Python 3.8 or newer is needed for the downloads. Install it from
echo   https://www.python.org/downloads/windows/
echo then run this again.
pause & exit /b 1
:python_ok

set "DISC=%~1"
if "%DISC%"=="" set /p "DISC=Path to your Parasite Eve Disc 1 .cue (you can drag the file here): "
set "DISC=%DISC:"=%"
if exist "%DISC%" goto disc_ok
echo Not found: "%DISC%"
pause & exit /b 1
:disc_ok

rem Disc 2 is only read while playing (the game's disc change), not for the
rem build. Second argument, else Disc 1's name with "Disc 2", else asked.
set "DISC2=%~2"
if not defined DISC2 goto disc2_guess
if "%DISC:Disc 2=%"=="%DISC%" goto disc2_check
rem Both dropped at once, Disc 2 first (Explorer picks the order).
set "DISC2=%DISC%"
set "DISC=%~2"
if exist "%DISC%" goto disc2_check
echo Not found: "%DISC%"
pause & exit /b 1
:disc2_guess
set "DISC2=%DISC:Disc 1=Disc 2%"
if /i "%DISC2%"=="%DISC%" goto ask_disc2
if exist "%DISC2%" goto disc2_found
:ask_disc2
set "DISC2="
set /p "DISC2=Path to your Parasite Eve Disc 2 .cue (drag the file here, or press Enter if you have none): "
if not defined DISC2 goto disc2_ok
:disc2_check
set "DISC2=%DISC2:"=%"
if exist "%DISC2%" goto disc2_found
echo Not found: "%DISC2%"
pause & exit /b 1
:disc2_found
echo Disc 2: "%DISC2%"
:disc2_ok

echo.
echo == Checking Ubuntu packages (as root inside WSL)
%WSL% --cd "%HERE%" -u root -e bash scripts/build-wsl.sh prepare "%DL%" || (
    echo Checking the Ubuntu packages failed.
    pause & exit /b 1
)

echo.
echo == Downloads (on Windows, into the downloads folder)
%PY% "%HERE%\scripts\fetch-windows.py" "%DL%" || (
    echo A download failed. Check your internet connection and run this again.
    pause & exit /b 1
)

echo.
echo == Ubuntu packages
%WSL% --cd "%HERE%" -u root -e bash scripts/build-wsl.sh packages "%DL%" || (
    echo Installing the Ubuntu packages failed.
    pause & exit /b 1
)

echo.
echo == Build (the first one takes a while; logs in ~/parasite-eve-build/build/logs in Ubuntu)
%WSL% --cd "%HERE%" -e bash scripts/build-wsl.sh build "%DISC%" "%HERE%" "%DL%" || (
    echo The build failed. See the messages above.
    pause & exit /b 1
)

rem The discs go into settings.ini next to the exe, so a double click finds them.
%PY% "%HERE%\scripts\write-settings.py" "%HERE%\settings.ini" "%DISC%" "%DISC2%" || (
    echo Writing settings.ini failed.
    pause & exit /b 1
)

echo.
echo Done: "%HERE%\parasite-eve.exe"
echo Your discs are saved in settings.ini next to it (disc= and disc2=).
echo Start parasite-eve.exe from this folder, or copy settings.ini along with it.
pause
exit /b 0

:find_git
set "HAVE_GIT="
where git >nul 2>nul && set "HAVE_GIT=1"
if defined HAVE_GIT exit /b 0
if not exist "%ProgramFiles%\Git\cmd\git.exe" exit /b 0
set "PATH=%ProgramFiles%\Git\cmd;%PATH%"
set "HAVE_GIT=1"
exit /b 0

:find_python
rem "python" can be the Microsoft Store placeholder, so check it really runs.
set "PYCHECK=import sys, pip; sys.exit(sys.version_info < (3, 8))"
set "PY="
python -c "%PYCHECK%" >nul 2>nul && set "PY=python"
if not defined PY py -3 -c "%PYCHECK%" >nul 2>nul && set "PY=py -3"
if defined PY exit /b 0
rem Just installed and not on this window's PATH yet: the usual folders.
for /d %%d in ("%LOCALAPPDATA%\Programs\Python\Python3*" "%ProgramFiles%\Python3*") do (
    "%%~d\python.exe" -c "%PYCHECK%" >nul 2>nul && set PY="%%~d\python.exe"
)
exit /b 0

rem :install <name> <winget id>, with WINGET_EXTRA added to the winget call.
rem Fails if winget is missing or the user says no.
:install
where winget >nul 2>nul || exit /b 1
echo.
echo %~1 is needed for the downloads and is not installed.
set "ANSWER="
set /p "ANSWER=Install %~1 now with winget? [Y/N] "
if /i not "%ANSWER:~0,1%"=="Y" exit /b 1
winget install --id %~2 -e --source winget --accept-package-agreements --accept-source-agreements %WINGET_EXTRA%
rem Take the PATH the installer saved, as this window still has the old one.
for /f "usebackq delims=" %%p in (`powershell -NoProfile -Command "[Environment]::GetEnvironmentVariable('Path','Machine') + ';' + [Environment]::GetEnvironmentVariable('Path','User')"`) do set "PATH=%%p;%PATH%"
exit /b 0
