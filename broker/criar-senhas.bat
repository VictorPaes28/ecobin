@echo off
rem Cria (ou recria) os usuarios do broker. O arquivo "senhas" fica fora do Git.
cd /d "%~dp0"
set /p SENHA_LIXEIRA=Senha para o usuario "lixeira" (ESP32 e simulador):
set /p SENHA_PAINEL=Senha para o usuario "painel" (navegador):
"C:\Program Files\mosquitto\mosquitto_passwd.exe" -c -b senhas lixeira %SENHA_LIXEIRA%
"C:\Program Files\mosquitto\mosquitto_passwd.exe" -b senhas painel %SENHA_PAINEL%
echo Pronto. Coloque a senha da lixeira em esp32-esp8266\include\segredos.h (MQTT_PASS).
pause
