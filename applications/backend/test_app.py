"""
Testes automaticos do back-end do EcoBin.

    pip install pytest                                   (uma vez)
    python -m pytest applications/backend/test_app.py -v

Nao precisam de broker nem da placa: as mensagens MQTT sao entregues direto para
processar(), que e a mesma funcao chamada quando chega mensagem de verdade.
Cada teste usa um banco SQLite novo numa pasta temporaria (o ecobin.db nao e tocado).
"""

import json
import os
import sys

import pytest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import app as backend  # noqa: E402

PREFIXO = backend.PREFIXO


@pytest.fixture
def db(tmp_path):
    return backend.Banco(str(tmp_path / "teste.db"))


@pytest.fixture
def cliente(db):
    flask_app = backend.criar_app(db, {"mqtt": True})
    flask_app.testing = True
    return flask_app.test_client()


def publicar(db, lixeira, assunto, dados, retida=False):
    """Simula a chegada de uma mensagem do broker."""
    payload = dados if isinstance(dados, (str, bytes)) else json.dumps(dados)
    if isinstance(payload, str):
        payload = payload.encode()
    backend.processar(db, f"{PREFIXO}/{lixeira}/{assunto}", payload, retida)


def nivel(pct, limiar=80, estado="NORMAL", sensor_ok=True, **extra):
    return {"dist_cm": 20.0, "nivel_pct": pct, "estado": estado, "limiar": limiar,
            "sensor_ok": sensor_ok, "ts": 0, **extra}


# ---------- Rotas basicas ----------

def test_saude_responde(cliente):
    r = cliente.get("/api/saude")
    assert r.status_code == 200
    assert r.get_json() == {"ok": True, "mqtt": True, "leituras": 0, "eventos": 0}


def test_lixeira_desconhecida_da_404(cliente):
    assert cliente.get("/api/lixeiras/lx99/leituras").status_code == 404
    assert cliente.get("/api/lixeiras/lx99/coletas").status_code == 404


# ---------- Leituras de nivel ----------

def test_leitura_aparece_no_estado_e_no_historico(db, cliente):
    publicar(db, "lx02", "info", {"local": "Bloco A - Cantina"})
    publicar(db, "lx02", "nivel", nivel(42))

    lx = cliente.get("/api/lixeiras").get_json()
    assert len(lx) == 1
    assert lx[0]["id"] == "lx02"
    assert lx[0]["nivel"] == 42
    assert lx[0]["local"] == "Bloco A - Cantina"
    assert lx[0]["sensor_ok"] is True

    leituras = cliente.get("/api/lixeiras/lx02/leituras").get_json()
    assert [p["nivel"] for p in leituras] == [42]
    assert leituras[0]["limiar"] == 80


def test_mensagem_retida_nao_entra_no_historico(db, cliente):
    # retida = ultimo valor guardado pelo broker, pode ser antigo
    publicar(db, "lx02", "nivel", nivel(70), retida=True)
    assert cliente.get("/api/lixeiras").get_json()[0]["nivel"] == 70   # estado atual atualiza
    assert cliente.get("/api/lixeiras/lx02/leituras").get_json() == []  # historico nao


def test_sensor_em_falha_nao_entra_no_historico(db, cliente):
    publicar(db, "lx02", "nivel", nivel(55, sensor_ok=False))
    lx = cliente.get("/api/lixeiras").get_json()[0]
    assert lx["sensor_ok"] is False
    assert cliente.get("/api/lixeiras/lx02/leituras").get_json() == []


def test_ir_travado_e_guardado(db, cliente):
    publicar(db, "lx01", "nivel", nivel(30, ir_ok=False))
    assert cliente.get("/api/lixeiras").get_json()[0]["ir_ok"] is False
    # o nivel continua sendo medido com o IR travado, entao entra no historico
    assert len(cliente.get("/api/lixeiras/lx01/leituras").get_json()) == 1


# ---------- Coletas ----------

@pytest.mark.parametrize("origem", ["botao", "painel", "automatica"])
def test_esvaziada_aparece_nas_coletas_com_a_origem(db, cliente, origem):
    publicar(db, "lx03", "evento", {"tipo": "ESVAZIADA", "nivel_antes": 88, "descartes": 7, "origem": origem})
    coletas = cliente.get("/api/lixeiras/lx03/coletas").get_json()
    assert len(coletas) == 1
    assert coletas[0]["nivel"] == 88
    assert coletas[0]["descartes"] == 7
    assert coletas[0]["origem"] == origem
    assert cliente.get("/api/lixeiras").get_json()[0]["ultima_coleta"] == coletas[0]["t"]


def test_origem_desconhecida_vira_nula(db, cliente):
    publicar(db, "lx03", "evento", {"tipo": "ESVAZIADA", "nivel_antes": 50, "descartes": 1, "origem": "hacker"})
    assert cliente.get("/api/lixeiras/lx03/coletas").get_json()[0]["origem"] is None


def test_esvaziada_retida_nao_conta(db, cliente):
    publicar(db, "lx03", "evento", {"tipo": "ESVAZIADA", "nivel_antes": 90, "descartes": 2, "origem": "botao"},
             retida=True)
    assert cliente.get("/api/lixeiras/lx03/coletas").get_json() == []


def test_alerta_e_status_viram_eventos(db, cliente):
    publicar(db, "lx02", "status", "online")
    publicar(db, "lx02", "alerta", {"tipo": "CHEIA", "nivel_pct": 85})
    publicar(db, "lx02", "status", "offline")
    tipos = [e["tipo"] for e in cliente.get("/api/eventos").get_json()]
    assert sorted(tipos) == ["CHEIA", "OFFLINE", "ONLINE"]
    assert cliente.get("/api/lixeiras").get_json()[0]["online"] is False


# ---------- Dados invalidos: ignorados sem derrubar o back-end ----------

@pytest.mark.parametrize("pct", [150, -5, "50", None, True, 12.5])
def test_nivel_fora_da_faixa_e_ignorado(db, cliente, pct):
    publicar(db, "lx02", "nivel", nivel(pct))
    assert cliente.get("/api/lixeiras/lx02/leituras").get_json() == []
    assert cliente.get("/api/saude").get_json()["leituras"] == 0


def test_estado_invalido_e_ignorado(db, cliente):
    publicar(db, "lx02", "nivel", nivel(40, estado="EXPLODIU"))
    assert cliente.get("/api/saude").get_json()["leituras"] == 0


@pytest.mark.parametrize("lixeira", ["lx 02", "lx02;DROP TABLE", "<script>", "a" * 40, "lx%02"])
def test_id_com_caracteres_estranhos_e_ignorado(db, cliente, lixeira):
    publicar(db, lixeira, "nivel", nivel(40))
    assert cliente.get("/api/lixeiras").get_json() == []


@pytest.mark.parametrize("payload", [b"isso nao e json", b"[1, 2, 3]", b"\xff\xfe\x00", b"", b"null"])
def test_payload_quebrado_nao_derruba(db, cliente, payload):
    publicar(db, "lx02", "nivel", payload)
    assert cliente.get("/api/saude").status_code == 200
    assert cliente.get("/api/lixeiras/lx02/leituras").get_json() == []


def test_topico_de_outro_prefixo_e_ignorado(db, cliente):
    backend.processar(db, "outro/grupo/lx02/nivel", json.dumps(nivel(40)).encode(), False)
    backend.processar(db, f"{PREFIXO}/lx02/nivel/extra", json.dumps(nivel(40)).encode(), False)
    assert cliente.get("/api/lixeiras").get_json() == []


def test_comandos_e_diagnostico_nao_sao_gravados(db, cliente):
    publicar(db, "lx01", "cmd", {"acao": "reset"})
    publicar(db, "lx01", "diag", {"heap": 1000})
    assert cliente.get("/api/lixeiras").get_json() == []


def test_local_muito_longo_e_cortado(db, cliente):
    publicar(db, "lx02", "info", {"local": "x" * 500})
    assert len(cliente.get("/api/lixeiras").get_json()[0]["local"]) == 60


# ---------- Parametros da API ----------

def test_parametros_absurdos_nao_quebram_a_api(db, cliente):
    publicar(db, "lx02", "nivel", nivel(40))
    for url in ("/api/lixeiras/lx02/leituras?horas=-10", "/api/lixeiras/lx02/leituras?horas=abc",
                "/api/lixeiras/lx02/leituras?horas=999999", "/api/lixeiras/lx02/coletas?limite=-1",
                "/api/lixeiras/lx02/coletas?limite=99999", "/api/eventos?limite=0"):
        assert cliente.get(url).status_code == 200, url


def test_limite_de_coletas_e_respeitado(db, cliente):
    for i in range(5):
        publicar(db, "lx03", "evento", {"tipo": "ESVAZIADA", "nivel_antes": 50 + i, "descartes": i, "origem": "botao"})
    assert len(cliente.get("/api/lixeiras/lx03/coletas?limite=3").get_json()) == 3
