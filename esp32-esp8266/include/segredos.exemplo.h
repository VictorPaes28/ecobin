#pragma once
// Copie este arquivo para "segredos.h" (mesma pasta) e preencha.
// segredos.h esta no .gitignore: senha de Wi-Fi NAO vai para o repositorio publico.

#define WIFI_SSID  "nome-da-rede"
#define WIFI_PASS  "senha-da-rede"
#define MQTT_HOST  "192.168.0.10"   // IP do PC com o broker (broker/iniciar.bat mostra o IP)
#define MQTT_PORT  1884             // porta do broker do projeto (broker/mosquitto.conf)
#define MQTT_USER  "lixeira"
#define MQTT_PASS  "senha-do-usuario-lixeira"   // criada com broker/criar-senhas.bat
