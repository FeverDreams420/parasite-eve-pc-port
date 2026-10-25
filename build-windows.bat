@echo off
rem Builds parasite-eve.exe on Windows through WSL (Ubuntu).
rem Double-click it, or drop your Disc 1 .cue onto it. The exe is written
rem next to this file. Needs WSL with Ubuntu ("wsl --install" once), and Git
rem and Python on Windows: downloads are made on Windows into the downloads
rem folder next to this file, as WSL networking can be much slower.
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
%WSL% -e true >nul 2>nul || (
    echo WSL has no Linux distribution yet. Open PowerShell as administrator, run
    echo   wsl --install -d Ubuntu
    echo then open Ubuntu once to create your user, and run this again.
    pause & exit /b 1
)
where git >nul 2>nul || (
    echo Git for Windows is needed for the downloads. Install it from
    echo   https://git-scm.com/download/win
    echo then run this again.
    pause & exit /b 1
)
rem "python" can be the Microsoft Store placeholder, so check it really runs.
set "PY="
python -c "import sys, pip; sys.exit(sys.version_info < (3, 8))" >nul 2>nul && set "PY=python"
if not defined PY py -3 -c "import sys, pip; sys.exit(sys.version_info < (3, 8))" >nul 2>nul && set "PY=py -3"
if not defined PY (
    echo Python 3.8 or newer is needed for the downloads. Install it from
    echo   https://www.python.org/downloads/windows/
    echo then run this again.
    pause & exit /b 1
)

set "DISC=%~1"
if "%DISC%"=="" set /p "DISC=Path to your Parasite Eve Disc 1 .cue (you can drag the file here): "
set "DISC=%DISC:"=%"
if exist "%DISC%" goto disc_ok
echo Not found: "%DISC%"
pause & exit /b 1
:disc_ok

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

echo.
echo Done: "%HERE%\parasite-eve.exe"
echo Put it in a folder with your Disc 1 .cue/.bin, or set disc= in settings.ini.
pause
