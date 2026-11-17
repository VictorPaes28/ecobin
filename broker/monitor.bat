@echo off
rem Mostra em portugues as mensagens do EcoBin (precisa do broker ligado: iniciar.bat)
cd /d "%~dp0"
chcp 65001 >nul
title EcoBin - monitor das mensagens
set PY=..\applications\simulador\.venv\Scripts\python.exe
if not exist "%PY%" set PY=python
"%PY%" monitor.py
pause
