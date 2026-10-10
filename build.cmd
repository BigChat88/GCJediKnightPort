@echo off
rem  build.cmd -- one-click GameCube disc image build.
rem
rem  1. put your Jedi Knight and/or Mysteries of the Sith CD images in  gamedata\
rem     (see gamedata\README.md)
rem  2. run this file
rem  3. the images appear in  output\openjkdf2.iso  and  output\openjkm.iso
rem
rem  Extra options are passed straight through, e.g.:
rem     build.cmd --only jk
rem     build.cmd --gamedata "D:\Rips\Jedi Knight"
setlocal
cd /d "%~dp0"

set "PY=python"
where python >nul 2>&1 || set "PY=py -3"

%PY% -m pip install --quiet --disable-pip-version-check -r requirements.txt
%PY% tools\pack_iso.py %*
set "RC=%ERRORLEVEL%"

echo.
if not "%RC%"=="0" (
    echo Build FAILED ^(exit %RC%^).
) else (
    echo Done. Check output\ for your disc images.
)
pause
exit /b %RC%
