@echo off
rem Liga o back-end do EcoBin (precisa do broker ligado: broker\iniciar.bat).
rem Deixe esta janela aberta. Painel com historico: http://localhost:5000/
cd /d "%~dp0"
chcp 65001 >nul
title EcoBin - back-end (historico)
if not exist .venv\Scripts\python.exe (
  echo Primeira vez: instalando dependencias...
  python -m venv .venv || goto erro
  .venv\Scripts\python.exe -m pip install -r requirements.txt || goto erro
)
rem Senha do usuario "backend": do arquivo local do broker, se existir; senao pergunta
set ECOBIN_SENHA_BACKEND=
for /f "tokens=1,* delims=: " %%a in ('type ..\..\broker\credenciais.local.txt 2^>nul') do if "%%a"=="backend" set "ECOBIN_SENHA_BACKEND=%%b"
if not defined ECOBIN_SENHA_BACKEND set /p "ECOBIN_SENHA_BACKEND=Senha do usuario backend do broker: "
.venv\Scripts\python.exe app.py
pause
exit /b 0
:erro
echo Falha ao instalar. Confira se o Python esta instalado.
pause
