#pragma once
// Configuracoes do EcoBin. Para outra lixeira ou para o hardware real, so mexer aqui.

// ---------------- Identificacao ----------------
#define DEVICE_ID "lx01"
#define LOCAL     "Prototipo (Wokwi)"
#define PREFIXO   "ecobin/cesar-g7k2"
#define TOPICO(sub) PREFIXO "/" DEVICE_ID "/" sub   // ex.: TOPICO("nivel")

// ---------------- Rede ----------------
// SIMULACAO_WOKWI vem do platformio.ini: env "wokwi" = 1, env "esp32dev" = 0.
// No Wokwi pelo navegador nao existe platformio.ini, entao o padrao e simulacao.
#ifndef SIMULACAO_WOKWI
  #define SIMULACAO_WOKWI 1
#endif

#if SIMULACAO_WOKWI
  #define WIFI_SSID  "Wokwi-GUEST"
  #define WIFI_PASS  ""
  #define WIFI_CANAL 6      // so no Wokwi: fixa o canal para conectar mais rapido
  #define MQTT_HOST  "broker.hivemq.com"
#else
  #if !__has_include("segredos.h")
    #error "Crie include/segredos.h a partir de include/segredos.exemplo.h"
  #endif
  #include "segredos.h"   // WIFI_SSID, WIFI_PASS, MQTT_HOST (fora do Git)
  #define WIFI_CANAL 0      // 0 = procura o canal automaticamente
#endif

#define MQTT_PORT 1883
#define FUSO_S    (-3 * 3600)           // horario de Brasilia (NTP)

// ---------------- Pinos ----------------
#define PIN_TRIG 5
#define PIN_ECHO 18   // circuito real: divisor de tensao (ECHO sai em 5 V)
#define PIN_IR   27   // sensor IR (no Wokwi: botao azul)
#define PIN_BTN  21   // botao "lixeira esvaziada"
// Semaforo com 3 LEDs comuns (2 pernas). D33, D26 e D14 ficam de 2 em 2 fileiras na placa:
// cada LED usa a fileira do pino (resistor + perna longa) e a de baixo (perna curta -> GND).
#define PIN_LED_VERDE    33
#define PIN_LED_AMARELO  26
#define PIN_LED_VERMELHO 14

// ---------------- Lixeira (medir na lixeira de verdade) ----------------
//   sensor ->  ___  <- tampa
//             |   |  DIST_CHEIA_CM: sensor ate a borda (lixo aqui = 100%)
//      topo   \   /  <- LARGURA_TOPO_CM
//              \ /   ALTURA_CM: sensor ate o fundo (lixeira vazia)
//      fundo   ---   <- LARGURA_FUNDO_CM
#define ALTURA_CM        40.0f  // dica: com a lixeira vazia, ler a distancia no Serial
#define DIST_CHEIA_CM     5.0f  // >= 4 cm: abaixo disso o HC-SR04 nao mede direito (zona cega)
#define LARGURA_FUNDO_CM 25.0f  // lado ou diametro no fundo
#define LARGURA_TOPO_CM  25.0f  // lado ou diametro na borda (igual ao fundo = paredes retas)
#define TEMPERATURA_C    28.0f  // temperatura media do local: corrige a velocidade do som

#define NIVEL_ATENCAO  50      // % a partir do qual fica amarelo
#define HISTERESE      10      // % abaixo do limiar para sair de CHEIA
#define CONFIRMA_CHEIA  3      // leituras seguidas acima do limiar para declarar CHEIA
#define LIMIAR_PADRAO  80      // % para "cheia"; ajustavel pelo painel
#define LIMIAR_MIN     50      // faixa aceita via painel
#define LIMIAR_MAX     95
#define AUTO_PICO_MIN    40    // so detecta esvaziamento se antes passou desta %...
#define AUTO_NIVEL_VAZIA 10    // ...e agora ficou abaixo desta % (3 leituras seguidas)

// ---------------- Tempos (ms) ----------------
#define PERIODO_LEITURA_MS  1000
#define FALHAS_SENSOR          5   // leituras seguidas sem eco para considerar o sensor com falha
#define INTERVALO_PING_MS     60   // minimo recomendado pelo HC-SR04 entre medicoes
#define PAUSA_DESCARTE_MS   2000   // ignora o ultrassonico durante e logo apos um descarte
#define HEARTBEAT_MS       10000   // publica o nivel mesmo sem mudanca
#define DIAG_MS            30000
#define JANELA_COLETA_MS   60000   // avisos de coleta dentro deste tempo contam como a mesma coleta
#define WIFI_TIMEOUT_MS    15000
#define BACKOFF_MIN_MS      1000   // espera entre tentativas de reconexao (dobra a cada falha)
#define BACKOFF_MAX_MS     30000
