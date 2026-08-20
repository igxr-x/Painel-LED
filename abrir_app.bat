@echo off
REM ====================================================================
REM  Abre o app configurador (instala o pyserial se faltar)
REM ====================================================================
setlocal
cd /d "%~dp0app"

python -c "import serial" >nul 2>nul
if not %ERRORLEVEL%==0 (
    echo Instalando dependencia pyserial...
    python -m pip install -r requirements.txt
)

python app.py
if not %ERRORLEVEL%==0 pause
endlocal
