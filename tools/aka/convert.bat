@echo off
rem Convertit des ROMs pour la Gamebuino AKA via WSL (Windows).
rem Usage : convert.bat jeu.gg    ou glisser-deposer une ou plusieurs ROMs sur ce fichier.
rem Les .zip sortent dans le dossier "aka-out" a cote de la ROM.
setlocal
if "%~1"=="" ( echo Glisse une ROM sur ce fichier, ou : convert.bat jeu.gg & pause & exit /b 1 )
where wsl >nul 2>&1 || ( echo WSL est requis : wsl --install & pause & exit /b 1 )
set "ARGS="
for %%F in (%*) do call :add "%%~F"
for /f "delims=" %%S in ('wsl wslpath -a "%~dp0convert.sh"') do set "SH=%%S"
for /f "delims=" %%O in ('wsl wslpath -a "%~dp1aka-out"') do set "OUT=%%O"
wsl bash "%SH%" -o "%OUT%" %ARGS%
echo.
echo Sortie : %~dp1aka-out
pause
exit /b 0
:add
for /f "delims=" %%P in ('wsl wslpath -a "%~1"') do set "ARGS=%ARGS% '%%P'"
exit /b 0
