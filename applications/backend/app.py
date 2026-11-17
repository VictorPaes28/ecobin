"""
Back-end do EcoBin: guarda no SQLite tudo o que passa pelo broker e entrega o
historico para o painel por uma API HTTP.

    python app.py --senha SENHA_DO_BACKEND        (ou variavel ECOBIN_SENHA_BACKEND)

    MQTT (assina ecobin/cesar-g7k2/#)  ->  SQLite (ecobin.db)  ->  API HTTP :5000  ->  painel

Rotas:
    GET /                               painel (applications/dashboard/index.html)
    GET /api/saude                      back-end vivo? conectado ao broker? quantos registros?
    GET /api/lixeiras                   estado atual de cada lixeira
    GET /api/lixeiras/<id>/leituras     historico de nivel  (?horas=24)
    GET /api/lixeiras/<id>/coletas      coletas registradas (?limite=50)
    GET /api/eventos                    alertas, coletas e quedas (?limite=100)
"""

import argparse
import json
import os
import re
import sqlite3
import threading
import time
import urllib.parse
import urllib.request

import paho.mqtt.client as mqtt
from flask import Flask, abort, jsonify, request, send_from_directory

PREFIXO = "ecobin/cesar-g7k2"
ID_OK = re.compile(r"^[A-Za-z0-9_-]{1,32}$")
PASTA = os.path.dirname(os.path.abspath(__file__))
PASTA_PAINEL = os.path.join(PASTA, "..", "dashboard")

ESQUEMA = """
CREATE TABLE IF NOT EXISTS lixeiras (
    id         TEXT PRIMARY KEY,
    local      TEXT,
    nivel      INTEGER,
    estado     TEXT,
    limiar     INTEGER,
    dist_cm    REAL,
    sensor_ok  INTEGER,
    ir_ok      INTEGER,
    descartes  INTEGER DEFAULT 0,
    online     INTEGER DEFAULT 0,
    atualizado INTEGER            -- ms desde 1970
);
CREATE TABLE IF NOT EXISTS leituras (
    lixeira TEXT    NOT NULL,
    t       INTEGER NOT NULL,     -- ms desde 1970 (hora em que o back-end recebeu)
    nivel   INTEGER NOT NULL,
    limiar  INTEGER,
    dist_cm REAL,
    estado  TEXT
);
CREATE INDEX IF NOT EXISTS ix_leituras ON leituras (lixeira, t);
CREATE TABLE IF NOT EXISTS eventos (
    lixeira   TEXT    NOT NULL,
    t         INTEGER NOT NULL,
    tipo      TEXT    NOT NULL,   -- CHEIA, ESVAZIADA, ONLINE, OFFLINE
    nivel     INTEGER,
    descartes INTEGER,
    origem    TEXT
);
CREATE INDEX IF NOT EXISTS ix_eventos ON eventos (lixeira, t);
"""


class Banco:
    """SQLite compartilhado entre a thread do MQTT e as requisicoes HTTP (um lock para tudo)."""

    def __init__(self, caminho):
        self.con = sqlite3.connect(caminho, check_same_thread=False)
        self.con.row_factory = sqlite3.Row
        self.lock = threading.Lock()
        with self.lock:
            self.con.executescript(ESQUEMA)

    def executar(self, sql, args=()):
        with self.lock, self.con:
            self.con.execute(sql, args)

    def consultar(self, sql, args=()):
        with self.lock:
            return [dict(r) for r in self.con.execute(sql, args).fetchall()]


def agora_ms():
    return int(time.time() * 1000)


def inteiro(v, lo, hi):
    return v if isinstance(v, int) and not isinstance(v, bool) and lo <= v <= hi else None


def numero(v, lo, hi):
    return float(v) if isinstance(v, (int, float)) and not isinstance(v, bool) and lo <= v <= hi else None


class Telegram:
    """Alerta opcional (bonus). Ligado so se TELEGRAM_TOKEN e TELEGRAM_CHAT_ID estiverem definidos.
    O envio roda numa thread propria: a internet lenta nao atrasa a gravacao no banco."""

    def __init__(self, token, chat_id):
        self.token, self.chat_id = token, chat_id

    @property
    def ligado(self):
        return bool(self.token and self.chat_id)

    def enviar(self, texto):
        if self.ligado:
            threading.Thread(target=self._enviar, args=(texto,), daemon=True).start()

    def _enviar(self, texto):
        url = f"https://api.telegram.org/bot{self.token}/sendMessage"
        dados = urllib.parse.urlencode({"chat_id": self.chat_id, "text": texto}).encode()
        try:
            with urllib.request.urlopen(url, data=dados, timeout=10) as r:
                r.read()
        except OSError as e:   # sem internet, token errado...: o resto do sistema segue
            print(f"[TELEGRAM] falha ao enviar: {e}")


def local_de(db, lx):
    r = db.consultar("SELECT local FROM lixeiras WHERE id = ?", (lx,))
    return f" ({r[0]['local']})" if r and r[0]["local"] else ""


def garantir_lixeira(db, lx):
    db.executar("INSERT OR IGNORE INTO lixeiras (id) VALUES (?)", (lx,))


def processar(db, topico, payload, retida, telegram=None):
    """Uma mensagem do broker -> banco. Tudo que vem da rede e validado antes de gravar."""
    partes = topico.split("/")
    if len(partes) != 4 or "/".join(partes[:2]) != PREFIXO:
        return
    lx, sub = partes[2], partes[3]
    if not ID_OK.match(lx) or sub in ("cmd", "diag"):
        return
    texto = payload.decode("utf-8", "replace")
    t = agora_ms()
    garantir_lixeira(db, lx)

    if sub == "status":
        online = texto == "online"
        db.executar("UPDATE lixeiras SET online = ?, atualizado = ? WHERE id = ?", (int(online), t, lx))
        if not retida:
            db.executar("INSERT INTO eventos (lixeira, t, tipo) VALUES (?, ?, ?)",
                        (lx, t, "ONLINE" if online else "OFFLINE"))
        return

    try:
        d = json.loads(texto)
    except ValueError:
        return
    if not isinstance(d, dict):
        return

    if sub == "info":
        local = d.get("local")
        if isinstance(local, str):
            db.executar("UPDATE lixeiras SET local = ? WHERE id = ?", (local[:60], lx))
    elif sub == "nivel":
        nivel, limiar = inteiro(d.get("nivel_pct"), 0, 100), inteiro(d.get("limiar"), 0, 100)
        estado = d.get("estado") if d.get("estado") in ("NORMAL", "ATENCAO", "CHEIA") else None
        if nivel is None or limiar is None or estado is None:
            return
        dist = numero(d.get("dist_cm"), 0, 500)
        sensor_ok, ir_ok = d.get("sensor_ok") is not False, d.get("ir_ok") is not False
        db.executar("""UPDATE lixeiras SET nivel = ?, estado = ?, limiar = ?, dist_cm = ?,
                       sensor_ok = ?, ir_ok = ?, atualizado = ? WHERE id = ?""",
                    (nivel, estado, limiar, dist, int(sensor_ok), int(ir_ok), t, lx))
        # Retida = valor antigo guardado pelo broker; com sensor em falha o valor esta parado.
        # Nenhum dos dois entra no historico (mesma regra do painel).
        if not retida and sensor_ok:
            db.executar("INSERT INTO leituras (lixeira, t, nivel, limiar, dist_cm, estado) VALUES (?, ?, ?, ?, ?, ?)",
                        (lx, t, nivel, limiar, dist, estado))
    elif sub == "descartes":
        total = inteiro(d.get("total"), 0, 10**9)
        if total is not None:
            db.executar("UPDATE lixeiras SET descartes = ? WHERE id = ?", (total, lx))
    elif sub == "alerta" and not retida:
        nivel = inteiro(d.get("nivel_pct"), 0, 100)
        db.executar("INSERT INTO eventos (lixeira, t, tipo, nivel) VALUES (?, ?, 'CHEIA', ?)", (lx, t, nivel))
        if telegram:
            telegram.enviar(f"EcoBin: a lixeira {lx}{local_de(db, lx)} está CHEIA ({nivel}%). Programe a coleta.")
    elif sub == "evento" and d.get("tipo") == "ESVAZIADA" and not retida:
        origem = d.get("origem") if d.get("origem") in ("botao", "painel", "automatica") else None
        db.executar("INSERT INTO eventos (lixeira, t, tipo, nivel, descartes, origem) VALUES (?, ?, 'ESVAZIADA', ?, ?, ?)",
                    (lx, t, inteiro(d.get("nivel_antes"), 0, 100), inteiro(d.get("descartes"), 0, 10**9), origem))


def criar_mqtt(db, args, estado, telegram=None):
    c = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2, client_id=f"ecobin-backend-{os.getpid()}")
    if args.usuario:
        c.username_pw_set(args.usuario, args.senha)

    def ao_conectar(cliente, userdata, flags, rc, props):
        estado["mqtt"] = not rc.is_failure
        if rc.is_failure:
            print(f"[MQTT] broker recusou a conexao: {rc}")
            return
        print(f"[MQTT] conectado a {args.broker}:{args.porta}")
        cliente.subscribe(f"{PREFIXO}/#", qos=1)

    def ao_desconectar(cliente, userdata, flags, rc, props):
        estado["mqtt"] = False
        print(f"[MQTT] desconectado ({rc}); reconectando...")

    def ao_receber(cliente, userdata, msg):
        try:
            processar(db, msg.topic, msg.payload, msg.retain, telegram)
        except sqlite3.Error as e:
            print(f"[BANCO] erro ao gravar {msg.topic}: {e}")

    c.on_connect, c.on_disconnect, c.on_message = ao_conectar, ao_desconectar, ao_receber
    c.reconnect_delay_set(1, 30)
    c.connect_async(args.broker, args.porta, keepalive=30)
    c.loop_start()
    return c


def criar_app(db, estado):
    app = Flask(__name__)

    @app.after_request
    def cors(resp):
        # Leitura apenas: deixa o painel aberto direto do arquivo (file://) consultar a API
        resp.headers["Access-Control-Allow-Origin"] = "*"
        return resp

    def lixeira_ou_404(lx):
        if not ID_OK.match(lx) or not db.consultar("SELECT 1 FROM lixeiras WHERE id = ?", (lx,)):
            abort(404)

    @app.get("/")
    def painel():
        return send_from_directory(PASTA_PAINEL, "index.html")

    @app.get("/api/saude")
    def saude():
        n = db.consultar("SELECT (SELECT COUNT(*) FROM leituras) AS leituras, (SELECT COUNT(*) FROM eventos) AS eventos")[0]
        return jsonify(ok=True, mqtt=estado["mqtt"], **n)

    @app.get("/api/lixeiras")
    def lixeiras():
        linhas = db.consultar("SELECT * FROM lixeiras ORDER BY id")
        for r in linhas:
            for k in ("sensor_ok", "ir_ok", "online"):
                r[k] = bool(r[k]) if r[k] is not None else None
            ultima = db.consultar("SELECT MAX(t) AS t FROM eventos WHERE lixeira = ? AND tipo = 'ESVAZIADA'", (r["id"],))
            r["ultima_coleta"] = ultima[0]["t"]
        return jsonify(linhas)

    @app.get("/api/lixeiras/<lx>/leituras")
    def leituras(lx):
        lixeira_ou_404(lx)
        horas = min(max(request.args.get("horas", 24, type=float), 0.01), 24 * 30)
        desde = agora_ms() - int(horas * 3600 * 1000)
        return jsonify(db.consultar(
            "SELECT t, nivel, limiar, dist_cm, estado FROM leituras WHERE lixeira = ? AND t >= ? ORDER BY t",
            (lx, desde)))

    @app.get("/api/lixeiras/<lx>/coletas")
    def coletas(lx):
        lixeira_ou_404(lx)
        limite = min(max(request.args.get("limite", 50, type=int), 1), 500)
        return jsonify(db.consultar(
            """SELECT t, nivel, descartes, origem FROM eventos
               WHERE lixeira = ? AND tipo = 'ESVAZIADA' ORDER BY t DESC LIMIT ?""", (lx, limite)))

    @app.get("/api/eventos")
    def eventos():
        limite = min(max(request.args.get("limite", 100, type=int), 1), 1000)
        return jsonify(db.consultar("SELECT * FROM eventos ORDER BY t DESC LIMIT ?", (limite,)))

    return app


def main():
    ap = argparse.ArgumentParser(description="Back-end do EcoBin (MQTT -> SQLite -> API HTTP)")
    ap.add_argument("--broker", default="127.0.0.1")
    ap.add_argument("--porta", type=int, default=1884)
    ap.add_argument("--usuario", default="backend")
    ap.add_argument("--senha", default=os.environ.get("ECOBIN_SENHA_BACKEND"))
    ap.add_argument("--banco", default=os.path.join(PASTA, "ecobin.db"))
    ap.add_argument("--http", type=int, default=5000, help="porta da API e do painel")
    args = ap.parse_args()
    if args.usuario and not args.senha:
        ap.error("informe --senha (ou a variavel ECOBIN_SENHA_BACKEND)")

    db = Banco(args.banco)
    estado = {"mqtt": False}
    telegram = Telegram(os.environ.get("TELEGRAM_TOKEN"), os.environ.get("TELEGRAM_CHAT_ID"))
    print("[TELEGRAM] alertas " + ("ligados" if telegram.ligado else
          "desligados (defina TELEGRAM_TOKEN e TELEGRAM_CHAT_ID para ligar)"))
    criar_mqtt(db, args, estado, telegram)
    print(f"[HTTP] painel e API em http://localhost:{args.http}/  (banco: {args.banco})")
    # 0.0.0.0: outros computadores da rede tambem abrem o painel pelo IP deste PC
    criar_app(db, estado).run(host="0.0.0.0", port=args.http, threaded=True)


if __name__ == "__main__":
    main()
