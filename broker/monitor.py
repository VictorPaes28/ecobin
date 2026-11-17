"""
Monitor do EcoBin: mostra em portugues tudo o que passa pelo broker.

    monitor.bat            (ou: python monitor.py)

Usa o usuario "painel". A senha vem de credenciais.local.txt (se existir)
ou e pedida no inicio.
"""

import getpass
import json
import os
import sys
import time

import paho.mqtt.client as mqtt

PREFIXO = "ecobin/cesar-g7k2"
HOST, PORTA, USUARIO = "127.0.0.1", 1884, "painel"   # IPv4 direto: "localhost" pode ir para ::1, onde o broker nao escuta
ESTADOS = {"NORMAL": "normal", "ATENCAO": "ATENÇÃO", "CHEIA": "CHEIA"}
ORIGENS = {"botao": "pelo botão", "painel": "pelo painel", "automatica": "detectado sozinho"}


def ler_senha():
    arq = os.path.join(os.path.dirname(os.path.abspath(__file__)), "credenciais.local.txt")
    try:
        with open(arq, encoding="utf-8") as f:
            for linha in f:
                if linha.startswith(USUARIO + ":"):
                    return linha.split(":", 1)[1].strip()
    except OSError:
        pass
    return getpass.getpass(f'Senha do usuário "{USUARIO}": ')


def texto(sub, d, bruto):
    if sub == "status":
        return "ficou ONLINE" if bruto == "online" else "ficou OFFLINE"
    if not isinstance(d, dict):
        return None
    if sub == "nivel":
        estado = ESTADOS.get(d.get("estado"), d.get("estado"))
        s = f"nível {d.get('nivel_pct')}%  {estado}  (lixo a {d.get('dist_cm')} cm do sensor)"
        if d.get("sensor_ok") is False:
            s += "  [FALHA NO SENSOR]"
        if d.get("ir_ok") is False:
            s += "  [IR TRAVADO]"
        return s
    if sub == "descartes":
        return f"descartes desde a última coleta: {d.get('total')}"
    if sub == "alerta":
        return f"*** ALERTA: lixeira CHEIA ({d.get('nivel_pct')}%) ***"
    if sub == "evento":
        return (f"lixeira ESVAZIADA {ORIGENS.get(d.get('origem'), '')} "
                f"(estava em {d.get('nivel_antes')}%, {d.get('descartes')} descartes)")
    if sub == "cmd":
        partes = []
        if d.get("acao") == "reset":
            partes.append("marcar como esvaziada")
        if "limiar" in d:
            partes.append(f"limiar de cheia = {d['limiar']}%")
        return "<- COMANDO do painel: " + ", ".join(partes or ["?"])
    if sub == "info":
        return f"local: {d.get('local')}"
    if sub == "diag":
        return f"saúde da placa: memória livre {d.get('heap', 0) // 1024} KB"
    return None


def ao_conectar(cliente, userdata, flags, rc, props):
    if rc != 0:
        print(f"Broker recusou a conexão ({rc}). Senha errada?")
        cliente.disconnect()
        return
    print(f"Conectado ao broker {HOST}:{PORTA}. Aguardando mensagens... (Ctrl+C para sair)\n")
    cliente.subscribe(f"{PREFIXO}/#")


def ao_receber(cliente, userdata, msg):
    partes = msg.topic.split("/")
    if len(partes) != 4:
        return
    lixeira, sub = partes[2], partes[3]
    bruto = msg.payload.decode("utf-8", "replace")
    try:
        d = json.loads(bruto)
    except ValueError:
        d = None
    s = texto(sub, d, bruto)
    if s:
        guardada = "  (última mensagem guardada no broker)" if msg.retain else ""
        print(f"{time.strftime('%H:%M:%S')}  {lixeira}  {s}{guardada}", flush=True)


def main():
    if hasattr(sys.stdout, "reconfigure"):
        sys.stdout.reconfigure(encoding="utf-8")
    c = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2, client_id=f"ecobin-monitor-{os.getpid()}")
    c.username_pw_set(USUARIO, ler_senha())
    c.on_connect = ao_conectar
    c.on_message = ao_receber
    c.connect(HOST, PORTA, keepalive=30)
    try:
        c.loop_forever()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
