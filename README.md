# sway-mstack

[![Version](https://img.shields.io/badge/version-2.1.0-blue.svg)]()
[![CI](https://github.com/pama1232/sway-master-stack/actions/workflows/ci.yml/badge.svg)](https://github.com/pama1232/sway-master-stack/actions/workflows/ci.yml)
[![Standard](https://img.shields.io/badge/C23-ISO%2FIEC%209899%3A2024-00599C.svg)](https://en.wikipedia.org/wiki/C23_(C_standard_revision))
[![Platform](https://img.shields.io/badge/Sway%20%7C%20SwayFX-orange.svg)](https://swaywm.org/)
[![RAM](https://img.shields.io/badge/RAM-~2MB-brightgreen.svg)]()
[![License](https://img.shields.io/badge/license-MIT-green.svg)](LICENSE)

[English](#english) | [Русский](#русский)

---

[![Demo](https://img.shields.io/badge/demo-GIF%20(14%20MB)-ff69b4.svg)](https://files.catbox.moe/pf5914.gif)

> [!NOTE]
> The demo is hosted externally (14 MB) and is not cloned with the
> repository.

> [!NOTE]
> The GIF shows SwayFX with animations enabled (250 ms). On vanilla Sway windows jump and flick: every layout change lands as a discrete transaction and there is no animation layer to smooth the transition. This is Sway's own design (a manual resize in plain Sway flickers the same way) and it cannot be smoothed over IPC. If this bothers you, run SwayFX.

> [!NOTE]
> Гифка лежит снаружи (14 МБ) и не клонируется вместе с репозиторием.

> [!NOTE]
> Гифка показывает SwayFX с включёнными анимациями (250 мс). На ванильном Sway окна дёргаются и мерцают: каждое изменение раскладки ложится дискретной транзакцией, и нет слоя анимаций, который сгладил бы переход. Это собственный дизайн Sway (ручной ресайз в чистом Sway дёргается так же), изнутри IPC это не сгладить. Если это мешает, используйте SwayFX.

---

## English

A small DWM-style master/stack tiling daemon for [Sway](https://github.com/swaywm/sway) and [SwayFX](https://github.com/wlrfx/swayfx). 

Written in **ISO C23** using the native Sway UNIX IPC protocol and `json-c`.

```text
+---------------------------+----------------------+
|                           |       Stack 1        |
|                           +----------------------+
|         Master            |       Stack 2        |
|                           +----------------------+
|                           |       Stack 3        |
|           55%             |         45%          |
+---------------------------+----------------------+
```

### Features

- **Master/stack layout**: the first window takes the workspace, every next one joins the stack on the right.
- **Focus zoom**: `move left` promotes a stack window to master, `move right` returns it. Standard bindings, nothing to rebind.
- **Fast response**: the batch goes out in a single `write()`, no tree round trip.
- **Tray and popups stay out**: the floating verdict is read from the socket before a new window is touched.
- **Multi-monitor safe**: windows stay on their output when the master closes.
- **Stable master close**: if only one stacked window remains, the layout returns to a horizontal master/stack; the next window does not open vertically.
- **Light**: ~2 MB RSS in a release build, 0% CPU idle, clean under AddressSanitizer and UndefinedBehaviorSanitizer.

### Configuration

- `sway-mstack 60`: master share at launch, range 10 to 90. Default 55/45.

### Requirements

- `clang` (>= 19, C23)
- `cmake` (>= 3.25)
- `lld`
- `json-c`
- `pkgconf`

On Arch Linux:

```sh
sudo pacman -S --needed clang lld cmake json-c pkgconf
```

Ubuntu 24.04 ships clang 18, which lacks C23 `constexpr`; install clang-19
or newer from [apt.llvm.org](https://apt.llvm.org/).

### Building

```sh
git clone https://github.com/pama1232/sway-master-stack.git
cd sway-master-stack

CC=clang cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build

sudo cmake --install build
```

`cmake --install` also creates `sway-master-stack` as a compatibility
symlink, so Sway configs from 2.0.x keep working. The canonical binary
name is `sway-mstack`.

For a portable x86-64 binary that is not tied to the build host's CPU,
add `-DSMS_NATIVE=OFF` to the configure command.

### Sway setup

Add to `~/.config/sway/config`:

```sway
exec sway-mstack
```

Or with a custom ratio:

```sway
exec sway-mstack 60
```

### Testing

```sh
# unit tests (CTest)
ctest --test-dir build --output-on-failure

# E2E: a private headless Sway per scenario
tests/e2e/run.sh
```

Unit tests are plain C with no framework; Debug builds run them under
AddressSanitizer and UndefinedBehaviorSanitizer. The E2E suite starts
its own headless Sway with an isolated `XDG_RUNTIME_DIR`, socket, and
lock, so the live session is never touched. Details and troubleshooting:
[tests/README.md](tests/README.md).

### Architecture

The state model, event pipeline, fast paths, and failure handling are
described in [ARCHITECTURE_en.md](ARCHITECTURE_en.md). Contribution
guidelines are in [CONTRIBUTING.md](CONTRIBUTING.md).

---

## Русский

Небольшой демон тайлинга Master-Stack в стиле DWM для [Sway](https://github.com/swaywm/sway) и [SwayFX](https://github.com/wlrfx/swayfx).

Написан на **ISO C23** через нативный сокет Sway IPC (`$SWAYSOCK`) и библиотеку `json-c`.

```text
+---------------------------+----------------------+
|                           |       Стопка 1       |
|                           +----------------------+
|         Мастер            |       Стопка 2       |
|                           +----------------------+
|                           |       Стопка 3       |
|           55%             |         45%          |
+---------------------------+----------------------+
```

### Возможности

- **Раскладка master/stack**: первое окно занимает весь стол, каждое следующее ложится в стопку справа.
- **Зум фокусом**: `move left` поднимает окно стопки в мастер, `move right` возвращает на место. Стандартные бинды, перевешивать не нужно.
- **Быстрый отклик**: команды уходят одним `write()`, без обхода дерева.
- **Трей и попапы не трогаются**: вердикт по плаванию вычитывается из сокета до того, как демон тронет новое окно.
- **Мультимонитор**: окна не улетают на другой выход при закрытии мастера.
- **Стабильное закрытие мастера**: если в стопке осталось одно окно, раскладка возвращается к горизонтальной; следующее окно не открывается вертикально.
- **Лёгкий**: ~2 МБ RSS в релизной сборке, 0% CPU в простое, чист под AddressSanitizer и UndefinedBehaviorSanitizer.

### Настройка

- `sway-mstack 60`: доля мастера при запуске, диапазон от 10 до 90. По умолчанию 55/45.

### Зависимости

- `clang` (>= 19, C23)
- `cmake` (>= 3.25)
- `lld`
- `json-c`
- `pkgconf`

В Arch Linux:

```sh
sudo pacman -S --needed clang lld cmake json-c pkgconf
```

В Ubuntu 24.04 пакетный clang — 18, в нём нет C23 `constexpr`;
установите clang-19 или новее из [apt.llvm.org](https://apt.llvm.org/).

### Сборка и установка

```sh
git clone https://github.com/pama1232/sway-master-stack.git
cd sway-master-stack

CC=clang cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build

sudo cmake --install build
```

`cmake --install` также создаёт совместимую ссылку `sway-master-stack`,
поэтому конфиги Sway от 2.0.x продолжают работать. Основное имя
бинарника — `sway-mstack`.

Для переносимого x86-64-бинарника, не привязанного к CPU сборочной
машины, добавьте `-DSMS_NATIVE=OFF` в команду конфигурации.

### Подключение в Sway

Добавьте в `~/.config/sway/config`:

```sway
exec sway-mstack
```

Или с нужной долей мастера:

```sway
exec sway-mstack 60
```

### Тестирование

```sh
# unit-тесты (CTest)
ctest --test-dir build --output-on-failure

# E2E: приватная headless-сессия Sway на каждый сценарий
tests/e2e/run.sh
```

Unit-тесты — обычный C без фреймворка; в Debug они гоняются под
AddressSanitizer и UndefinedBehaviorSanitizer. E2E поднимает свой
headless Sway с изолированными `XDG_RUNTIME_DIR`, сокетом и lock-файлом,
поэтому живая сессия не затрагивается. Подробности и отладка —
в [tests/README.md](tests/README.md).

### Архитектура

Модель состояния, конвейер событий, быстрые пути и обработка ошибок
описаны в [ARCHITECTURE_ru.md](ARCHITECTURE_ru.md). Правила участия —
в [CONTRIBUTING.md](CONTRIBUTING.md).

---

## Acknowledgements / Благодарности

- Written, debugged, and optimized with the assistance of **GLM 5.3** and **DeepSeek V4.1 Flash**.
- Разработано, отлажено и оптимизировано с помощью **GLM 5.3** и **DeepSeek V4.1 Flash**.

## License / Лицензия

[MIT](LICENSE) © 2026 [pama1232](https://github.com/pama1232)
