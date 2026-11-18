# Правила для ИИ-ассистентов

У пользователя iPad и Android (Pixel 7). Локально ничего не компилирует: всё собирает GitHub Actions (`tools/build.py`), прошивка — из Chrome на Android по USB (WebUSB, `web/index.html`). Отвечай коротко, по-русски.

**Публикация:** пользователь разрешил коммитить и пушить **сразу в `main`**, без pull request. После пуша проверь, что GitHub Actions «Build firmware» прошёл (иначе исправь), и скажи, что через 3–5 минут проект появится на https://djptrvch.github.io/Arduino-Programmator/.

## Новый проект прошивки
1. Создай `firmware/<id>/<id>.ino` (имя `.ino` = имя папки, латиница, без пробелов).
2. Создай `firmware/<id>/project.json`:
   ```json
   { "name": "Название", "name_en": "Name",
     "description": "Что делает и как подключать", "description_en": "What it does and how to wire it",
     "boards": ["promicro"], "libraries": ["Keyboard"] }
   ```
   `boards` — ключи из `boards.json`; `libraries` — имена из Arduino Library Manager (можно `"Имя@версия"`).
3. Для 32U4 (Leonardo/Micro) контроллер = HID: библиотеки `Keyboard`, `Mouse`. iPad понимает клавиатуру/мышь/геймпад без драйверов.
4. Распиновку и схему подключения опиши комментарием в начале `.ino` и в `description`.

## Новая плата
Добавь запись в `boards.json`:
- `fqbn`, `core` (ядро для `arduino-cli core install`; для не-arduino/esp32 ядер добавь URL индекса в `tools/build.py`);
- `method`: протокол прошивки из `PROTOCOLS` в `web/index.html` (сейчас `avr109`) или `download` — только скачивание;
- для `avr109`: `signature` (3 байта hex), `pageSize` (байт), `flashSize` — размер области программы без загрузчика (32U4: 28672).
- Новый протокол (STK500v1 для Uno, STK500v2 для Mega, esptool для ESP32) = новый объект в `PROTOCOLS` с методами `probe(port)` и `flash(port, pages, board)`. Для плат с CH340/CP2102 нужен свой драйвер поверх WebUSB (они не CDC-ACM).

## Языки (RU / EN)
Обе страницы двуязычные, выбор хранится в `localStorage.lang`. Любой новый текст интерфейса: в HTML — атрибут `data-en="English"` (русский текст внутри элемента), в JS — `L("Русский", "English")`. У проектов — `name_en` / `description_en`.

## Веб-программатор
- `web/index.html` — один файл без внешних зависимостей. Маркер `/*@BOARDS@*/null` не трогать.
- Работает только по HTTPS (GitHub Pages) в Chrome на Android/компьютере; в Safari WebUSB нет.

## Конфигуратор контроллера
- `configurator/index.html` → https://djptrvch.github.io/Arduino-Programmator/configurator/ — отдельная страница (Web MIDI + SysEx), настраивает прошивку `firmware/encoders16` без перепрошивки.
- Протокол: `F0 7D 4D <cmd> … F7`, команды описаны в начале раздела SysEx в `encoders16.ino`. Порядок байт настроек одинаков в прошивке (`struct Config`) и конфигураторе (`ENC_FIELDS`) — менять синхронно, при изменении формата поднять версию в EEPROM.
- Не упоминать сторонние коммерческие контроллеры в коде и текстах.

## Проверка
Локально (если есть arduino-cli): `python3 tools/build.py`. Иначе смотри результат GitHub Actions.
