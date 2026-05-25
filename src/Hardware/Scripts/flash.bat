@echo off
setlocal

:: flash.bat -- Flash firmware
:: Usage: flash.bat [CHIP] [FREQ] [PORT]
::   CHIP   chip type   default esp32c3
::   FREQ   flash freq  default 80m
::   PORT   COM port    default COM3
:: Firmware: main.bin in parent directory

set CHIP=esp32c3
set FREQ=80m
set PORT=COM3

if not "%~1"=="" set CHIP=%1
if not "%~2"=="" set FREQ=%2
if not "%~3"=="" set PORT=%3

set ESPTOOL=%~dp0esptool.exe
set BIN_PATH=%~dp0..\main.bin

if not exist "%BIN_PATH%" (
    echo [ERROR] firmware not found: %BIN_PATH%
    exit /b 1
)

echo.
echo  Chip : %CHIP%
echo  Freq : %FREQ%
echo  Port : %PORT%
echo  Bin  : %BIN_PATH%
echo.

"%ESPTOOL%" --chip %CHIP% --port %PORT% -b 1152000 ^
    --before default_reset --after hard_reset ^
    write_flash --flash_mode dio --flash_size keep --flash_freq %FREQ% ^
    0x0 "%BIN_PATH%"

echo.
if %errorlevel%==0 ( echo [OK] flash done ) else ( echo [FAIL] flash failed, check port and firmware )
endlocal