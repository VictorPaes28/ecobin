@echo off
rem Inicia o broker local do EcoBin. Deixe esta janela aberta enquanto usa o sistema.
cd /d "%~dp0"
if not exist senhas (
  echo Arquivo "senhas" nao encontrado. Rode criar-senhas.bat primeiro.
  pause
  exit /b 1
)
title EcoBin - broker MQTT
echo Broker EcoBin: MQTT na porta 1884, WebSocket na porta 9001
echo IPs deste PC (use o da rede Wi-Fi no segredos.h do ESP32):
ipconfig | findstr /c:"IPv4"
echo.
echo Aqui aparecem os aparelhos conectando e desconectando.
echo Para ver o conteudo das mensagens, abra monitor.bat.
echo.
rem Para ver cada pacote MQTT (PUBLISH, PINGREQ...), troque a linha abaixo por: ... -c mosquitto.conf -v
"C:\Program Files\mosquitto\mosquitto.exe" -c mosquitto.conf
pause
