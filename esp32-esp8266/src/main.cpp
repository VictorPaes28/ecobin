/*
 * EcoBin - lixeira inteligente com ESP32 + FreeRTOS + MQTT
 *
 *   TaskSensor ---[filaLeituras]---> TaskProcessamento --(semCheia)--> TaskAlerta --+
 *   ISR IR ------(semDescartes)----> TaskDescartes                                 |
 *   ISR botao ---(notification)----> TaskEsvaziar ---------------------------------+--[filaEventos]--> TaskRede --> MQTT
 *                                                                                                       ^
 *   Todas leem/escrevem o struct "lixeira" protegido por mutexLixeira -----------------------------------+
 *
 * Mecanismos: Fila (filaLeituras, filaEventos), Semaforo binario (semCheia),
 * Semaforo contador (semDescartes), ISR + Task Notification (botao), Mutex (lixeira e Serial).
 */

#include <Arduino.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <sys/time.h>
#include "config.h"

// ================= Tipos =================

typedef enum { NORMAL, ATENCAO, CHEIA } Estado;
static const char* NOME_ESTADO[] = { "NORMAL", "ATENCAO", "CHEIA" };

// Estado atual da lixeira: um unico struct, sempre acessado com mutexLixeira
typedef struct {
  float    distCm;
  int      nivel;      // %
  Estado   estado;
  int      limiar;     // % para considerar cheia
  uint32_t descartes;  // desde o ultimo esvaziamento
  bool     sensorOk;   // false ate a 1a leitura valida ou apos FALHAS_SENSOR leituras sem eco
  int      pico;       // maior nivel (confirmado) desde a ultima coleta
} Lixeira;

// Eventos que nao podem se perder: ficam na fila ate serem publicados
typedef enum { EV_CHEIA, EV_ESVAZIADA } TipoEvento;

// Quem avisou que a lixeira foi esvaziada (bits da Task Notification)
#define ORIGEM_BOTAO  (1u << 0)
#define ORIGEM_PAINEL (1u << 1)
#define ORIGEM_AUTO   (1u << 2)

typedef struct {
  TipoEvento tipo;
  int        nivel;
  uint32_t   descartes;
  uint32_t   origem;     // so para EV_ESVAZIADA
  int64_t    ts;         // ms desde 1970 (0 = relogio ainda nao sincronizado)
} Evento;

// ================= Objetos FreeRTOS =================

QueueHandle_t     filaLeituras;   // float: distancia em cm
QueueHandle_t     filaEventos;    // Evento
SemaphoreHandle_t semCheia;       // binario
SemaphoreHandle_t semDescartes;   // contador
SemaphoreHandle_t mutexLixeira;
SemaphoreHandle_t mutexSerial;
TaskHandle_t      hEsvaziar;
TaskHandle_t      tarefas[6];     // para o diagnostico de stack

Lixeira lixeira = { 0, 0, NORMAL, LIMIAR_PADRAO, 0, false, 0 };

WiFiClient   wifiClient;
PubSubClient mqtt(wifiClient);

// ================= Utilitarios =================

void logMsg(const char* fmt, ...) {
  char buf[160];
  va_list args;
  va_start(args, fmt);
  vsnprintf(buf, sizeof(buf), fmt, args);
  va_end(args);
  xSemaphoreTake(mutexSerial, portMAX_DELAY);
  Serial.println(buf);
  xSemaphoreGive(mutexSerial);
}

Lixeira lerLixeira() {
  xSemaphoreTake(mutexLixeira, portMAX_DELAY);
  Lixeira copia = lixeira;
  xSemaphoreGive(mutexLixeira);
  return copia;
}

int64_t agoraMs() {
  struct timeval tv;
  gettimeofday(&tv, NULL);
  if (tv.tv_sec < 1700000000) return 0;  // NTP ainda nao respondeu
  return (int64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

void enfileirarEvento(TipoEvento tipo, int nivel, uint32_t descartes, uint32_t origem = 0) {
  Evento ev = { tipo, nivel, descartes, origem, agoraMs() };
  if (xQueueSend(filaEventos, &ev, 0) != pdTRUE) logMsg("[EVENTO] fila cheia, evento perdido");
}

// Semaforo: um LED aceso por vez
void setLeds(Estado e) {
  digitalWrite(PIN_LED_VERDE,    e == NORMAL);
  digitalWrite(PIN_LED_AMARELO,  e == ATENCAO);
  digitalWrite(PIN_LED_VERMELHO, e == CHEIA);
}

// ================= Medicao =================

// Velocidade do som corrigida pela temperatura: 331,3 + 0,606*T m/s -> cm/us
const float CM_POR_US = (331.3f + 0.606f * TEMPERATURA_C) / 10000.0f;
// Espera o eco por no maximo o tempo de ida e volta de 1,5x a altura da lixeira
const unsigned long TIMEOUT_ECO_US = (unsigned long)(2.0f * ALTURA_CM * 1.5f / CM_POR_US);

// Distancia em cm, ou -1 se nao houve eco ou se a leitura nao cabe dentro da lixeira
// (tampa aberta, eco desviado). Abaixo de 2 cm o HC-SR04 nao mede.
float medirDistancia() {
  digitalWrite(PIN_TRIG, LOW);
  delayMicroseconds(2);
  digitalWrite(PIN_TRIG, HIGH);
  delayMicroseconds(10);
  digitalWrite(PIN_TRIG, LOW);
  unsigned long dur = pulseIn(PIN_ECHO, HIGH, TIMEOUT_ECO_US);
  float d = dur * CM_POR_US / 2.0f;
  return (dur == 0 || d < 2.0f || d > ALTURA_CM * 1.1f) ? -1 : d;
}

// Distancia -> % do VOLUME ocupado.
// A largura varia linearmente do fundo (a) ao topo (b); a area da secao e proporcional
// a largura^2 (vale para secao quadrada ou redonda). Integrando de 0 a h:
//   volume(h) / volume(H) = (w(h)^3 - a^3) / (b^3 - a^3),  com w(h) = a + (b - a) * h / H
// Com paredes retas (a == b) vira simplesmente h / H.
int volumePct(float dist) {
  const float H = ALTURA_CM - DIST_CHEIA_CM;   // altura util
  const float a = LARGURA_FUNDO_CM, b = LARGURA_TOPO_CM;
  float h = constrain(ALTURA_CM - dist, 0.0f, H);
  float frac;
  if (fabsf(b - a) < 0.01f) {
    frac = h / H;
  } else {
    float w = a + (b - a) * h / H;
    frac = (w * w * w - a * a * a) / (b * b * b - a * a * a);
  }
  return constrain(lroundf(frac * 100.0f), 0, 100);
}

// ================= ISRs =================

volatile TickType_t tickUltimoIR = 0;   // ultimo acionamento do IR (para pausar o ultrassonico)

void IRAM_ATTR isrIR() {
  static TickType_t ultimo = 0;
  TickType_t agora = xTaskGetTickCountFromISR();
  tickUltimoIR = agora;
  if (agora - ultimo < pdMS_TO_TICKS(1000)) return;  // no maximo 1 descarte por segundo
  ultimo = agora;
  BaseType_t acordou = pdFALSE;
  xSemaphoreGiveFromISR(semDescartes, &acordou);
  portYIELD_FROM_ISR(acordou);
}

void IRAM_ATTR isrBotao() {
  static TickType_t ultimo = 0;
  TickType_t agora = xTaskGetTickCountFromISR();
  if (agora - ultimo < pdMS_TO_TICKS(300)) return;   // debounce
  ultimo = agora;
  BaseType_t acordou = pdFALSE;
  xTaskNotifyFromISR(hEsvaziar, ORIGEM_BOTAO, eSetBits, &acordou);
  portYIELD_FROM_ISR(acordou);
}

// ================= Tasks da lixeira =================

// Mao ou objeto passando pela boca da lixeira: o ultrassonico mediria ele, nao o lixo
bool descarteEmAndamento() {
  return digitalRead(PIN_IR) == LOW ||
         (tickUltimoIR != 0 && xTaskGetTickCount() - tickUltimoIR < pdMS_TO_TICKS(PAUSA_DESCARTE_MS));
}

// Mediana de 5 medicoes do ultrassonico -> filaLeituras
void TaskSensor(void* pv) {
  TickType_t proximo = xTaskGetTickCount();
  int falhas = 0;
  for (;;) {
    float v[5];
    int n = 0;
    bool pausado = descarteEmAndamento();
    for (int i = 0; i < 5 && !pausado; i++) {
      float d = medirDistancia();
      if (d > 0) v[n++] = d;
      vTaskDelay(pdMS_TO_TICKS(INTERVALO_PING_MS));
      pausado = descarteEmAndamento();
    }
    if (pausado) {
      // descarte acontecendo: descarta o ciclo inteiro (nao conta como falha do sensor)
    } else if (n >= 3) {  // mediana so vale com a maioria das medicoes validas
      for (int i = 1; i < n; i++)  // insertion sort para achar a mediana
        for (int j = i; j > 0 && v[j] < v[j - 1]; j--) { float t = v[j]; v[j] = v[j - 1]; v[j - 1] = t; }
      float mediana = v[n / 2];
      xQueueSend(filaLeituras, &mediana, 0);
      falhas = 0;
    } else if (++falhas == FALHAS_SENSOR) {
      // sem eco: jumper solto ou sensor queimado. Avisa em vez de repetir o ultimo nivel.
      xSemaphoreTake(mutexLixeira, portMAX_DELAY);
      lixeira.sensorOk = false;
      xSemaphoreGive(mutexLixeira);
      logMsg("[SENSOR] ultrassonico sem resposta");
    }
    vTaskDelayUntil(&proximo, pdMS_TO_TICKS(PERIODO_LEITURA_MS));
  }
}

// Converte distancia em volume/estado. Na transicao para CHEIA libera o semaforo binario.
// Tambem percebe sozinha quando a lixeira foi esvaziada (o nivel cai e fica baixo).
void TaskProcessamento(void* pv) {
  float dist;
  int acima = 0;          // leituras seguidas acima do limiar
  int baixo = 0;          // leituras seguidas "vazia" depois de ter enchido
  int ult[3] = { 0, 0, 0 };
  for (;;) {
    xQueueReceive(filaLeituras, &dist, portMAX_DELAY);
    int nivel = volumePct(dist);
    ult[0] = ult[1]; ult[1] = ult[2]; ult[2] = nivel;
    int sustentado = min(ult[0], min(ult[1], ult[2]));   // um pico isolado nao conta

    xSemaphoreTake(mutexLixeira, portMAX_DELAY);
    Estado anterior = lixeira.estado;
    int limiar = lixeira.limiar;
    acima = nivel >= limiar ? acima + 1 : 0;
    // CHEIA so depois de CONFIRMA_CHEIA leituras seguidas (um saco em pe ou um pico nao dispara alerta);
    // para sair, precisa cair abaixo de limiar - HISTERESE
    Estado e = (acima >= CONFIRMA_CHEIA || (anterior == CHEIA && nivel >= limiar - HISTERESE)) ? CHEIA
             : (nivel >= NIVEL_ATENCAO) ? ATENCAO : NORMAL;
    bool mudou = nivel != lixeira.nivel || e != anterior || !lixeira.sensorOk;
    lixeira.distCm = dist;
    lixeira.nivel = nivel;
    lixeira.estado = e;
    lixeira.sensorOk = true;
    if (sustentado > lixeira.pico) lixeira.pico = sustentado;
    int pico = lixeira.pico;
    baixo = (pico >= AUTO_PICO_MIN && nivel <= AUTO_NIVEL_VAZIA) ? baixo + 1 : 0;
    xSemaphoreGive(mutexLixeira);

    if (baixo == CONFIRMA_CHEIA) {
      logMsg("[COLETA] nivel caiu de %d%% para %d%%: esvaziamento detectado", pico, nivel);
      xTaskNotify(hEsvaziar, ORIGEM_AUTO, eSetBits);
    }
    setLeds(e);
    if (e == CHEIA && anterior != CHEIA) xSemaphoreGive(semCheia);
    if (mudou) logMsg("[NIVEL] %.1f cm | %d%% | %s | limiar %d%%", dist, nivel, NOME_ESTADO[e], limiar);
  }
}

void TaskAlerta(void* pv) {
  for (;;) {
    xSemaphoreTake(semCheia, portMAX_DELAY);
    Lixeira l = lerLixeira();
    enfileirarEvento(EV_CHEIA, l.nivel, l.descartes);
    logMsg("[ALERTA] lixeira cheia (%d%%)", l.nivel);
  }
}

// Cada "give" da ISR do IR e um descarte a contabilizar
void TaskDescartes(void* pv) {
  for (;;) {
    xSemaphoreTake(semDescartes, portMAX_DELAY);
    xSemaphoreTake(mutexLixeira, portMAX_DELAY);
    uint32_t total = ++lixeira.descartes;
    xSemaphoreGive(mutexLixeira);
    logMsg("[DESCARTE] total = %u", (unsigned)total);
  }
}

// Acordada por Task Notification: ISR do botao, comando "reset" do painel ou deteccao automatica.
// O valor da notificacao diz quem avisou (bits ORIGEM_*).
void TaskEsvaziar(void* pv) {
  TickType_t ultimaColeta = 0;
  bool houveColeta = false;
  for (;;) {
    uint32_t origem = 0;
    xTaskNotifyWait(0, UINT32_MAX, &origem, portMAX_DELAY);
    // botao e deteccao automatica costumam avisar a mesma coleta: registra so uma
    if (houveColeta && xTaskGetTickCount() - ultimaColeta < pdMS_TO_TICKS(JANELA_COLETA_MS)) {
      logMsg("[ESVAZIAR] coleta ja registrada ha pouco, aviso ignorado");
      continue;
    }
    houveColeta = true;
    ultimaColeta = xTaskGetTickCount();
    xSemaphoreTake(mutexLixeira, portMAX_DELAY);
    int nivelAntes = max(lixeira.pico, lixeira.nivel);
    uint32_t descartes = lixeira.descartes;
    lixeira.descartes = 0;
    lixeira.pico = lixeira.nivel;
    xSemaphoreGive(mutexLixeira);
    enfileirarEvento(EV_ESVAZIADA, nivelAntes, descartes, origem);
    logMsg("[ESVAZIAR] nivel antes = %d%%, descartes = %u, origem = %s", nivelAntes, (unsigned)descartes,
           origem & ORIGEM_BOTAO ? "botao" : origem & ORIGEM_PAINEL ? "painel" : "automatica");
  }
}

// ================= Rede (Wi-Fi + MQTT) =================
// Unica task que usa o Wi-Fi e o cliente MQTT. Funciona como maquina de estados:
//   INICIO -> WIFI -> MQTT -> ONLINE, voltando para WIFI/MQTT quando algo cai,
//   com espera crescente entre tentativas (1 s, 2 s, 4 s ... ate 30 s).

typedef enum { REDE_INICIO, REDE_WIFI, REDE_MQTT, REDE_ONLINE } EstadoRede;

bool publicarJson(const char* topico, JsonDocument& doc, bool retain) {
  char buf[256];
  size_t n = serializeJson(doc, buf, sizeof(buf));
  return mqtt.publish(topico, (const uint8_t*)buf, n, retain);
}

// Comandos do dashboard: {"acao":"reset"} e/ou {"limiar":75}. Roda dentro da TaskRede (mqtt.loop).
void aoReceberComando(char* topico, byte* payload, unsigned int len) {
  JsonDocument doc;
  if (deserializeJson(doc, payload, len)) { logMsg("[CMD] JSON invalido"); return; }

  if (strcmp(doc["acao"] | "", "reset") == 0) xTaskNotify(hEsvaziar, ORIGEM_PAINEL, eSetBits);

  if (doc["limiar"].is<int>()) {
    int v = constrain(doc["limiar"].as<int>(), LIMIAR_MIN, LIMIAR_MAX);
    xSemaphoreTake(mutexLixeira, portMAX_DELAY);
    lixeira.limiar = v;
    xSemaphoreGive(mutexLixeira);
    logMsg("[CMD] limiar = %d%%", v);
  }
}

bool conectarMqtt() {
  static char clientId[32];
  snprintf(clientId, sizeof(clientId), "ecobin-%s-%06x", DEVICE_ID, (unsigned)(ESP.getEfuseMac() & 0xFFFFFF));
  // LWT: se a conexao cair sem aviso, o broker publica "offline" sozinho
  if (!mqtt.connect(clientId, TOPICO("status"), 1, true, "offline")) return false;

  mqtt.publish(TOPICO("status"), "online", true);
  JsonDocument info;
  info["local"] = LOCAL;
  info["altura_cm"] = ALTURA_CM;
  publicarJson(TOPICO("info"), info, true);
  mqtt.subscribe(TOPICO("cmd"), 1);
  return true;
}

// Publica os eventos pendentes. So remove da fila depois de publicar com sucesso.
bool enviarEventos() {
  Evento ev;
  while (xQueuePeek(filaEventos, &ev, 0) == pdTRUE) {
    JsonDocument doc;
    doc["tipo"] = ev.tipo == EV_CHEIA ? "CHEIA" : "ESVAZIADA";
    doc["ts"] = ev.ts;
    bool ok;
    if (ev.tipo == EV_CHEIA) {
      doc["nivel_pct"] = ev.nivel;
      ok = publicarJson(TOPICO("alerta"), doc, false);
    } else {
      doc["nivel_antes"] = ev.nivel;
      doc["descartes"] = ev.descartes;
      doc["origem"] = ev.origem & ORIGEM_BOTAO ? "botao" : ev.origem & ORIGEM_PAINEL ? "painel" : "automatica";
      ok = publicarJson(TOPICO("evento"), doc, false);
    }
    if (!ok) return false;
    xQueueReceive(filaEventos, &ev, 0);
  }
  return true;
}

// Publica o nivel quando muda ou a cada HEARTBEAT_MS; descartes quando mudam.
bool enviarTelemetria(bool forcar) {
  static Lixeira ultima;
  static TickType_t ultimoEnvio = 0;
  Lixeira l = lerLixeira();
  bool ok = true;

  bool mudou = l.nivel != ultima.nivel || l.estado != ultima.estado ||
               l.limiar != ultima.limiar || l.sensorOk != ultima.sensorOk;
  if (forcar || mudou || xTaskGetTickCount() - ultimoEnvio >= pdMS_TO_TICKS(HEARTBEAT_MS)) {
    JsonDocument doc;
    doc["dist_cm"] = roundf(l.distCm * 10) / 10;
    doc["nivel_pct"] = l.nivel;
    doc["estado"] = NOME_ESTADO[l.estado];
    doc["limiar"] = l.limiar;
    doc["sensor_ok"] = l.sensorOk;
    doc["ts"] = agoraMs();
    ok = publicarJson(TOPICO("nivel"), doc, true);
    if (ok) ultimoEnvio = xTaskGetTickCount();
  }
  if (ok && (forcar || l.descartes != ultima.descartes)) {
    JsonDocument doc;
    doc["total"] = l.descartes;
    ok = publicarJson(TOPICO("descartes"), doc, true);
  }
  if (ok) ultima = l;
  return ok;
}

// Memoria livre e menor folga de stack de cada task (em bytes no ESP32)
void enviarDiagnostico() {
  static TickType_t ultimo = 0;
  if (xTaskGetTickCount() - ultimo < pdMS_TO_TICKS(DIAG_MS)) return;
  ultimo = xTaskGetTickCount();
  JsonDocument doc;
  doc["heap"] = ESP.getFreeHeap();
  JsonObject stack = doc["stack"].to<JsonObject>();
  for (TaskHandle_t t : tarefas) stack[pcTaskGetName(t)] = uxTaskGetStackHighWaterMark(t);
  publicarJson(TOPICO("diag"), doc, false);
}

void TaskRede(void* pv) {
  EstadoRede rede = REDE_INICIO;
  uint32_t espera = BACKOFF_MIN_MS;
  TickType_t inicioWifi = 0;

  mqtt.setServer(MQTT_HOST, MQTT_PORT);
  mqtt.setCallback(aoReceberComando);
  mqtt.setBufferSize(512);
  mqtt.setKeepAlive(30);
  mqtt.setSocketTimeout(5);   // padrao e 15 s: travaria a task por muito tempo

  for (;;) {
    switch (rede) {
      case REDE_INICIO:
        logMsg("[REDE] conectando ao Wi-Fi %s...", WIFI_SSID);
        WiFi.mode(WIFI_STA);
        WiFi.setAutoReconnect(true);
        WiFi.begin(WIFI_SSID, WIFI_PASS, WIFI_CANAL);
        inicioWifi = xTaskGetTickCount();
        rede = REDE_WIFI;
        break;

      case REDE_WIFI:
        if (WiFi.status() == WL_CONNECTED) {
          logMsg("[REDE] Wi-Fi ok, IP %s", WiFi.localIP().toString().c_str());
          configTime(FUSO_S, 0, "pool.ntp.org", "time.google.com");
          espera = BACKOFF_MIN_MS;
          rede = REDE_MQTT;
        } else if (xTaskGetTickCount() - inicioWifi > pdMS_TO_TICKS(WIFI_TIMEOUT_MS)) {
          logMsg("[REDE] Wi-Fi nao respondeu, nova tentativa em %u s", (unsigned)(espera / 1000));
          WiFi.disconnect();
          vTaskDelay(pdMS_TO_TICKS(espera));
          espera = min(espera * 2, (uint32_t)BACKOFF_MAX_MS);
          rede = REDE_INICIO;
        }
        break;

      case REDE_MQTT:
        if (WiFi.status() != WL_CONNECTED) { inicioWifi = xTaskGetTickCount(); rede = REDE_WIFI; break; }
        logMsg("[REDE] conectando ao broker %s...", MQTT_HOST);
        if (conectarMqtt()) {
          logMsg("[REDE] MQTT ok");
          espera = BACKOFF_MIN_MS;
          enviarTelemetria(true);   // reenvia o estado completo apos (re)conectar
          rede = REDE_ONLINE;
        } else {
          logMsg("[REDE] MQTT falhou (rc=%d), nova tentativa em %u s", mqtt.state(), (unsigned)(espera / 1000));
          vTaskDelay(pdMS_TO_TICKS(espera));
          espera = min(espera * 2, (uint32_t)BACKOFF_MAX_MS);
        }
        break;

      case REDE_ONLINE:
        if (WiFi.status() != WL_CONNECTED) {
          logMsg("[REDE] Wi-Fi caiu");
          inicioWifi = xTaskGetTickCount();
          rede = REDE_WIFI;
        } else if (!mqtt.loop() || !enviarEventos() || !enviarTelemetria(false)) {
          logMsg("[REDE] conexao MQTT perdida");
          rede = REDE_MQTT;
        } else {
          enviarDiagnostico();
        }
        break;
    }
    vTaskDelay(pdMS_TO_TICKS(20));
  }
}

// ================= Setup =================

void setup() {
  Serial.begin(115200);

  pinMode(PIN_TRIG, OUTPUT);
  pinMode(PIN_ECHO, INPUT);
  pinMode(PIN_IR, INPUT_PULLUP);
  pinMode(PIN_BTN, INPUT_PULLUP);
  pinMode(PIN_LED_VERDE, OUTPUT);
  pinMode(PIN_LED_AMARELO, OUTPUT);
  pinMode(PIN_LED_VERMELHO, OUTPUT);
  // teste rapido dos LEDs ao ligar: verde, amarelo, vermelho
  for (int pino : { PIN_LED_VERDE, PIN_LED_AMARELO, PIN_LED_VERMELHO }) {
    digitalWrite(pino, HIGH);
    delay(300);
    digitalWrite(pino, LOW);
  }
  setLeds(NORMAL);

  // 1) Fila, semaforos e mutex ANTES das tasks e das interrupcoes
  filaLeituras = xQueueCreate(5, sizeof(float));
  filaEventos  = xQueueCreate(20, sizeof(Evento));
  semCheia     = xSemaphoreCreateBinary();
  semDescartes = xSemaphoreCreateCounting(50, 0);
  mutexLixeira = xSemaphoreCreateMutex();
  mutexSerial  = xSemaphoreCreateMutex();

  // 2) Tasks. A TaskRede e a ultima: o callback de comando usa hEsvaziar,
  //    que precisa existir antes de qualquer mensagem chegar.
  //    Rede no core 0 (junto com a pilha Wi-Fi); o resto no core 1.
  xTaskCreatePinnedToCore(TaskSensor,        "Sensor",    3072, NULL, 2, &tarefas[0], 1);
  xTaskCreatePinnedToCore(TaskProcessamento, "Process",   3072, NULL, 3, &tarefas[1], 1);
  xTaskCreatePinnedToCore(TaskAlerta,        "Alerta",    3072, NULL, 3, &tarefas[2], 1);
  xTaskCreatePinnedToCore(TaskDescartes,     "Descartes", 3072, NULL, 2, &tarefas[3], 1);
  xTaskCreatePinnedToCore(TaskEsvaziar,      "Esvaziar",  3072, NULL, 4, &hEsvaziar,  1);
  tarefas[4] = hEsvaziar;
  xTaskCreatePinnedToCore(TaskRede,          "Rede",      8192, NULL, 2, &tarefas[5], 0);

  // 3) Interrupcoes por ultimo
  attachInterrupt(digitalPinToInterrupt(PIN_IR),  isrIR,    FALLING);
  attachInterrupt(digitalPinToInterrupt(PIN_BTN), isrBotao, FALLING);

  logMsg("EcoBin %s iniciado", DEVICE_ID);
}

void loop() {
  vTaskDelete(NULL);  // tudo roda nas tasks
}
