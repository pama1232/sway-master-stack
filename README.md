# sway-master-stack

[![Version](https://img.shields.io/badge/version-2.0.0-blue.svg)]()
[![Standard](https://img.shields.io/badge/C23-ISO%2FIEC%209899%3A2024-00599C.svg)](https://en.wikipedia.org/wiki/C23_(C_standard_revision))
[![Platform](https://img.shields.io/badge/Sway-wlroots-orange.svg)](https://swaywm.org/)
[![RAM](https://img.shields.io/badge/RAM-~2MB-brightgreen.svg)]()
[![License](https://img.shields.io/badge/license-MIT-green.svg)](LICENSE)

[English](#english) | [Русский](#русский)

---

![Demo](demo.gif)

---

## English

A fast, no-nonsense DWM-style master/stack tiling daemon for [Sway](https://github.com/swaywm/sway) and SwayFX. 

Written in pure **ISO C23** using the native Sway UNIX IPC protocol and `json-c`.

```text
+------------------------+---------------------+
|                        |       Stack 1       |
|                        +---------------------+
|        Master          |       Stack 2       |
|         55%            +---------------------+
|                        |       Stack 3       |
|                        |         45%         |
+------------------------+---------------------+
```

### What makes v2.0 special?

1. **Zero-Lag Spawn**: Windows appear in their correct place immediately without flashing or moving around after creation.
2. **True DWM Zoom & Return**:
   - Hit your regular `move left` binding on any stack window -> it instantly swaps with Master.
   - Hit `move right` on Master -> it returns right back to where it came from in the stack.
3. **Tray Apps Just Work**: Popups and tray apps from Waybar don't break the tiling grid or kick windows into floating mode.
4. **Multi-Monitor Safe**: Windows never fly off to your other screen when closing or moving them.
5. **Custom Master Size**: Pick your master width on launch: `sway-master-stack 60` for 60/40, or omit it for the default 55/45.
6. **No Config Hacks**: Doesn't hijack your keybindings or require dummy `nop` commands in your Sway config.
7. **Tiny Footprint**: Consumes **~2 MB RAM** and 0.0% CPU when idle. Clean under AddressSanitizer and UndefinedBehaviorSanitizer.

### Building & Installing

Dependencies: `clang` (or `gcc` with C23), `cmake` (>= 3.25), `lld`, `json-c`, `pkgconf`.

On Arch Linux:
```sh
sudo pacman -S --needed clang lld cmake json-c pkgconf
```

Build:
```sh
git clone https://github.com/pama1232/sway-master-stack.git
cd sway-master-stack
CC=clang cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
sudo cmake --install build
```

### Sway Setup

Just add this single line to `~/.config/sway/config`:

```sway
exec sway-master-stack
```

*(Or specify a custom ratio, e.g. `exec sway-master-stack 60`)*.

---

## Русский

Быстрый и надежный демон тайлинга Master-Stack в стиле DWM для [Sway](https://github.com/swaywm/sway) и SwayFX.

Написан на чистом **ISO C23** через нативный сокет Sway IPC (`$SWAYSOCK`) и библиотеку `json-c`.

```text
+------------------------+---------------------+
|                        |       Стопка 1      |
|                        +---------------------+
|        Мастер          |       Стопка 2      |
|         55%            +---------------------+
|                        |       Стопка 3      |
|                        |         45%         |
+------------------------+---------------------+
```

### Главные фишки версии 2.0:

1. **Мгновенное появление окон**: Новые окна сразу встают на своё место без перескакиваний и дерготни.
2. **Настоящий DWM Зум и Возврат**:
   - Нажмите ваш обычный бинд `move left` на любом окне стопки — оно станет Мастером.
   - Нажмите `move right` на Мастере — он вернётся точно на то место в стопке, откуда пришёл.
3. **Трей-окна не ломают сетку**: Апплеты громкости и сети из Waybar открываются плавающими и не сбивают тайлинг.
4. **Безопасен для двух мониторов**: Окна больше не улетают на соседний монитор при закрытии Мастера.
5. **Выбор пропорций**: Задавайте ширину мастера при запуске: `sway-master-stack 60` (для 60/40) или запускайте без аргументов для дефолтных 55/45.
6. **Никаких костылей в конфиге**: Демон не перехватывает ваши хоткеи и не требует странных `nop`-строк в настройках Sway.
7. **Потребление ресурсов**: Всего **~2 МБ ОЗУ** и 0% процессора. Проверено под AddressSanitizer (ноль утечек).

### Сборка и установка

Установка зависимостей в Arch Linux:
```sh
sudo pacman -S --needed clang lld cmake json-c pkgconf
```

Сборка:
```sh
git clone https://github.com/pama1232/sway-master-stack.git
cd sway-master-stack
CC=clang cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
sudo cmake --install build
```

### Подключение в Sway

Добавьте одну строку в ваш `~/.config/sway/config`:

```sway
exec sway-master-stack
```

*(Либо укажите желаемый процент Мастера, например: `exec sway-master-stack 60`)*.

---

## Acknowledgements / Благодарности

- Written, debugged, and optimized with the assistance of **GLM 5.3**.
- Разработано, отлажено и оптимизировано с помощью **GLM 5.3**.

---

## License / Лицензия

[MIT](LICENSE) © 2026 [pama1232](https://github.com/pama1232)
