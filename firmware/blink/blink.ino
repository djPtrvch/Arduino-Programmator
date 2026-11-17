// Мигание светодиодом для проверки прошивки.
// Leonardo/Uno/Nano/Mega: светодиод L (пин 13).
// Pro Micro: на пине 13 светодиода нет — мигает светодиод RX (жёлтый/зелёный у разъёма USB).
#ifndef LED_BUILTIN
#define LED_BUILTIN 2
#endif

void led(bool on) {
  digitalWrite(LED_BUILTIN, on);
#ifdef LED_BUILTIN_RX
  digitalWrite(LED_BUILTIN_RX, !on);  // RX-светодиод 32U4 включается низким уровнем
#endif
}

void setup() {
  pinMode(LED_BUILTIN, OUTPUT);
#ifdef LED_BUILTIN_RX
  pinMode(LED_BUILTIN_RX, OUTPUT);
#endif
}

void loop() {
  led(true);
  delay(200);
  led(false);
  delay(800);
}
