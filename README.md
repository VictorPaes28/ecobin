# EcoBin: MVP simulado

Lixeira inteligente com ESP32 + FreeRTOS + MQTT, simulada no navegador antes da montagem física.

```
ESP32 simulado (Wokwi) ──┐                                   ┌── Painel web (index.html)
  lx01                   ├──MQTT──> broker.hivemq.com ──WS──>┤    fila de coleta, previsão,
Simulador Python ────────┘   <──────── comandos (cmd) ───────┘    histórico, comandos
  lx02, lx03, lx04
```

## Estrutura

```
ecobin/
├── esp32-esp8266/                 # projeto PlatformIO (firmware FreeRTOS)
│   ├── platformio.ini             # placa, bibliotecas e ambientes (esp32dev / wokwi)
│   ├── src/main.cpp               # firmware
│   ├── include/config.h           # pinos, geometria da lixeira, limites e tempos
│   ├── include/segredos.exemplo.h # modelo para segredos.h (Wi-Fi e broker, fora do Git)
│   ├── diagram.json               # circuito para o Wokwi
│   └── wokwi.toml                 # simulação do build do PlatformIO no VS Code
├── applications/
│   ├── dashboard/index.html       # painel de gestão (abre direto no navegador)
│   └── simulador/simulador.py     # lixeiras virtuais lx02–lx04
├── schematics/                    # guia de montagem e imagens do circuito
└── docs/                          # relatório de check point, relatório CSBC e figuras
```

## Como rodar

### 1. Firmware com PlatformIO (VS Code)
1. Instale o VS Code e a extensão **PlatformIO IDE**.
2. *File → Open Folder* → `esp32-esp8266/`. O PlatformIO baixa a plataforma ESP32 e as bibliotecas (PubSubClient, ArduinoJson) sozinho, a partir do `platformio.ini`.
3. **Placa real (`esp32dev`)**: copie `include/segredos.exemplo.h` para `include/segredos.h`, preencha a rede e o IP do broker, conecte o ESP32 no USB e use **Upload** e depois **Monitor** na barra do PlatformIO.

Pelo terminal do PlatformIO:
```bash
pio run -e esp32dev -t upload
```
```bash
pio device monitor
```

### 2. Simulação (sem placa)
- **No VS Code**: instale a extensão **Wokwi Simulator**, compile o ambiente `wokwi` (`pio run -e wokwi`) e rode *F1 → Wokwi: Start Simulator*. Usa o `diagram.json` e o `wokwi.toml` da pasta.
- **No navegador**: em https://wokwi.com/projects/new/esp32, cole `src/main.cpp` no `sketch.ino`, crie `config.h` com o conteúdo de `include/config.h`, cole o `diagram.json` e adicione **PubSubClient** e **ArduinoJson** no *Library Manager*.

Na simulação, o Serial deve mostrar `[REDE] Wi-Fi ok` e `[REDE] MQTT ok`.

### 2. Simulador das outras lixeiras
```bash
cd applications/simulador
python -m venv .venv
.venv\Scripts\activate          # Linux/Mac: source .venv/bin/activate
pip install -r requirements.txt
python simulador.py --velocidade 3
```

### 3. Painel
Abra `applications/dashboard/index.html` no navegador.

## Interação na simulação

| No Wokwi | Simula | Mecanismo |
|---|---|---|
| Clicar no HC-SR04 e mover a distância | lixo subindo/descendo | Fila (Sensor → Processamento) |
| Distância < ~10 cm | lixeira cheia → alerta | Semáforo binário |
| Botão azul (tecla `i`) | descarte detectado pelo IR | Semáforo contador |
| Botão vermelho (tecla `e`) | funcionário esvaziou | ISR + Task Notification |
| Esvaziar a lixeira (distância volta para perto de 40 cm) | coleta detectada sozinha | Task Notification (origem automática) |

No painel: clique numa lixeira da fila para ver o detalhe; **Marcar como esvaziada** e **Aplicar limiar** mandam comandos para ela.

## Como o volume é medido

```
 sensor ─▶  ┌───┐  ◀ tampa                 1. distância d = tempo do eco × velocidade do som / 2
            │   │  DIST_CHEIA_CM (5)          (velocidade corrigida para TEMPERATURA_C)
     topo   \   /  ◀ LARGURA_TOPO_CM       2. altura do lixo h = ALTURA_CM − d
             \ /   ALTURA_CM (40)          3. volume % = fração do volume da lixeira até h
     fundo   ───   ◀ LARGURA_FUNDO_CM
```

**Altura não é volume.** Lixeira comum é mais larga em cima, então a metade de baixo guarda menos lixo. Com a largura variando de `a` (fundo) a `b` (topo):

```
volume(h) / volume(total) = (w³ − a³) / (b³ − a³),   w = a + (b − a) · h / H,   H = ALTURA_CM − DIST_CHEIA_CM
```

Vale para seção quadrada ou redonda; com paredes retas (`a = b`) vira `h / H`. Conferido por integração numérica.

| Lixeira (fundo → topo) | Lixo na metade da altura = |
|---|---|
| reta, 25 → 25 cm | 50% do volume |
| balde, 20 → 30 cm | 40% do volume |
| cesto, 15 → 35 cm | 31% do volume |

### Filtros contra leitura errada
| Problema | Proteção |
|---|---|
| Mão ou objeto passando na frente do sensor durante o descarte | Ultrassônico pausado enquanto o IR está ativo e por 2 s depois (`PAUSA_DESCARTE_MS`) |
| Saco em pé, objeto caindo, pico isolado | CHEIA só após 3 leituras seguidas acima do limiar (`CONFIRMA_CHEIA`) |
| Ruído e ecos falsos | Mediana de 5 medições, exigindo pelo menos 3 válidas |
| Eco do ping anterior dentro da caixa | 60 ms entre medições (recomendação do HC-SR04) |
| Leitura maior que a lixeira (tampa aberta, eco desviado) | Descartada; se persistir, vira "falha no sensor" |
| Ficar oscilando entre cheia e não cheia | Histerese de 10% para sair de CHEIA |
| Temperatura (Recife ~28 °C × 20 °C padrão) | Velocidade do som corrigida: ~1,5% de erro a menos |

### Calibração na lixeira real
1. Com a lixeira **vazia** e o sensor no lugar, ler a distância no Serial → `ALTURA_CM`.
2. Medir do sensor até a borda → `DIST_CHEIA_CM` (pelo menos 4 cm; se for menor, subir o sensor).
3. Medir a largura (ou o diâmetro) no fundo e na borda → `LARGURA_FUNDO_CM` e `LARGURA_TOPO_CM`.
4. Validar: colocar objetos de altura conhecida e comparar. Isso vira a tabela de precisão do relatório.

### Limites físicos do ultrassônico (citar no relatório)
- **O sensor mede o ponto mais alto dentro do cone (~15°), não a média.** Lixo amontoado no centro faz parecer mais cheia; lixo encostado nas paredes, fora do cone, faz parecer mais vazia. Por isso trabalhos maiores usam vários sensores (ex.: 4 sensores, ~93,8% de precisão).
- **Materiais macios** (papel amassado, tecido, sacolas) absorvem o som: o eco pode falhar ou atravessar.
- **Caixas estreitas** fazem o som bater nas paredes. Para 40 cm de profundidade, usar recipiente com pelo menos ~20 cm de largura e o sensor centralizado e paralelo ao fundo.
- **Lixo encostando no sensor** (< 2 cm) não é medido: a leitura é descartada e aparece como falha do sensor. Por isso o sensor fica a ≥ 4 cm da borda.

## Arquitetura do firmware

```
TaskSensor ──[filaLeituras]──> TaskProcessamento ──(semCheia)──> TaskAlerta ──┐
ISR IR ─────(semDescartes)───> TaskDescartes                                  │
ISR botão ──(notification)───> TaskEsvaziar ──────────────────────────────────┴─[filaEventos]─> TaskRede ─> MQTT
Todas leem/escrevem o struct `lixeira` protegido por `mutexLixeira`
```

| Task | Prioridade | Core | Função |
|---|---|---|---|
| Rede | 2 | 0 | Única dona do Wi-Fi e do MQTT |
| Sensor | 2 | 1 | Mediana de 5 medições a cada 1 s |
| Processamento | 3 | 1 | Nível, estado (com histerese), semáforo de LEDs (verde D33, amarelo D26, vermelho D14) |
| Alerta | 3 | 1 | Gera o evento CHEIA |
| Descartes | 2 | 1 | Conta descartes |
| Esvaziar | 4 | 1 | Zera o contador e gera o evento ESVAZIADA |

### Estruturas de dados
- **`Lixeira`** — estado atual (distância, nível, estado, limiar, descartes) num único struct protegido por mutex. Quem precisa ler pega uma cópia (`lerLixeira()`).
- **`filaLeituras`** — só a distância (`float`), produtor → consumidor.
- **`filaEventos`** — eventos que **não podem se perder** (`CHEIA`, `ESVAZIADA`), com horário do momento em que aconteceram. Só saem da fila depois de publicados.
- O nível **não** passa por fila: a TaskRede publica uma cópia do estado atual, então nunca envia leitura velha acumulada.
- **Falha do sensor**: após 5 leituras seguidas sem eco (jumper solto, sensor queimado), `sensorOk` vira `false` e vai em `sensor_ok`. O painel mostra "Falha no sensor" em vez de repetir o último nível como se fosse atual. Também começa `false` até a primeira leitura válida.
- A TaskRede é criada por último: o callback de comandos usa a handle da TaskEsvaziar, que precisa existir antes de qualquer mensagem chegar.

### Conexão sem fio (TaskRede)
Máquina de estados `INICIO → WIFI → MQTT → ONLINE`:
- Timeout de 15 s no Wi-Fi; espera crescente entre tentativas (1, 2, 4… até 30 s).
- `setSocketTimeout(5)` evita que o `connect` trave a task por 15 s.
- **LWT** com QoS 1: se cair sem aviso, o broker publica `offline`.
- Ao reconectar, reenvia o estado completo e os eventos pendentes.
- Nível publicado só quando muda ou a cada 10 s (heartbeat), retido no broker.
- Horário via NTP, enviado em `ts` (ms) para medir latência.

## Tópicos MQTT

Prefixo: `ecobin/cesar-g7k2/<id>`

| Tópico | Direção | Retido | Exemplo |
|---|---|---|---|
| `/nivel` | ESP → | sim | `{"dist_cm":12.4,"nivel_pct":75,"estado":"ATENCAO","limiar":80,"sensor_ok":true,"ts":1760970000000}` |
| `/descartes` | ESP → | sim | `{"total":7}` |
| `/alerta` | ESP → | não | `{"tipo":"CHEIA","nivel_pct":84,"ts":…}` |
| `/evento` | ESP → | não | `{"tipo":"ESVAZIADA","nivel_antes":88,"descartes":7,"ts":…}` |
| `/status` | ESP → | sim | `online` / `offline` (LWT) |
| `/info` | ESP → | sim | `{"local":"Bloco A - Cantina","altura_cm":40}` |
| `/diag` | ESP → | não | `{"heap":215000,"stack":{"Rede":5000,…}}` (a cada 30 s) |
| `/cmd` | → ESP | não | `{"acao":"reset"}` ou `{"limiar":75}` (QoS 1) |

## Painel
- **Fila de coleta**: ordenada por prioridade (cheias primeiro, depois a que vai encher antes).
- **Previsão**: regressão linear do nível nos últimos 30 min (desde a última coleta) → velocidade em %/h e tempo até o limiar.
- **Histórico de coletas**, **eventos**, **diagnóstico de stack** e **notificações do navegador**.
- O histórico fica no `localStorage` do navegador (sobrevive ao F5). O banco de dados de verdade entra com o backend. Sem `localStorage` (aba anônima, pré-visualização), o painel funciona só em memória.
- **Validação**: como o broker é público, todo valor recebido é checado (tipo, faixa, estado conhecido, ID com até 20 caracteres `A-Z a-z 0-9 _ -`). Textos são exibidos como texto, nunca como HTML.

## Limitações conhecidas (MVP)
- **Broker público**: qualquer um pode ler e enviar comandos nos tópicos. No final: Mosquitto local com usuário/senha.
- **PubSubClient só publica com QoS 0**: um alerta pode se perder se a conexão cair exatamente no envio. Para QoS 1, trocar pela biblioteca `espMqttClient`.
- **Latência no Wokwi**: o relógio simulado anda mais devagar que o real, então a latência medida com o Wokwi não vale; com o simulador Python ou o ESP32 real, sim.
- **IR simulado por botão**: no circuito real, o módulo IR liga no mesmo GPIO27.
- **ECHO do HC-SR04**: no real, divisor de tensão (1 kΩ + 2 kΩ).
- Para o hardware real: ambiente `esp32dev` do PlatformIO + `include/segredos.h` preenchido (rede e IP do broker).
