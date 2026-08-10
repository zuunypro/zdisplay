@echo off
setlocal enabledelayedexpansion
rem =====================================================================
rem  Zdisplay - build script.
rem
rem  Locates a C++ compiler (MinGW-w64 / clang). If none is found, downloads
rem  w64devkit (portable, no installation, no elevation) into tools\.
rem
rem  Usage:  build.bat            release build, then run the test suite
rem          build.bat debug      build with symbols and no optimisation
rem          build.bat test       test suite only
rem          build.bat setup      build the installer (zdisplay-setup.exe)
rem          build.bat clean      remove the build directory
rem
rem  Flags come from flags.mk, the same file the Makefile includes. Never
rem  duplicate a flag here: a single source is what keeps the two build paths
rem  from drifting apart.
rem =====================================================================

cd /d "%~dp0"

if /i "%~1"=="clean" (
    if exist build rmdir /s /q build
    if exist zdisplay.exe del /q zdisplay.exe
    call installer\make-setup.bat clean
    echo Limpo.
    goto :eof
)

rem The installer has its own script: it needs a finished zdisplay.exe and a
rem packer that lives only there.
if /i "%~1"=="setup" (
    call installer\make-setup.bat
    exit /b %errorlevel%
)

set "ONLYTESTS="
if /i "%~1"=="test" set "ONLYTESTS=1"

set "CONFIG=release"
if /i "%~1"=="debug" set "CONFIG=debug"

rem ------------------------------------------------------------ flags from flags.mk
if not exist flags.mk (
    echo flags.mk nao encontrado. Ele e a fonte unica das flags de compilacao.
    goto :fail
)
for /f "usebackq eol=# tokens=1,* delims==" %%A in ("flags.mk") do (
    if /i "%%A"=="ZDISPLAY_STD"              set "F_STD=%%B"
    if /i "%%A"=="ZDISPLAY_WARN"             set "F_WARN=%%B"
    if /i "%%A"=="ZDISPLAY_REL"              set "F_REL=%%B"
    if /i "%%A"=="ZDISPLAY_DBG"              set "F_DBG=%%B"
    if /i "%%A"=="ZDISPLAY_LD"               set "F_LD=%%B"
    if /i "%%A"=="ZDISPLAY_LDREL"            set "F_LDREL=%%B"
    if /i "%%A"=="ZDISPLAY_LIBS"             set "F_LIBS=%%B"
    if /i "%%A"=="ZDISPLAY_TESTFLAGS"        set "F_TESTFLAGS=%%B"
    if /i "%%A"=="ZDISPLAY_TESTSRC"          set "F_TESTSRC=%%B"
    if /i "%%A"=="ZDISPLAY_TESTLIBS"         set "F_TESTLIBS=%%B"
    if /i "%%A"=="ZDISPLAY_TOOLCHAIN_URL"    set "F_TCURL=%%B"
    if /i "%%A"=="ZDISPLAY_TOOLCHAIN_SHA256" set "F_TCSHA=%%B"
)

rem ------------------------------------------------------------------- compiler
set "CXX="
where g++ >nul 2>&1 && set "CXX=g++"

if not defined CXX if exist "tools\w64devkit\bin\g++.exe" (
    set "PATH=%CD%\tools\w64devkit\bin;%PATH%"
    set "CXX=g++"
)

if not defined CXX (
    where clang++ >nul 2>&1 && set "CXX=clang++"
)

if not defined CXX (
    echo.
    echo Nenhum compilador C++ encontrado.
    echo Vou baixar o w64devkit ^(portatil, ~60 MB, sem instalacao^).
    echo.
    call :download_toolchain
    if errorlevel 1 goto :fail
    set "PATH=%CD%\tools\w64devkit\bin;%PATH%"
    set "CXX=g++"
)

echo Compilador: %CXX%

set "BUILDDIR=build\%CONFIG%"
set "CXXFLAGS=%F_STD% %F_WARN%"
set "LDFLAGS=%F_LD%"
if /i "%CONFIG%"=="debug" (
    set "CXXFLAGS=%CXXFLAGS% %F_DBG%"
) else (
    set "CXXFLAGS=%CXXFLAGS% %F_REL%"
    set "LDFLAGS=%LDFLAGS% %F_LDREL%"
)

set "SRC=src\common.cpp src\core.cpp src\icon.cpp src\backends_display.cpp src\backends_vendor.cpp src\backends_hw.cpp src\engine.cpp src\services.cpp src\ui_app.cpp src\ui_settings.cpp src\ui_events.cpp src\ui_guard.cpp src\ui_theme.cpp src\main.cpp"

if not exist build mkdir build
if not exist "%BUILDDIR%" mkdir "%BUILDDIR%"

rem --------------------------------------------------------------------- tests
rem The suite covers pure logic (color math, shadow curve, configuration, safety
rem limits, rules) and requires no hardware.
call :run_tests
if errorlevel 1 goto :fail
if defined ONLYTESTS goto :eof

rem ----------------------------------------------------------------- resources
set "RESOBJ="
where windres >nul 2>&1
if errorlevel 1 (
    echo AVISO: windres ausente; o executavel ficara sem icone e sem manifesto.
    goto :compile
)

set "RESFLAG="
if exist assets\zdisplay.ico set "RESFLAG=-DZDISPLAY_HAS_ICON"
echo Compilando recursos...
windres %RESFLAG% -i zdisplay.rc -o "%BUILDDIR%\zdisplay_res.o"
if errorlevel 1 goto :fail
set "RESOBJ=%BUILDDIR%\zdisplay_res.o"

:compile
echo Compilando...
%CXX% %CXXFLAGS% %SRC% %RESOBJ% -o "%BUILDDIR%\zdisplay.exe" %LDFLAGS% %F_LIBS%
if errorlevel 1 goto :fail

rem First build: generate the icon, then rebuild to embed it in the executable.
if not exist assets\zdisplay.ico (
    echo Gerando o icone...
    if not exist assets mkdir assets
    "%BUILDDIR%\zdisplay.exe" --make-icon
    if exist assets\zdisplay.ico (
        echo Recompilando com o icone...
        windres -DZDISPLAY_HAS_ICON -i zdisplay.rc -o "%BUILDDIR%\zdisplay_res.o"
        %CXX% %CXXFLAGS% %SRC% "%BUILDDIR%\zdisplay_res.o" -o "%BUILDDIR%\zdisplay.exe" %LDFLAGS% %F_LIBS%
        if errorlevel 1 goto :fail
    )
)

echo Conferindo protecoes do executavel...
call :verify_pe_hardening "%BUILDDIR%\zdisplay.exe"
if errorlevel 1 goto :fail

rem The copy to the project root fails while a zdisplay.exe is running. The new
rem binary is still valid inside build\, so this is reported as a warning rather
rem than a build error.
copy /y "%BUILDDIR%\zdisplay.exe" zdisplay.exe >nul
if errorlevel 1 (
    echo.
    echo AVISO: nao consegui substituir zdisplay.exe na raiz ^(esta em execucao?^).
    echo O binario novo esta em %BUILDDIR%\zdisplay.exe
    goto :eof
)
echo.
for %%F in (zdisplay.exe) do echo Pronto: zdisplay.exe  ^(%%~zF bytes^)
echo Execute com:  zdisplay.exe
goto :eof

rem ---------------------------------------------------------------- subroutines
:run_tests
    if not exist "%BUILDDIR%\test" mkdir "%BUILDDIR%\test"
    echo Compilando os testes...
    %CXX% %F_STD% %F_WARN% %F_TESTFLAGS% %F_TESTSRC% -o "%BUILDDIR%\test\test_zdisplay.exe" %F_TESTLIBS%
    if errorlevel 1 exit /b 1
    echo.
    "%BUILDDIR%\test\test_zdisplay.exe"
    if errorlevel 1 (
        echo.
        echo *** Ha testes falhando. ***
        exit /b 1
    )
    echo.
    exit /b 0

:download_toolchain
    if not exist tools mkdir tools
    echo Baixando %F_TCURL%
    curl -L --fail --progress-bar -o tools\w64devkit.7z.exe "%F_TCURL%"
    if errorlevel 1 (
        echo.
        echo Falha no download. Baixe o w64devkit manualmente em
        echo   https://github.com/skeeto/w64devkit/releases
        echo e extraia para  %CD%\tools\w64devkit
        exit /b 1
    )

    rem The archive is verified before anything is extracted: an unverified
    rem 90 MB toolchain download turns a substituted HTTP response into code
    rem execution.
    for /f "skip=1 tokens=1" %%H in ('certutil -hashfile tools\w64devkit.7z.exe SHA256') do (
        if not defined GOTHASH set "GOTHASH=%%H"
    )
    if not defined F_TCSHA (
        echo.
        echo O SHA-256 do que foi baixado e:
        echo   !GOTHASH!
        echo.
        echo ZDISPLAY_TOOLCHAIN_SHA256 esta vazio em flags.mk, entao nao tenho com
        echo o que comparar. Confira o valor acima na pagina oficial do release,
        echo cole em flags.mk e rode o build de novo.
        exit /b 1
    )
    if /i not "!GOTHASH!"=="%F_TCSHA%" (
        echo.
        echo *** O arquivo baixado NAO confere com o hash esperado. ***
        echo   esperado: %F_TCSHA%
        echo   recebido: !GOTHASH!
        del /q tools\w64devkit.7z.exe
        exit /b 1
    )
    echo Hash conferido.

    rem The package and each executable inside it are Authenticode-signed. The
    rem file is treated purely as a 7-Zip archive; the self-extractor is never
    rem executed.
    set "SIGSTATUS="
    for /f %%S in ('powershell -NoProfile -Command "$s=Get-AuthenticodeSignature -LiteralPath 'tools\w64devkit.7z.exe';$s.Status"') do set "SIGSTATUS=%%S"
    if /i not "!SIGSTATUS!"=="Valid" (
        echo.
        echo *** A assinatura digital do toolchain nao e valida. ***
        del /q tools\w64devkit.7z.exe
        exit /b 1
    )
    echo Assinatura digital conferida.

    echo Extraindo...
    tar -xf tools\w64devkit.7z.exe -C tools
    if errorlevel 1 exit /b 1
    del /q tools\w64devkit.7z.exe
    if not exist "tools\w64devkit\bin\g++.exe" (
        echo Extracao nao produziu tools\w64devkit\bin\g++.exe
        exit /b 1
    )
    exit /b 0

:verify_pe_hardening
    where objdump >nul 2>&1
    if errorlevel 1 (
        echo objdump nao encontrado; nao da para conferir DEP/ASLR do binario.
        exit /b 1
    )
    objdump -x "%~1" | findstr /c:"DYNAMIC_BASE" >nul
    if errorlevel 1 (
        echo *** ASLR nao esta marcado em %~1. ***
        exit /b 1
    )
    objdump -x "%~1" | findstr /c:"HIGH_ENTROPY_VA" >nul
    if errorlevel 1 (
        echo *** ASLR de alta entropia nao esta marcado em %~1. ***
        exit /b 1
    )
    objdump -x "%~1" | findstr /c:"NX_COMPAT" >nul
    if errorlevel 1 (
        echo *** DEP/NX nao esta marcado em %~1. ***
        exit /b 1
    )
    exit /b 0

:fail
echo.
echo *** A compilacao falhou. ***
exit /b 1
