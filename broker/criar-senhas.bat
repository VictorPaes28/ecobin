@echo off
rem Cria (ou recria) os usuarios do broker. O arquivo "senhas" fica fora do Git.
rem Evite espaco e os caracteres & | ^ % " nas senhas.
cd /d "%~dp0"
set /p "SENHA_LIXEIRA=Senha para o usuario lixeira (ESP32 e simulador): "
set /p "SENHA_PAINEL=Senha para o usuario painel (navegador): "
set /p "SENHA_BACKEND=Senha para o usuario backend (servidor do historico): "
"C:\Program Files\mosquitto\mosquitto_passwd.exe" -c -b senhas lixeira "%SENHA_LIXEIRA%"
"C:\Program Files\mosquitto\mosquitto_passwd.exe" -b senhas painel "%SENHA_PAINEL%"
"C:\Program Files\mosquitto\mosquitto_passwd.exe" -b senhas backend "%SENHA_BACKEND%"
echo Pronto. Coloque a senha da lixeira em esp32-esp8266\include\segredos.h (MQTT_PASS)
echo e reinicie o broker (feche a janela "EcoBin - broker MQTT" e abra iniciar.bat).
pause
