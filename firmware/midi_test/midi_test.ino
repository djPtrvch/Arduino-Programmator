// MIDI-тест: 2 потенциометра -> USB-MIDI Control Change.
//
// Подключение (макетная плата):
//   Потенциометр 1: левая ножка -> GND, правая -> 5V, средняя -> A0   (CC 20)
//   Потенциометр 2: левая ножка -> GND, правая -> 5V, средняя -> A1   (CC 21)
// Подойдут потенциометры 10 кОм линейные (B10K).
//
// Плата видна как USB-MIDI устройство "Arduino Leonardo" — драйверы не нужны
// (Android, iPad, Windows, macOS).
#include <MIDIUSB.h>

const uint8_t MIDI_CHANNEL = 1;   // 1..16
const uint8_t CC_FIRST = 20;      // номер CC первого потенциометра
const uint8_t POTS[] = {A0, A1};
const uint8_t NUM_POTS = sizeof(POTS);

// Сглаживание и защита от «дребезга» значений
const int HYSTERESIS = 6;  // в единицах АЦП (0..1023)

int filtered[NUM_POTS];
int lastSentRaw[NUM_POTS];
int8_t lastSentCc[NUM_POTS];

void sendCC(uint8_t cc, uint8_t value) {
  midiEventPacket_t ev = {0x0B, (uint8_t)(0xB0 | (MIDI_CHANNEL - 1)), cc, value};
  MidiUSB.sendMIDI(ev);
  MidiUSB.flush();
}

void setup() {
  for (uint8_t i = 0; i < NUM_POTS; i++) {
    filtered[i] = analogRead(POTS[i]);
    lastSentRaw[i] = -100;
    lastSentCc[i] = -1;
  }
}

void loop() {
  for (uint8_t i = 0; i < NUM_POTS; i++) {
    int raw = analogRead(POTS[i]);
    filtered[i] += (raw - filtered[i]) / 4;  // простое экспоненциальное сглаживание
    int cc = filtered[i] >> 3;               // 0..1023 -> 0..127
    if (cc != lastSentCc[i] && abs(filtered[i] - lastSentRaw[i]) >= HYSTERESIS) {
      sendCC(CC_FIRST + i, cc);
      lastSentCc[i] = cc;
      lastSentRaw[i] = filtered[i];
    }
  }
  delay(2);
}
