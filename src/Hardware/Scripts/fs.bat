@echo off
setlocal

:: fs.bat -- Pack and flash SPIFFS
:: Usage: fs.bat <SPIFFS_DIR> [CHIP] [FREQ] [OFFSET] [SIZE] [PORT]
::   SPIFFS_DIR  directory to pack (required)
::   CHIP        chip type          default esp32c3
::   FREQ        flash freq         default 80m
::   OFFSET      partition offset   default 0x190000
::   SIZE        partition size     default 0x270000
::   PORT        COM port           default COM3

if "%~1"=="" (
    echo [ERROR] Usage: fs.bat ^<SPIFFS_DIR^> [CHIP] [FREQ] [OFFSET] [SIZE] [PORT]
    echo         Example: fs.bat ..\FS\SPIFFS esp32c3 80m 0x190000 0x270000 COM3
    exit /b 1
)

set SPIFFS_DIR=%~1
set CHIP=esp32c3
set FREQ=80m
set OFFSET=0x190000
set SIZE=0x270000
set PORT=COM3

if not "%~2"=="" set CHIP=%2
if not "%~3"=="" set FREQ=%3
if not "%~4"=="" set OFFSET=%4
if not "%~5"=="" set SIZE=%5
if not "%~6"=="" set PORT=%6

set SCRIPT_DIR=%~dp0
set ESPTOOL=%SCRIPT_DIR%esptool.exe
set SPIFFS_PY=%SCRIPT_DIR%spiffs.py
set FS_BIN=%SCRIPT_DIR%fs.bin

if not exist "%SPIFFS_DIR%" (
    echo [ERROR] SPIFFS dir not found: %SPIFFS_DIR%
    exit /b 1
)
if not exist "%SPIFFS_PY%" (
    echo [ERROR] spiffs.py not found: %SPIFFS_PY%
    exit /b 1
)

echo.
echo  SPIFFS Dir : %SPIFFS_DIR%
echo  Chip       : %CHIP%
echo  Freq       : %FREQ%
echo  Offset     : %OFFSET%
echo  Size       : %SIZE%
echo  Port       : %PORT%
echo  Output     : %FS_BIN%
echo.

echo [1/2] Packing SPIFFS ...
python "%SPIFFS_PY%" %SIZE% "%SPIFFS_DIR%" "%FS_BIN%"
if %errorlevel% neq 0 (
    echo [FAIL] SPIFFS pack failed
    exit /b 1
)
echo [OK] packed: %FS_BIN%
echo.

echo [2/2] Flashing to device ...
"%ESPTOOL%" --chip %CHIP% --port %PORT% -b 1152000 ^
    --before default_reset --after hard_reset ^
    write_flash --flash_mode dio --flash_size keep --flash_freq %FREQ% ^
    %OFFSET% "%FS_BIN%"
if %errorlevel% neq 0 (
    echo [FAIL] flash failed, check port and device
    exit /b 1
)

echo.
echo [OK] FS flash done
endlocal