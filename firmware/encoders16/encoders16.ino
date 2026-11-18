// MIDI-контроллер: 16 энкодеров EC11 с кнопкой через 3 мультиплексора CD74HC4067.
// Настройки каждого энкодера хранятся в EEPROM и меняются конфигуратором
// (страница configurator/ на сайте) по USB-MIDI SysEx — без перепрошивки.
//
// Подключение (S0–S3 общие для всех трёх мультиплексоров):
//   S0 -> 2   S1 -> 3   S2 -> 4   S3 -> 5   EN -> GND   VCC -> VCC   GND -> GND
//   Мультиплексор 1 (выводы A энкодеров):      SIG -> 6
//   Мультиплексор 2 (выводы B энкодеров):      SIG -> 7
//   Мультиплексор 3 (кнопки энкодеров):        SIG -> 8
// Энкодер N (1..16) -> входы C(N-1) всех трёх мультиплексоров:
//   A -> C(N-1) мультиплексора 1, B -> C(N-1) мультиплексора 2,
//   одна ножка кнопки -> C(N-1) мультиплексора 3.
//   Средний вывод C энкодера и вторая ножка кнопки -> GND.
// Резисторы не нужны: на пинах 6, 7, 8 включена внутренняя подтяжка.
#include <MIDIUSB.h>
#include <EEPROM.h>

const uint8_t SELECT_PINS[4] = {2, 3, 4, 5};
const uint8_t PIN_A = 6, PIN_B = 7, PIN_SW = 8;
const uint8_t NUM_ENC = 16;
const uint8_t FW_VERSION = 1;

// ---------------------------------------------------------------- настройки

enum { ENC_ABSOLUTE = 0, ENC_REL_BINARY = 1, ENC_REL_TWOS = 2 };
enum { BTN_CC = 0, BTN_NOTE = 1, BTN_OFF = 2 };
enum { BTN_MOMENTARY = 0, BTN_TOGGLE = 1 };

struct EncConfig {
  uint8_t channel;     // 0..15
  uint8_t cc;          // 0..127
  uint8_t mode;        // ENC_*
  uint8_t accel;       // 0/1 — ускорение при быстром вращении
  uint8_t btnType;     // BTN_CC / BTN_NOTE / BTN_OFF
  uint8_t btnChannel;  // 0..15
  uint8_t btnNumber;   // 0..127
  uint8_t btnMode;     // BTN_MOMENTARY / BTN_TOGGLE
};

struct Config {
  uint8_t detentSteps;  // переходов A/B на один щелчок: 4, 2 или 1
  uint8_t reverse;      // 1 — инвертировать направление
  EncConfig enc[NUM_ENC];
};

Config cfg;
const uint16_t CFG_SIZE = sizeof(Config);  // 2 + 16 * 8 = 130
const int EEPROM_ADDR = 4;                  // [0..2] — метка, [3] — версия

void setDefaults() {
  cfg.detentSteps = 4;
  cfg.reverse = 0;
  for (uint8_t i = 0; i < NUM_ENC; i++) {
    cfg.enc[i] = {0, i, ENC_ABSOLUTE, 1, BTN_CC, 1, i, BTN_MOMENTARY};
  }
}

bool configValid(const Config &c) {
  if (c.detentSteps != 1 && c.detentSteps != 2 && c.detentSteps != 4) return false;
  if (c.reverse > 1) return false;
  for (uint8_t i = 0; i < NUM_ENC; i++) {
    const EncConfig &e = c.enc[i];
    if (e.channel > 15 || e.cc > 127 || e.mode > 2 || e.accel > 1) return false;
    if (e.btnType > 2 || e.btnChannel > 15 || e.btnNumber > 127 || e.btnMode > 1) return false;
  }
  return true;
}

void loadConfig() {
  if (EEPROM.read(0) == 'E' && EEPROM.read(1) == 'N' && EEPROM.read(2) == 'C' && EEPROM.read(3) == 1) {
    EEPROM.get(EEPROM_ADDR, cfg);
    if (configValid(cfg)) return;
  }
  setDefaults();
}

void saveConfig() {
  EEPROM.update(0, 'E');
  EEPROM.update(1, 'N');
  EEPROM.update(2, 'C');
  EEPROM.update(3, 1);
  EEPROM.put(EEPROM_ADDR, cfg);  // put пишет только изменившиеся байты
}

// ---------------------------------------------------------------- состояние

struct EncState {
  uint8_t ab;          // последнее состояние A/B
  int8_t acc;          // накопленные переходы до щелчка
  uint8_t value;       // текущее значение 0..127 (абсолютный режим)
  uint32_t lastStep;   // время последнего щелчка — для ускорения
  bool swRaw, swStable, toggled;
  uint32_t swChanged;
};

EncState st[NUM_ENC];

// ---------------------------------------------------------------- MIDI-вывод

bool midiPending = false;

void sendCC(uint8_t ch, uint8_t cc, uint8_t value) {
  midiEventPacket_t p = {0x0B, (uint8_t)(0xB0 | ch), cc, value};
  MidiUSB.sendMIDI(p);
  midiPending = true;
}

void sendNote(uint8_t ch, uint8_t note, bool on) {
  midiEventPacket_t p = {(uint8_t)(on ? 0x09 : 0x08), (uint8_t)((on ? 0x90 : 0x80) | ch), note,
                         (uint8_t)(on ? 127 : 0)};
  MidiUSB.sendMIDI(p);
  midiPending = true;
}

void sendSysex(const uint8_t *d, uint16_t len) {
  uint16_t i = 0;
  while (len - i > 3) {
    midiEventPacket_t p = {0x04, d[i], d[i + 1], d[i + 2]};
    MidiUSB.sendMIDI(p);
    i += 3;
  }
  uint8_t rest = len - i;
  midiEventPacket_t p = {0, 0, 0, 0};
  if (rest == 1) p = {0x05, d[i], 0, 0};
  else if (rest == 2) p = {0x06, d[i], d[i + 1], 0};
  else p = {0x07, d[i], d[i + 1], d[i + 2]};
  MidiUSB.sendMIDI(p);
  MidiUSB.flush();
}

// ---------------------------------------------------------------- энкодеры и кнопки

// Таблица квадратурного декодера: индекс = (старое AB << 2) | новое AB
const int8_t QDEC[16] = {0, -1, 1, 0, 1, 0, 0, -1, -1, 0, 0, 1, 0, 1, -1, 0};

void onDetent(uint8_t i, int8_t dir, uint32_t now) {
  const EncConfig &e = cfg.enc[i];
  EncState &s = st[i];
  if (cfg.reverse) dir = -dir;

  int8_t step = 1;
  if (e.accel) {
    uint32_t dt = now - s.lastStep;
    if (dt < 15) step = 6;
    else if (dt < 35) step = 3;
    else if (dt < 70) step = 2;
  }
  s.lastStep = now;
  int8_t delta = dir * step;

  if (e.mode == ENC_ABSOLUTE) {
    int16_t v = (int16_t)s.value + delta;
    if (v < 0) v = 0;
    if (v > 127) v = 127;
    if (v != s.value) {
      s.value = v;
      sendCC(e.channel, e.cc, v);
    }
  } else if (e.mode == ENC_REL_BINARY) {
    sendCC(e.channel, e.cc, 64 + delta);              // 65 = +1, 63 = −1
  } else {
    sendCC(e.channel, e.cc, delta > 0 ? delta : 128 + delta);  // 1 = +1, 127 = −1
  }
}

void updateEncoder(uint8_t i, uint8_t ab, uint32_t now) {
  EncState &s = st[i];
  if (ab == s.ab) return;
  s.acc += QDEC[(s.ab << 2) | ab];
  s.ab = ab;

  int8_t dir = 0;
  if (cfg.detentSteps == 4) {            // EC11 с 20 щелчками: щелчок в положении «оба разомкнуты»
    if (ab == 3) {
      if (s.acc >= 2) dir = 1;
      else if (s.acc <= -2) dir = -1;
      s.acc = 0;
    }
  } else if (cfg.detentSteps == 2) {     // EC11 с 30 щелчками: щелчок в положениях 00 и 11
    if (ab == 0 || ab == 3) {
      if (s.acc >= 1) dir = 1;
      else if (s.acc <= -1) dir = -1;
      s.acc = 0;
    }
  } else {
    dir = s.acc;
    s.acc = 0;
  }
  if (dir) onDetent(i, dir, now);
}

void onButton(uint8_t i, bool pressed) {
  const EncConfig &e = cfg.enc[i];
  EncState &s = st[i];
  if (e.btnType == BTN_OFF) return;
  bool on;
  if (e.btnMode == BTN_TOGGLE) {
    if (!pressed) return;
    s.toggled = !s.toggled;
    on = s.toggled;
  } else {
    on = pressed;
  }
  if (e.btnType == BTN_CC) sendCC(e.btnChannel, e.btnNumber, on ? 127 : 0);
  else sendNote(e.btnChannel, e.btnNumber, on);
}

void updateButton(uint8_t i, bool pressed, uint32_t now) {
  EncState &s = st[i];
  if (pressed != s.swRaw) {
    s.swRaw = pressed;
    s.swChanged = now;
  }
  if (s.swRaw != s.swStable && now - s.swChanged >= 5) {  // антидребезг 5 мс
    s.swStable = s.swRaw;
    onButton(i, s.swStable);
  }
}

void selectChannel(uint8_t ch) {
  for (uint8_t b = 0; b < 4; b++) digitalWrite(SELECT_PINS[b], (ch >> b) & 1);
  delayMicroseconds(10);  // подтяжка успевает зарядить линию через мультиплексор
}

uint8_t readAB() {
  return (digitalRead(PIN_A) << 1) | digitalRead(PIN_B);
}

void scan() {
  uint32_t now = millis();
  for (uint8_t i = 0; i < NUM_ENC; i++) {
    selectChannel(i);
    uint8_t ab = readAB();
    bool pressed = digitalRead(PIN_SW) == LOW;
    updateEncoder(i, ab, now);
    updateButton(i, pressed, now);
  }
}

// ---------------------------------------------------------------- SysEx-протокол конфигуратора
// F0 7D 4D <команда> [данные] F7   (7D — некоммерческий ID производителя, 4D — устройство)
//   01 → 02 <версия> <число энкодеров> <размер настроек: старшие 7 бит> <младшие 7 бит>
//   03 → 04 <настройки, CFG_SIZE байт>
//   05 <настройки> → 06 <0 — сохранено, 1 — ошибка>
//   07 (сброс по умолчанию) → 06 <0>
//   08 → 09 <16 текущих значений>

const uint8_t SYSEX_ID = 0x7D, DEVICE_ID = 0x4D;
uint8_t rx[CFG_SIZE + 8];
uint16_t rxLen = 0;
bool rxOverflow = false;

void reply(uint8_t cmd, const uint8_t *data, uint16_t len) {
  static uint8_t tx[CFG_SIZE + 6];
  tx[0] = 0xF0;
  tx[1] = SYSEX_ID;
  tx[2] = DEVICE_ID;
  tx[3] = cmd;
  memcpy(tx + 4, data, len);
  tx[4 + len] = 0xF7;
  sendSysex(tx, len + 5);
}

void replyStatus(uint8_t status) {
  reply(0x06, &status, 1);
}

void handleSysex() {
  if (rxOverflow || rxLen < 5 || rx[0] != 0xF0 || rx[1] != SYSEX_ID || rx[2] != DEVICE_ID ||
      rx[rxLen - 1] != 0xF7)
    return;
  uint8_t cmd = rx[3];
  const uint8_t *data = rx + 4;
  uint16_t len = rxLen - 5;

  if (cmd == 0x01) {
    uint8_t info[4] = {FW_VERSION, NUM_ENC, (uint8_t)(CFG_SIZE >> 7), (uint8_t)(CFG_SIZE & 0x7F)};
    reply(0x02, info, 4);
  } else if (cmd == 0x03) {
    reply(0x04, (const uint8_t *)&cfg, CFG_SIZE);
  } else if (cmd == 0x05) {
    Config incoming;
    if (len != CFG_SIZE) {
      replyStatus(1);
      return;
    }
    memcpy(&incoming, data, CFG_SIZE);
    if (!configValid(incoming)) {
      replyStatus(1);
      return;
    }
    cfg = incoming;
    saveConfig();
    replyStatus(0);
  } else if (cmd == 0x07) {
    setDefaults();
    saveConfig();
    replyStatus(0);
  } else if (cmd == 0x08) {
    uint8_t values[NUM_ENC];
    for (uint8_t i = 0; i < NUM_ENC; i++) values[i] = st[i].value;
    reply(0x09, values, NUM_ENC);
  }
}

void sysexByte(uint8_t b) {
  if (b == 0xF0) {
    rxLen = 0;
    rxOverflow = false;
  }
  if (rxLen < sizeof(rx)) rx[rxLen++] = b;
  else rxOverflow = true;
  if (b == 0xF7) {
    handleSysex();
    rxLen = 0;
  }
}

// Обратная связь от программы:
//  - CC на номер/канал энкодера (абсолютный режим) — новое текущее значение, поворот продолжится с него;
//  - CC (≥64 — вкл) или нота (Note On > 0 — вкл) на кнопку-переключатель — новое состояние переключателя.
void onFeedback(uint8_t type, uint8_t ch, uint8_t num, uint8_t value) {
  for (uint8_t i = 0; i < NUM_ENC; i++) {
    const EncConfig &e = cfg.enc[i];
    if (type == 0xB0 && e.mode == ENC_ABSOLUTE && e.channel == ch && e.cc == num) st[i].value = value;
    if (e.btnMode != BTN_TOGGLE || e.btnChannel != ch || e.btnNumber != num) continue;
    if (type == 0xB0 && e.btnType == BTN_CC) st[i].toggled = value >= 64;
    if (type == 0x90 && e.btnType == BTN_NOTE) st[i].toggled = value > 0;
    if (type == 0x80 && e.btnType == BTN_NOTE) st[i].toggled = false;
  }
}

void pollMidi() {
  for (;;) {
    midiEventPacket_t p = MidiUSB.read();
    if (p.header == 0) break;
    uint8_t cin = p.header & 0x0F;
    if (cin == 0x04 || cin == 0x07) {
      sysexByte(p.byte1);
      sysexByte(p.byte2);
      sysexByte(p.byte3);
    } else if (cin == 0x05) {
      sysexByte(p.byte1);
    } else if (cin == 0x06) {
      sysexByte(p.byte1);
      sysexByte(p.byte2);
    } else if (cin == 0x0B || cin == 0x09 || cin == 0x08) {
      onFeedback(p.byte1 & 0xF0, p.byte1 & 0x0F, p.byte2, p.byte3);
    }
  }
}

// ---------------------------------------------------------------- setup / loop

void setup() {
  for (uint8_t b = 0; b < 4; b++) pinMode(SELECT_PINS[b], OUTPUT);
  pinMode(PIN_A, INPUT_PULLUP);
  pinMode(PIN_B, INPUT_PULLUP);
  pinMode(PIN_SW, INPUT_PULLUP);
  loadConfig();
  for (uint8_t i = 0; i < NUM_ENC; i++) {
    selectChannel(i);
    st[i].ab = readAB();
    st[i].swRaw = st[i].swStable = digitalRead(PIN_SW) == LOW;
  }
}

void loop() {
  pollMidi();
  scan();
  if (midiPending) {
    MidiUSB.flush();
    midiPending = false;
  }
}
