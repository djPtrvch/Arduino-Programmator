// MIDI-тест: 2 потенциометра -> USB-MIDI Control Change.
//
// Подключение (макетная плата):
//   Потенциометр 1: крайняя ножка -> GND, другая крайняя -> VCC, средняя -> A0   (CC 20)
//   Потенциометр 2: крайняя ножка -> GND, другая крайняя -> VCC, средняя -> A1   (CC 21)
// Подойдут потенциометры 10 кОм линейные (B10K).
//
// Плата видна как USB-MIDI устройство — драйверы не нужны (Android, iPad, Windows, macOS).
#include <MIDIUSB.h>

const uint8_t MIDI_CHANNEL = 1;   // 1..16
const uint8_t CC_FIRST = 20;      // номер CC первого потенциометра
const uint8_t POTS[] = {A0, A1};  // можно добавить A2, A3
const uint8_t NUM_POTS = sizeof(POTS);

// Защита от дрожания: значение CC меняется, только если ручка ушла
// за границы текущего шага больше чем на MARGIN единиц АЦП (0..1023, шаг CC = 8).
const int MARGIN = 4;
const uint8_t OVERSAMPLE = 8;  // чтений на одно измерение (усреднение)

int32_t filt[NUM_POTS];   // сглаженное значение * 16
int16_t lastCc[NUM_POTS];

int readPot(uint8_t pin) {
  analogRead(pin);  // холостое чтение: АЦП переключается на новый вход
  uint16_t sum = 0;
  for (uint8_t k = 0; k < OVERSAMPLE; k++) sum += analogRead(pin);
  return sum / OVERSAMPLE;
}

void sendCC(uint8_t cc, uint8_t value) {
  midiEventPacket_t ev = {0x0B, (uint8_t)(0xB0 | (MIDI_CHANNEL - 1)), cc, value};
  MidiUSB.sendMIDI(ev);
}

void setup() {
  for (uint8_t i = 0; i < NUM_POTS; i++) {
    filt[i] = (int32_t)readPot(POTS[i]) * 16;
    lastCc[i] = -1;
  }
}

void loop() {
  bool sent = false;
  for (uint8_t i = 0; i < NUM_POTS; i++) {
    filt[i] += ((int32_t)readPot(POTS[i]) * 16 - filt[i]) / 8;  // экспоненциальное сглаживание
    int v = filt[i] / 16;                                      // 0..1023
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
  delay(1);
}
