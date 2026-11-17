# EcoBin: MVP simulado

Lixeira inteligente com ESP32 + FreeRTOS + MQTT, simulada no navegador antes da montagem física.

```
ESP32 (placa real) ──────┐   MQTT :1884                       ┌── Painel web (index.html)
  lx01                   ├──────────> Mosquitto local ──WS───>┤    fila de coleta, previsão,
Simulador Python ────────┘   <───── comandos (cmd) ──── :9001 ┘    histórico, comandos
  lx02, lx03, lx04                  (broker/, com usuário e senha)
```

A simulação no Wokwi não alcança o PC, então ela continua usando o broker público `broker.hivemq.com` (ver *Simulação*).

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
│   ├── backend/app.py             # Flask + SQLite: grava o histórico e serve o painel
│   ├── dashboard/index.html       # painel de gestão
│   └── simulador/simulador.py     # lixeiras virtuais lx02–lx04
├── broker/                        # Mosquitto local: configuração, ACL e scripts (senhas fora do Git)
├── schematics/                    # guia de montagem e imagens do circuito
└── docs/                          # relatório de check point, relatório CSBC e figuras
```

## Como rodar

### 0. Broker MQTT local (Mosquitto)
1. Instale o Mosquitto 2.x (https://mosquitto.org/download/ ou `winget install EclipseFoundation.Mosquitto`).
2. Na primeira vez, rode `broker/criar-senhas.bat` e escolha as senhas dos usuários `lixeira` (ESP32 e simulador) e `painel` (navegador). O arquivo `broker/senhas` fica fora do Git.
3. Rode `broker/iniciar.bat` e deixe a janela aberta. Ele mostra o IP do PC, que vai no `MQTT_HOST` do `segredos.h`.
4. Na primeira execução, o Windows pergunta se o Mosquitto pode usar a rede: permita em **redes privadas**.
5. Opcional: `broker/monitor.bat` abre uma janela que mostra em português cada mensagem que passa pelo broker (nível, alertas, comandos do painel).

| Porta | Uso |
|---|---|
| 1884 | MQTT (ESP32 e simulador) |
| 9001 | MQTT sobre WebSocket (painel no navegador) |

Sem login o broker recusa a conexão, e cada usuário só acessa os tópicos `ecobin/#` (arquivo `broker/acl`). As portas são 1884/9001 e não 1883 porque o serviço padrão do Mosquitto já ocupa a 1883 dentro do PC.

**Rede:** o ESP32 e o PC precisam estar na mesma rede, e ela não pode isolar os aparelhos entre si (alguns Wi-Fi públicos fazem isso). Se o ESP32 não conectar, use o hotspot do celular para os dois.

### 1. Firmware com PlatformIO (VS Code)
1. Instale o VS Code e a extensão **PlatformIO IDE**.
2. *File → Open Folder* → `esp32-esp8266/`. O PlatformIO baixa a plataforma ESP32 e as bibliotecas (PubSubClient, ArduinoJson) sozinho, a partir do `platformio.ini`.
3. **Placa real (`esp32dev`)**: copie `include/segredos.exemplo.h` para `include/segredos.h`, preencha a rede (2,4 GHz), o IP do PC com o broker, a porta 1884 e a senha do usuário `lixeira`, conecte o ESP32 no USB e use **Upload** e depois **Monitor** na barra do PlatformIO.

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

Na simulação, o Serial deve mostrar `[REDE] Wi-Fi ok` e `[REDE] MQTT ok`. O Wokwi usa o broker público; para ver a simulação no painel, abra `index.html?broker=wss://broker.hivemq.com:8884/mqtt&usuario=`.

### 3. Simulador das outras lixeiras
```bash
cd applications/simulador
python -m venv .venv
.venv\Scripts\activate          # Linux/Mac: source .venv/bin/activate
pip install -r requirements.txt
python simulador.py --senha SENHA_DA_LIXEIRA --velocidade 3
```
Por padrão ele usa o broker local (`localhost:1884`). Para o broker público: `--broker broker.hivemq.com --porta 1883 --usuario ""`.

### 4. Back-end (histórico no banco de dados)
Duplo clique em `applications/backend/iniciar-backend.bat` (na primeira vez ele instala Flask e paho-mqtt). Deixe a janela aberta.

```
broker ──MQTT (usuário backend, só leitura)──> app.py ──> SQLite (ecobin.db) ──API HTTP :5000──> painel
```

| Rota | Devolve |
|---|---|
| `GET /` | o painel |
| `GET /api/saude` | back-end vivo, conectado ao broker, quantidade de registros |
| `GET /api/lixeiras` | estado atual de cada lixeira e a última coleta |
| `GET /api/lixeiras/<id>/leituras?horas=24` | histórico de nível |
| `GET /api/lixeiras/<id>/coletas` | coletas registradas (quando, nível, descartes, origem) |
| `GET /api/eventos` | alertas de cheia, coletas, lixeiras online/offline |

Tabelas: `lixeiras` (estado atual), `leituras` (nível ao longo do tempo) e `eventos`. Mensagens retidas e leituras com o sensor em falha não entram no histórico, para não distorcer a previsão. A senha do usuário `backend` vem de `broker/credenciais.local.txt` ou é pedida ao iniciar.

### 5. Painel
Com o back-end ligado, abra **http://localhost:5000/**. O painel carrega o histórico do banco (aparece "Histórico: banco de dados" no topo) e recebe o tempo real pelo broker. Sem o back-end, também dá para abrir `applications/dashboard/index.html` direto: o histórico fica só no navegador. Na primeira vez ele pede a senha do usuário `painel`. De outro computador da rede: `http://IP_DO_PC:5000/`.

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
| Sensor IR travado em "detectando" (sol, sujeira, sensibilidade alta) | Ativo por mais de 10 s (`IR_TRAVADO_MS`) = travado: o IR é ignorado, o nível continua sendo medido e o painel mostra "IR travado" |

Para conferir o hardware sem filtros, há um firmware de diagnóstico (`diag/diag.cpp`) que mostra no Serial o valor cru de cada sensor: `pio run -e diag -t upload` e depois `pio device monitor`. Para voltar ao EcoBin: `pio run -e esp32dev -t upload`.
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
- `setSocketTimeout(3)`: a PubSubClient espera a resposta do broker num laço ocupado; com 5 s ou mais, o watchdog do núcleo 0 reinicia a placa (visto no teste físico de 17/11).
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
- O histórico vem do back-end (SQLite) quando ele está ligado; sem ele, fica no `localStorage` do navegador (sobrevive ao F5). Sem os dois (aba anônima sem back-end), o painel funciona só em memória.
- **Validação**: como o broker é público, todo valor recebido é checado (tipo, faixa, estado conhecido, ID com até 20 caracteres `A-Z a-z 0-9 _ -`). Textos são exibidos como texto, nunca como HTML.

## Limitações conhecidas (MVP)
- **Sem TLS no broker local**: usuário e senha trafegam sem criptografia na rede local. Suficiente para o protótipo; em produção, usar TLS (porta 8883). A simulação no Wokwi ainda usa o broker público, aberto a qualquer um.
- **PubSubClient só publica com QoS 0**: um alerta pode se perder se a conexão cair exatamente no envio. Para QoS 1, trocar pela biblioteca `espMqttClient`.
- **Latência no Wokwi**: o relógio simulado anda mais devagar que o real, então a latência medida com o Wokwi não vale; com o simulador Python ou o ESP32 real, sim.
- **IR simulado por botão**: no circuito real, o módulo IR liga no mesmo GPIO27.
- **ECHO do HC-SR04**: no real, divisor de tensão (1 kΩ + 2 kΩ).
- Para o hardware real: ambiente `esp32dev` do PlatformIO + `include/segredos.h` preenchido (rede e IP do broker).
