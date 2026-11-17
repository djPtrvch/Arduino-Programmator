// MIDI-контроллер: 16 потенциометров через CD74HC4067 -> USB-MIDI Control Change.
//
// Подключение CD74HC4067 (модуль-плата):
//   S0 -> D2   S1 -> D3   S2 -> D4   S3 -> D5
//   SIG -> A0  EN -> GND  VCC -> 5V  GND -> GND
// Потенциометры (B10K): крайние ножки -> 5V и GND (у всех одинаково), средняя -> C0..C15.
// Потенциометр на C0 = CC 20, C1 = CC 21, ... C15 = CC 35.
// Неиспользуемые входы C* соедините с GND, иначе они «шумят» и шлют случайные CC.
//
// Больше потенциометров: второй CD74HC4067 подключается к тем же S0..S3,
// его SIG -> A1, и в MUX_SIG добавляется A1 (получится 32 потенциометра, CC 20..51).
#include <MIDIUSB.h>

const uint8_t MIDI_CHANNEL = 1;          // 1..16
const uint8_t CC_FIRST = 20;             // номер CC первого потенциометра
const uint8_t SELECT_PINS[4] = {2, 3, 4, 5};  // S0..S3
const uint8_t MUX_SIG[] = {A0};          // по одному аналоговому входу на мультиплексор
const uint8_t NUM_MUX = sizeof(MUX_SIG);
const uint8_t NUM_POTS = NUM_MUX * 16;

// Защита от дрожания: значение CC меняется, только если ручка ушла
// за границы текущего шага больше чем на MARGIN единиц АЦП (0..1023, шаг CC = 8).
const int MARGIN = 4;
const uint8_t OVERSAMPLE = 4;  // чтений на одно измерение (усреднение)

int32_t filt[NUM_POTS];   // сглаженное значение * 16
int16_t lastCc[NUM_POTS];

void selectChannel(uint8_t ch) {
  for (uint8_t b = 0; b < 4; b++) digitalWrite(SELECT_PINS[b], (ch >> b) & 1);
  delayMicroseconds(10);  // даём мультиплексору и входу АЦП установиться
}

int readPot(uint8_t index) {
  selectChannel(index % 16);
  uint8_t pin = MUX_SIG[index / 16];
  analogRead(pin);  // холостое чтение после переключения
  uint16_t sum = 0;
  for (uint8_t k = 0; k < OVERSAMPLE; k++) sum += analogRead(pin);
  return sum / OVERSAMPLE;
}

void sendCC(uint8_t cc, uint8_t value) {
  midiEventPacket_t ev = {0x0B, (uint8_t)(0xB0 | (MIDI_CHANNEL - 1)), cc, value};
  MidiUSB.sendMIDI(ev);
}

void setup() {
  for (uint8_t b = 0; b < 4; b++) pinMode(SELECT_PINS[b], OUTPUT);
  for (uint8_t i = 0; i < NUM_POTS; i++) {
    filt[i] = (int32_t)readPot(i) * 16;
    lastCc[i] = -1;
  }
}

void loop() {
  bool sent = false;
  for (uint8_t i = 0; i < NUM_POTS; i++) {
    filt[i] += ((int32_t)readPot(i) * 16 - filt[i]) / 8;  // экспоненциальное сглаживание
    int v = filt[i] / 16;                                // 0..1023
    int lo = lastCc[i] * 8 - MARGIN;
    int hi = lastCc[i] * 8 + 7 + MARGIN;
    if (lastCc[i] < 0 || v < lo || v > hi) {
      int cc = v >> 3;  // 0..127
      if (cc != lastCc[i]) {
        sendCC(CC_FIRST + i, cc);
        lastCc[i] = cc;
        sent = true;
      }
    }
  }
  if (sent) MidiUSB.flush();
}
