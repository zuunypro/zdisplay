@echo off
setlocal enabledelayedexpansion
rem =====================================================================
rem  Zdisplay - builds the installer (zdisplay-setup.exe).
rem
rem  Takes the compiled zdisplay.exe, compresses it with installer\pack.exe,
rem  embeds it as a resource and builds the installer UI around it.
rem
rem  Usage:  make-setup.bat          build the installer
rem          make-setup.bat clean    remove installer\build
rem
rem  Build flags come from ..\flags.mk, the same file the Makefile and build.bat
rem  read. Never duplicate a flag here: a single source is what keeps the build
rem  paths from drifting apart.
rem =====================================================================

cd /d "%~dp0"

if /i "%~1"=="clean" (
    if exist build rmdir /s /q build
    if exist ..\zdisplay-setup.exe del /q ..\zdisplay-setup.exe
    echo Limpo.
    goto :eof
)

rem ------------------------------------------------------------ flags from flags.mk
if not exist ..\flags.mk (
    echo flags.mk nao encontrado.
    goto :fail
)
for /f "usebackq eol=# tokens=1,* delims==" %%A in ("..\flags.mk") do (
    if /i "%%A"=="ZDISPLAY_STD"   set "F_STD=%%B"
    if /i "%%A"=="ZDISPLAY_WARN"  set "F_WARN=%%B"
    if /i "%%A"=="ZDISPLAY_REL"   set "F_REL=%%B"
    if /i "%%A"=="ZDISPLAY_LD"    set "F_LD=%%B"
    if /i "%%A"=="ZDISPLAY_LDREL" set "F_LDREL=%%B"
)

rem The library set differs from the application's: the installer talks to no
rem monitor, but needs COM for the .lnk shortcut and the folder dialog.
set "SETUP_LIBS=-lgdi32 -luser32 -ladvapi32 -lshell32 -lole32 -loleaut32 -lbcrypt -luuid"

rem ------------------------------------------------------------------- compiler
set "CXX="
where g++ >nul 2>&1 && set "CXX=g++"

if not defined CXX if exist "..\tools\w64devkit\bin\g++.exe" (
    set "PATH=%CD%\..\tools\w64devkit\bin;%PATH%"
    set "CXX=g++"
)

if not defined CXX (
    where clang++ >nul 2>&1 && set "CXX=clang++"
)

if not defined CXX (
    echo.
    echo Nenhum compilador C++ encontrado. Rode  build.bat  na raiz uma vez: ele
    echo sabe baixar o toolchain portatil ^(e confere hash e assinatura^).
    goto :fail
)

echo Compilador: %CXX%

rem ---------------------------------------------------- application to embed
set "APP="
if exist "..\build\release\zdisplay.exe" set "APP=..\build\release\zdisplay.exe"
if not defined APP if exist "..\zdisplay.exe" set "APP=..\zdisplay.exe"

if not defined APP (
    echo Nao achei o zdisplay.exe; compilando o programa primeiro...
    call ..\build.bat
    if errorlevel 1 goto :fail
    if exist "..\build\release\zdisplay.exe" set "APP=..\build\release\zdisplay.exe"
    if not defined APP if exist "..\zdisplay.exe" set "APP=..\zdisplay.exe"
)

if not defined APP (
    echo Nao consegui obter o zdisplay.exe para embutir.
    goto :fail
)
echo Programa: %APP%

if not exist build mkdir build

rem ------------------------------------------------------------------- packer
echo Compilando o empacotador...
%CXX% %F_STD% %F_WARN% -O2 pack.cpp -o build\pack.exe -static -lbcrypt
if errorlevel 1 goto :fail

echo Compactando o payload...
build\pack.exe "%APP%" build\payload.bin
if errorlevel 1 goto :fail

rem ----------------------------------------------------------------- resources
rem No windres means no resource, and no resource means no payload. An installer
rem with nothing inside it is useless, so this is a hard error here, unlike in
rem build.bat where the only loss is the icon.
where windres >nul 2>&1
if errorlevel 1 (
    echo windres nao encontrado; sem ele o instalador sai sem o programa dentro.
    goto :fail
)

set "RESFLAGS=-DZDISPLAY_HAS_PAYLOAD"
if exist ..\assets\zdisplay.ico set "RESFLAGS=!RESFLAGS! -DZDISPLAY_HAS_ICON"

echo Compilando recursos...
windres !RESFLAGS! -i setup.rc -o build\setup_res.o
if errorlevel 1 goto :fail

rem ----------------------------------------------------------------- installer
echo Compilando o instalador...
%CXX% %F_STD% %F_WARN% %F_REL% setup_ui.cpp setup_work.cpp setup_text.cpp build\setup_res.o -o build\zdisplay-setup.exe %F_LD% %F_LDREL% %SETUP_LIBS%
if errorlevel 1 goto :fail

echo Verificando o instalador...
build\zdisplay-setup.exe /verify
if errorlevel 1 goto :fail
call :verify_pe_hardening "build\zdisplay-setup.exe"
if errorlevel 1 goto :fail

copy /y build\zdisplay-setup.exe ..\zdisplay-setup.exe >nul
if errorlevel 1 (
    echo.
    echo AVISO: nao consegui copiar para a raiz ^(o instalador esta aberto?^).
    echo O binario novo esta em installer\build\zdisplay-setup.exe
    goto :eof
)

echo.
for %%F in (..\zdisplay-setup.exe) do echo Pronto: zdisplay-setup.exe  ^(%%~zF bytes^)
goto :eof

:verify_pe_hardening
    where objdump >nul 2>&1
    if errorlevel 1 (
        echo objdump nao encontrado; nao da para conferir DEP/ASLR do instalador.
        exit /b 1
    )
    objdump -x "%~1" | findstr /c:"DYNAMIC_BASE" >nul
    if errorlevel 1 exit /b 1
    objdump -x "%~1" | findstr /c:"HIGH_ENTROPY_VA" >nul
    if errorlevel 1 exit /b 1
    objdump -x "%~1" | findstr /c:"NX_COMPAT" >nul
    if errorlevel 1 exit /b 1
    exit /b 0

:fail
echo.
echo *** Nao consegui montar o instalador. ***
exit /b 1
