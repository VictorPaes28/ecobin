@echo off
rem Inicia o broker local do EcoBin. Deixe esta janela aberta enquanto usa o sistema.
cd /d "%~dp0"
chcp 65001 >nul
title EcoBin - broker MQTT
if not exist senhas (
  echo Arquivo "senhas" nao encontrado. Rode criar-senhas.bat primeiro.
  pause
  exit /b 1
)
netstat -an | findstr /r /c:":1884 .*LISTENING" >nul
if not errorlevel 1 (
  echo O broker do EcoBin JA ESTA LIGADO ^(porta 1884 em uso^). Nao precisa abrir de novo.
  echo Se quiser reinicia-lo, feche a outra janela "EcoBin - broker MQTT" e abra esta de novo.
  pause
  exit /b 0
)
echo Broker EcoBin: MQTT na porta 1884, WebSocket na porta 9001
echo IPs deste PC (use o da rede Wi-Fi, normalmente 192.168.x.x ou 172.x.x.x, no segredos.h do ESP32):
ipconfig | findstr /c:"IPv4"
echo.
echo Aqui aparecem os aparelhos conectando e desconectando.
echo Para ver o conteudo das mensagens, abra monitor.bat.
echo.
rem Para ver cada pacote MQTT (PUBLISH, PINGREQ...), acrescente -v no fim da linha abaixo
"C:\Program Files\mosquitto\mosquitto.exe" -c mosquitto.conf
pause
