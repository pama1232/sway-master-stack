# sway-master-stack

[![Version](https://img.shields.io/badge/version-2.0.0-blue.svg)]()
[![Standard](https://img.shields.io/badge/C23-ISO%2FIEC%209899%3A2024-00599C.svg)](https://en.wikipedia.org/wiki/C23_(C_standard_revision))
[![Platform](https://img.shields.io/badge/Sway%20%7C%20SwayFX-orange.svg)](https://swaywm.org/)
[![RAM](https://img.shields.io/badge/RAM-~2MB-brightgreen.svg)]()
[![License](https://img.shields.io/badge/license-MIT-green.svg)](LICENSE)

[English](#english) | [Русский](#русский)

---

![Demo](demo-nvenc.mp4)

> [!NOTE]
> **About visual smoothness in the demo:**  
> The recording uses **SwayFX** with `animation_duration_ms 250`. In Sway, the compositor maps and renders a window surface (`view_map`) before emitting the `window::new` IPC event. Any external daemon can only reorder containers after receiving that event. In vanilla Sway (without animations), you may see a brief 1-frame repositioning step when opening windows. Enabling window animations in SwayFX smooths out this transition.

---

## English

A small DWM-style master/stack tiling daemon for [Sway](https://github.com/swaywm/sway) and SwayFX. 

Written in **ISO C23** using the native Sway UNIX IPC protocol and `json-c`.

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

### Features

1. **Deterministic Master/Stack Layout**:
   - The first window takes 100% of the workspace.
   - The second window opens to the right; the master window resizes to 55%.
   - Subsequent windows (3+) join the vertical stack on the right.
2. **Snapshot-Based Placement**:
   - Uses cached workspace layouts to dispatch repositioning commands immediately upon receiving events.
3. **DWM Zoom & Return**:
   - Regular `move left` on any stack window promotes it to Master.
   - Regular `move right` on Master returns it back to its previous position in the stack.
4. **Waybar & Tray Applet Handling**:
   - Checks socket buffers during window map events to allow floating rules (`for_window ... floating`) to apply first, preventing popups from disrupting the tiled grid.
5. **Multi-Monitor Safe**:
   - Resolves workspaces by container ID within their local tree instead of relying on global seat focus. Does not move windows across display boundaries on window close.
6. **Configurable Master Ratio**:
   - Specify master width on launch: `sway-master-stack 60` for 60/40, or omit the argument for the default 55/45. Can also be set via the `SMS_PCT` environment variable.
7. **No Config Modification Required**:
   - Works with default Sway move keybindings. Does not require dummy bindings or custom commands in your Sway configuration.
8. **Low Resource Usage**:
   - Consumes **~2 MB RSS** and 0.0% CPU when idle (event-driven via blocking `read()` on the UNIX socket). Checked with AddressSanitizer and UndefinedBehaviorSanitizer.

### Requirements

- `clang` (>= 18 with C23 support)
- `cmake` (>= 3.25)
- `lld`
- `json-c`
- `pkgconf`

Install dependencies on Arch Linux:
```sh
sudo pacman -S --needed clang lld cmake json-c pkgconf
```

### Building & Installing

```sh
git clone https://github.com/pama1232/sway-master-stack.git
cd sway-master-stack

# Build release binary
CC=clang cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build

# Install binary to /usr/local/bin
sudo cmake --install build
```

### Sway Setup

Add this line to `~/.config/sway/config`:

```sway
exec sway-master-stack
```

*(Or specify a custom ratio, e.g. `exec sway-master-stack 60`)*.

---

## Русский

Небольшой демон тайлинга Master-Stack в стиле DWM для [Sway](https://github.com/swaywm/sway) и SwayFX.

Написан на **ISO C23** через нативный сокет Sway IPC (`$SWAYSOCK`) и библиотеку `json-c`.

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

> [!NOTE]
> **О плавности на видео:**  
> На записи используется **SwayFX** с анимациями `animation_duration_ms 250`. В Sway композитор сначала мапит и рендерит окно (`view_map`), и лишь затем отправляет событие `window::new` в сокет IPC. Любой внешний демон реагирует уже постфактум. В ванильном Sway (где анимаций нет) при спавне окна может быть заметен скачок в один кадр. Анимации в SwayFX сглаживают это смещение.

### Возможности

1. **Предсказуемый Master/Stack**:
   - Первое окно занимает 100% стола.
   - Второе окно встаёт справа; мастер занимает 55%.
   - Все следующие окна (3+) ложатся в правую вертикальную стопку.
2. **Расстановка по снимкам**:
   - Использует сохранённую структуру стола для быстрой отправки команд сразу при получении события.
3. **DWM Зум и Возврат**:
   - Стандартный `move left` на окне стопки делает его Мастером.
   - Стандартный `move right` на Мастере возвращает его обратно на прежнее место в стопке.
4. **Устойчивость к трей-окнам**:
   - Проверяет буфер сокета в момент мапа, позволяя правилам плавающих окон (`for_window ... floating`) сработать до перестроения тайлинга.
5. **Корректная работа с несколькими мониторами**:
   - Ищет контейнеры по ID внутри локального дерева стола, не завязываясь на глобальный фокус. Окна не улетают на соседний монитор при закрытии мастера.
6. **Выбор пропорций**:
   - Ширину мастера можно задать аргументом при запуске: `sway-master-stack 60` (для 60/40) или переменной `SMS_PCT`. По умолчанию: 55/45.
7. **Без лишних правок конфига**:
   - Работает со стандартными биндами перемещения Sway. Не требует сторонних команд или фиктивных биндингов.
8. **Экономное потребление**:
   - Занимает **~2 МБ ОЗУ** и 0% CPU в простое (спит на блокирующем вызове `read()` из сокета). Проверен под AddressSanitizer и UndefinedBehaviorSanitizer.

### Зависимости

- `clang` (>= 18 с поддержкой C23)
- `cmake` (>= 3.25)
- `lld`
- `json-c`
- `pkgconf`

Установка зависимостей в Arch Linux:
```sh
sudo pacman -S --needed clang lld cmake json-c pkgconf
```

### Сборка и установка

```sh
git clone https://github.com/pama1232/sway-master-stack.git
cd sway-master-stack

# Сборка Release-бинарника
CC=clang cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build

# Установка бинарника в /usr/local/bin
sudo cmake --install build
```

### Подключение в Sway

Добавьте строку в `~/.config/sway/config`:

```sway
exec sway-master-stack
```

*(Или с нужным процентом, например: `exec sway-master-stack 60`)*.

---

## Acknowledgements / Благодарности

- Written, debugged, and optimized with the assistance of **GLM 5.3**.
- Разработано, отлажено и оптимизировано с помощью **GLM 5.3**.

---

## License / Лицензия

[MIT](LICENSE) © 2026 [pama1232](https://github.com/pama1232)

---
