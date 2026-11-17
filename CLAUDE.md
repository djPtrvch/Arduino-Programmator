# Правила для ИИ-ассистентов

Пользователь работает **только с iPad**. Локально ничего не компилирует: всё собирает GitHub Actions (`tools/build.py`), прошивает ESP32-мост по Wi-Fi. Отвечай коротко, по-русски.

## Новый проект прошивки
1. Создай `firmware/<id>/<id>.ino` (имя `.ino` = имя папки, латиница, без пробелов).
2. Создай `firmware/<id>/project.json`:
   ```json
   { "name": "Название", "description": "Что делает и как подключать",
     "boards": ["leonardo"], "libraries": ["Keyboard"] }
   ```
   `boards` — ключи из `boards.json`; `libraries` — имена из Arduino Library Manager (можно `"Имя@версия"`).
3. Для 32U4 (Leonardo/Micro) контроллер = HID: библиотеки `Keyboard`, `Mouse`. iPad понимает клавиатуру/мышь/геймпад без драйверов.
4. Распиновку и схему подключения опиши комментарием в начале `.ino` и в `description`.

## Новая плата
Добавь запись в `boards.json`:
- `fqbn`, `core` (ядро для `arduino-cli core install`; для не-arduino/esp32 ядер добавь URL индекса в `tools/build.py`);
- `method`: `isp` — прошивка мостом (только AVR); `download` — только скачивание;
- для `isp`: `signature` (3 байта hex из даташита), `pageSize` (байт), `flashSize` (байт), `withBootloader: true`.

## Мост и веб-интерфейс
- `web/index.html` — один файл без внешних зависимостей (должен работать офлайн на точке доступа моста). Маркер `/*@BOARDS@*/null` не трогать.
- После правки `web/index.html` выполни `python3 tools/build.py --webui-only` и закоммить `bridge/web_ui.h`.
- API моста: `GET /api/status`, `GET /api/chip`, `POST /api/flash?page=&flash=&sig=` (multipart; записи `[u32 LE адрес][страница]`), `POST /api/bridge-update`, `POST /api/wifi`.

## Проверка
Локальная сборка (если есть arduino-cli): `python3 tools/build.py --no-bridge`. Иначе смотри результат GitHub Actions.
