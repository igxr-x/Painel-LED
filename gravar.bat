@echo off
REM ====================================================================
REM  Gravar firmware via PlatformIO
REM  Uso:  gravar.bat [micro|nano|nanoold]      (padrao = micro)
REM    micro    -> Arduino Micro (ATmega32U4)
REM    nano     -> Arduino Nano bootloader NOVO (Optiboot, 115200)
REM    nanoold  -> Arduino Nano bootloader ANTIGO (57600)
REM ====================================================================
setlocal
cd /d "%~dp0firmware"

set "ENV=%~1"
if "%ENV%"=="" set "ENV=micro"

echo.
echo === Painel LED Lambda - Gravacao via PlatformIO (placa: %ENV%) ===
echo.

REM Procura o executavel do PlatformIO (pio / platformio / modulo python)
where pio >nul 2>nul
if %ERRORLEVEL%==0 ( set "PIO=pio" & goto :run )
where platformio >nul 2>nul
if %ERRORLEVEL%==0 ( set "PIO=platformio" & goto :run )
python -m platformio --version >nul 2>nul
if %ERRORLEVEL%==0 ( set "PIO=python -m platformio" & goto :run )

echo [ERRO] PlatformIO nao encontrado.
echo   Instale com:  pip install platformio
echo   ou instale a extensao PlatformIO IDE no VS Code.
goto :end

:run
echo Usando: %PIO%   (ambiente: %ENV%)
echo Compilando e gravando...
echo.
%PIO% run -e %ENV% -t upload
if %ERRORLEVEL%==0 (
    echo.
    echo === Gravacao concluida com sucesso ===
) else (
    echo.
    echo [ERRO] Falha na gravacao.
    echo   - Na Nano, se falhar, tente:  gravar.bat nanoold
    echo   - Verifique o cabo e o driver CH340/FTDI da porta COM.
)

:end
echo.
pause
endlocal
