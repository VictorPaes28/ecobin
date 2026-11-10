"""
Simulador de lixeiras EcoBin.

Cria lixeiras virtuais que publicam no MQTT exatamente como o ESP32
(mesmos topicos e mesmo formato JSON), para testar o painel de gestao
com varias lixeiras sem precisar de mais hardware.

Uso:
    python simulador.py                    # 3 lixeiras, velocidade normal
    python simulador.py --velocidade 3     # enche 3x mais rapido
    python simulador.py --broker localhost # usar o Mosquitto local
"""

import argparse
import json
import random
import signal
import threading
import time

import paho.mqtt.client as mqtt

PREFIXO = "ecobin/cesar-g7k2"
ALTURA_CM = 40.0
DIST_CHEIA_CM = 5.0  # sensor ate a borda (igual ao config.h do ESP32)
NIVEL_ATENCAO = 50
HISTERESE = 10
INTERVALO_S = 2.0

# taxa = % de enchimento por minuto (acelerado para a demonstracao)
LIXEIRAS = [
    {"id": "lx02", "local": "Bloco A - Cantina", "taxa": 6.0},
    {"id": "lx03", "local": "Bloco B - Corredor", "taxa": 2.5},
    {"id": "lx04", "local": "Estacionamento", "taxa": 1.0},
]


def agora_ms():
    return int(time.time() * 1000)


class LixeiraVirtual:
    def __init__(self, cfg, broker, porta, velocidade):
        self.id = cfg["id"]
        self.local = cfg["local"]
        self.taxa = cfg["taxa"] * velocidade
        self.base = f"{PREFIXO}/{self.id}"
        self.nivel = random.uniform(10, 55)
        self.limiar = 80
        self.estado = "NORMAL"
        self.descartes = 0
        self.lock = threading.Lock()

        self.cliente = mqtt.Client(
            mqtt.CallbackAPIVersion.VERSION2,
            client_id=f"ecobin-sim-{self.id}-{random.randint(0, 0xFFFF):04x}",
        )
        self.cliente.will_set(f"{self.base}/status", "offline", qos=1, retain=True)
        self.cliente.on_connect = self._ao_conectar
        self.cliente.on_message = self._ao_receber
        self.cliente.connect_async(broker, porta, keepalive=30)
        self.cliente.loop_start()

    def _pub(self, sub, dados, retain=False):
        payload = dados if isinstance(dados, str) else json.dumps(dados)
        self.cliente.publish(f"{self.base}/{sub}", payload, retain=retain)

    def _ao_conectar(self, cliente, userdata, flags, reason_code, properties):
        print(f"[{self.id}] conectado ({reason_code})")
        with self.lock:
            self._pub("status", "online", retain=True)
            self._pub("info", {"local": self.local, "altura_cm": ALTURA_CM}, retain=True)
            self._pub("descartes", {"total": self.descartes}, retain=True)
        cliente.subscribe(f"{self.base}/cmd", qos=1)

    def _ao_receber(self, cliente, userdata, msg):
        try:
            cmd = json.loads(msg.payload)
        except ValueError:
            return
        if not isinstance(cmd, dict):
            return
        print(f"[{self.id}] comando: {cmd}")
        # uma excecao aqui mataria a thread de rede do paho: valida antes de usar
        limiar = cmd.get("limiar")
        if isinstance(limiar, bool) or not isinstance(limiar, (int, float)):
            limiar = None
        with self.lock:
            if cmd.get("acao") == "reset":
                self._pub("evento", {"tipo": "ESVAZIADA", "nivel_antes": round(self.nivel),
                                     "descartes": self.descartes, "origem": "painel", "ts": agora_ms()})
                self.nivel = random.uniform(0, 3)
                self.descartes = 0
                self._pub("descartes", {"total": 0}, retain=True)
            if limiar is not None:
                self.limiar = max(50, min(95, int(limiar)))

    def passo(self):
        with self.lock:
            # enchimento continuo + descartes aleatorios (cada um soma um pouco de lixo)
            self.nivel += self.taxa / 60 * INTERVALO_S * random.uniform(0.5, 1.5)
            if random.random() < self.taxa / 20:
                self.descartes += 1
                self.nivel += random.uniform(0.3, 1.5)
                self._pub("descartes", {"total": self.descartes}, retain=True)
            self.nivel = min(self.nivel, 100.0)
            nivel = round(self.nivel)

            anterior = self.estado
            if anterior == "CHEIA" and nivel >= self.limiar - HISTERESE:
                self.estado = "CHEIA"
            elif nivel >= self.limiar:
                self.estado = "CHEIA"
            elif nivel >= NIVEL_ATENCAO:
                self.estado = "ATENCAO"
            else:
                self.estado = "NORMAL"

            if self.estado == "CHEIA" and anterior != "CHEIA":
                self._pub("alerta", {"tipo": "CHEIA", "nivel_pct": nivel, "ts": agora_ms()})
                print(f"[{self.id}] ALERTA: cheia ({nivel}%)")

            dist = ALTURA_CM - self.nivel / 100 * (ALTURA_CM - DIST_CHEIA_CM)
            self._pub("nivel", {"dist_cm": round(dist, 1), "nivel_pct": nivel,
                                "estado": self.estado, "limiar": self.limiar,
                                "sensor_ok": True, "ts": agora_ms()}, retain=True)

    def desligar(self):
        # desligamento limpo nao dispara o LWT, entao avisa "offline" manualmente
        try:
            self.cliente.publish(f"{self.base}/status", "offline", qos=1, retain=True).wait_for_publish(2)
        except (RuntimeError, ValueError):
            pass  # ja estava desconectado: o LWT ja publicou "offline"
        self.cliente.disconnect()
        self.cliente.loop_stop()  # depois do disconnect, senao o pedido nao chega ao broker


def main():
    ap = argparse.ArgumentParser(description="Simulador de lixeiras EcoBin")
    ap.add_argument("--broker", default="broker.hivemq.com")
    ap.add_argument("--porta", type=int, default=1883)
    ap.add_argument("--velocidade", type=float, default=1.0,
                    help="multiplicador da velocidade de enchimento")
    args = ap.parse_args()

    lixeiras = [LixeiraVirtual(c, args.broker, args.porta, args.velocidade) for c in LIXEIRAS]
    rodando = threading.Event()
    rodando.set()
    signal.signal(signal.SIGINT, lambda *_: rodando.clear())

    print(f"Simulando {len(lixeiras)} lixeiras em {args.broker}. Ctrl+C para parar.")
    while rodando.is_set():
        for lx in lixeiras:
            lx.passo()
        time.sleep(INTERVALO_S)

    print("Encerrando...")
    for lx in lixeiras:
        lx.desligar()


if __name__ == "__main__":
    main()
