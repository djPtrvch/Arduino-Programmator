// Контроллер на ATmega32U4: кнопки -> USB-клавиатура (HID).
// Кнопка замыкает пин на GND, внутренняя подтяжка включена.
#include <Keyboard.h>

struct Button {
  uint8_t pin;
  uint8_t key;
  bool pressed;
  uint32_t changedAt;
};

Button buttons[] = {
  {2, KEY_UP_ARROW, false, 0},
  {3, KEY_DOWN_ARROW, false, 0},
  {4, KEY_LEFT_ARROW, false, 0},
  {5, KEY_RIGHT_ARROW, false, 0},
  {6, ' ', false, 0},
  {7, KEY_RETURN, false, 0},
  {8, 'z', false, 0},
  {9, 'x', false, 0},
};

const uint32_t DEBOUNCE_MS = 10;

void setup() {
  for (auto &b : buttons) pinMode(b.pin, INPUT_PULLUP);
  Keyboard.begin();
}

void loop() {
  uint32_t now = millis();
  for (auto &b : buttons) {
    bool down = digitalRead(b.pin) == LOW;
    if (down != b.pressed && now - b.changedAt >= DEBOUNCE_MS) {
      b.pressed = down;
      b.changedAt = now;
      if (down) Keyboard.press(b.key);
      else Keyboard.release(b.key);
    }
  }
}
