// Diagnostico do hardware do EcoBin (nao e o firmware do projeto).
// Gravar:  pio run -e diag -t upload    |   voltar ao EcoBin:  pio run -e esp32dev -t upload
// A cada 0,5 s mostra no Serial o estado cru de cada entrada, sem filtros.
#include <Arduino.h>
#include "config.h"

void setup() {
  Serial.begin(115200);
  pinMode(PIN_TRIG, OUTPUT);
  pinMode(PIN_ECHO, INPUT);
  pinMode(PIN_IR, INPUT_PULLUP);
  pinMode(PIN_BTN, INPUT_PULLUP);
  for (int p : { PIN_LED_VERDE, PIN_LED_AMARELO, PIN_LED_VERMELHO }) pinMode(p, OUTPUT);
  delay(500);
  Serial.println("\n[DIAG] inicio. IR/BTN: 0 = ativo (nivel baixo), 1 = repouso");
}

void loop() {
  int echoRepouso = digitalRead(PIN_ECHO);  // deve ser 0 antes do disparo

  digitalWrite(PIN_TRIG, LOW);  delayMicroseconds(2);
  digitalWrite(PIN_TRIG, HIGH); delayMicroseconds(10);
  digitalWrite(PIN_TRIG, LOW);

  unsigned long t0 = micros(), subiu = 0, desceu = 0;
  while (digitalRead(PIN_ECHO) == LOW)  { if (micros() - t0 > 30000) break; }
  if (digitalRead(PIN_ECHO) == HIGH) {
    subiu = micros();
    while (digitalRead(PIN_ECHO) == HIGH) { if (micros() - subiu > 30000) break; }
    desceu = micros();
  }

  Serial.printf("IR=%d BTN=%d | ECHO repouso=%d ", digitalRead(PIN_IR), digitalRead(PIN_BTN), echoRepouso);
  if (!subiu) {
    Serial.println("| SEM ECO (ECHO nunca subiu em 30 ms)");
  } else {
    unsigned long largura = desceu - subiu;
    Serial.printf("| atraso=%lu us largura=%lu us dist=%.1f cm\n",
                  subiu - t0, largura, largura * 0.0343f / 2.0f);
  }

  // LEDs piscam em sequencia para confirmar que cada um acende
  static int led = 0;
  const int leds[] = { PIN_LED_VERDE, PIN_LED_AMARELO, PIN_LED_VERMELHO };
  for (int i = 0; i < 3; i++) digitalWrite(leds[i], i == led);
  led = (led + 1) % 3;

  delay(500);
}
