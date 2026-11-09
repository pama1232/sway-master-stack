# sway-mstack

[![Version](https://img.shields.io/badge/version-2.1.0-blue.svg)]()
[![CI](https://github.com/pama1232/sway-master-stack/actions/workflows/ci.yml/badge.svg)](https://github.com/pama1232/sway-master-stack/actions/workflows/ci.yml)
[![Tests](https://img.shields.io/badge/tests-316%20unit%20%2B%2061%20E2E-success.svg)]()
[![Standard](https://img.shields.io/badge/C23-ISO%2FIEC%209899%3A2024-00599C.svg)](https://en.wikipedia.org/wiki/C23_(C_standard_revision))
[![Platform](https://img.shields.io/badge/Sway%20%7C%20SwayFX-orange.svg)](https://swaywm.org/)
[![RAM](https://img.shields.io/badge/RAM-~2MB-brightgreen.svg)]()
[![License](https://img.shields.io/badge/license-MIT-green.svg)](LICENSE)

[English](#english) | [Русский](#русский)

---

[![Demo](https://img.shields.io/badge/demo-GIF%20(14%20MB)-ff69b4.svg)](https://files.catbox.moe/pf5914.gif)

> [!NOTE]
> The demo is an external GIF (14 MB) and is not cloned with the
> repository. It shows SwayFX with 250 ms animations; vanilla Sway applies
> layout changes as discrete transactions, so windows may jump.

> [!NOTE]
> Демо — внешняя GIF (14 МБ), не клонируется с репозиторием. Показан
> SwayFX с анимациями 250 мс; ванильный Sway применяет изменения
> раскладки дискретными транзакциями, поэтому окна могут дёргаться.

---

## English

**DWM-style master/stack tiling for Sway — as a small IPC daemon, not a
compositor fork.**

Sway ships a manual tiling model: you split, move, and resize containers
yourself. `sway-mstack` adds the missing automatic layout: one master
window on the left, a stack of the remaining windows on the right, and
dwm-like focus zoom through the standard `move left` / `move right`
bindings. It runs next to Sway, talks over `$SWAYSOCK`, and never patches
the compositor or asks you to rebind keys.

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

### Why it exists

If you like dwm's master/stack model but want Wayland, SwayFX
animations, or the rest of the Sway ecosystem, the usual choice is
between a manual tiler and a different compositor. This project keeps
Sway and adds the layout as a daemon.

### What you get

- **Automatic master/stack layout** on every workspace. The first
  window becomes the master; every next one joins the stack.
- **Focus zoom with standard bindings.** `move left` promotes a stack
  window to master; `move right` returns it. Nothing to rebind.
- **No compositor patch.** It is a normal Sway client: no plugin, no
  custom Sway build, no config file.
- **Fast event handling.** Layout changes go out as one IPC batch,
  usually without a `get_tree` round trip.
- **Trays and popups stay out.** The floating verdict is read from the
  event stream before a new window is touched.
- **Multi-monitor safe.** Windows stay on their output when the master
  closes.
- **Stable master close.** If only one stacked window remains, the
  workspace returns to a horizontal master/stack instead of leaving a
  lone vertical container.
- **Light.** About 2 MB RSS in a release build, 0% CPU when idle, clean
  under AddressSanitizer and UndefinedBehaviorSanitizer.
- **Tested.** 316 unit checks and 8 E2E scenarios / 61 checks in CI on
  every push, in Release and Debug (ASan/UBSan).

### How it works

`sway-mstack` subscribes to Sway's `window` and `workspace` events. On
each event it computes the canonical tree for the affected workspace:

```text
splith {
    master _m<ws>
    splitv stack _s<ws>
}
```

It then sends the required commands as one `run_command` batch, using
workspace guards so a window that was moved to another workspace is
never touched by mistake. Healthy layouts are remembered as snapshots,
and a short `reconcile` pass verifies the result after structural
changes. The daemon keeps no state outside Sway: the tree itself is the
source of truth, with marks for master, stack anchor, and zoom partner.

### How it differs from its neighbours

- **Stock Sway** gives you manual splits and containers. It does not
  maintain a master area, stack order, or zoom memory. `sway-mstack`
  adds those without replacing Sway.
- **autotiling** flips the split orientation as windows open. It does
  not maintain a master/stack invariant, stack order, or zoom state.
- **swayr** is a window/workspace switcher and command palette, not an
  automatic layout daemon.
- **SwayFX** is a Sway fork with animations, blur, and rounded corners.
  `sway-mstack` runs on both stock Sway and SwayFX; use SwayFX if you
  want animated transitions.
- **i3-layouts** targets i3 on X11. `sway-mstack` is native to Sway's
  IPC and Wayland session.

### When not to use it

- You want manual control over every split and container.
- You want a floating-only or scratchpad-centric workflow.
- You need multiple dynamic layouts (grid, spiral, monocle) with runtime
  switching. This daemon deliberately implements one layout well.
- You want a full desktop shell, a status bar, or a compositor.

### Configuration

```sh
sway-mstack 60   # master share at launch, range 10..90, default 55
```

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

Ubuntu 24.04 ships clang 18, which lacks C23 `constexpr`; install
clang-19 or newer from [apt.llvm.org](https://apt.llvm.org/).

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

### Project layout

```text
src/
  main.c          entry point, signals, include order
  sms.h           feature macros, includes, tuning, shared flags
  parts/          unity fragments included by main.c
    ipc.inc       POSIX I/O and Sway IPC framing
    json.inc      NULL-safe json-c accessors
    layout.inc    workspace guards, tile collection, index lookup
    echo.inc      echo cache for move commands
    command.inc   command batching and get_tree
    snapshot.inc  proportions, snapshots, output checks
    retile.inc    canonical retile and reconcile
    spawn.inc     fast spawn/death paths and verdicts
    events.inc    window/workspace event handlers
    socket.inc    socket, subscription, lock
```

The daemon is still a single translation unit: CMake compiles only
`src/main.c`, which includes `sms.h` and the parts in declaration order.
The split is for navigation; it does not change code generation. Unit
tests include `src/main.c` and therefore see every static helper.

### Architecture

The state model, event pipeline, fast paths, and failure handling are
described in [ARCHITECTURE_en.md](ARCHITECTURE_en.md). Contribution
guidelines are in [CONTRIBUTING.md](CONTRIBUTING.md).

---

## Русский

**Master/stack в стиле DWM для Sway — маленький IPC-демон, а не форк
композитора.**

В Sway тайлинг ручной: контейнеры делятся, двигаются и ресайзятся
вручную. `sway-mstack` добавляет недостающую автоматику: одно
окно-мастер слева, стопка остальных справа и dwm-подобный зум через
штатные `move left` / `move right`. Демон работает рядом с Sway,
общается через `$SWAYSOCK`, не патчит композитор и не просит
перевешивать бинды.

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

### Зачем это нужно

Если нравится master/stack из dwm, но хочется Wayland, анимаций SwayFX
и остальной экосистемы Sway, обычно приходится выбирать между ручным
тайлером и другим композитором. Этот проект оставляет Sway и добавляет
раскладку отдельным демоном.

### Что вы получаете

- **Автоматическую раскладку master/stack** на каждом воркспейсе.
  Первое окно становится мастером, каждое следующее — в стопку.
- **Зум штатными биндами.** `move left` поднимает окно стопки в мастер,
  `move right` возвращает его на место. Перевешивать ничего не нужно.
- **Без патча композитора.** Это обычный клиент Sway: без плагина,
  кастомной сборки Sway и конфигурационного файла.
- **Быструю обработку событий.** Изменения раскладки уходят одной
  IPC-пачкой, обычно без обратного `get_tree`.
- **Трей и попапы не трогаются.** Вердикт по плаванию читается из потока
  событий до того, как демон тронет новое окно.
- **Безопасность на нескольких выходах.** Окна не улетают на другой
  выход при закрытии мастера.
- **Стабильное закрытие мастера.** Если в стопке осталось одно окно,
  воркспейс возвращается к горизонтальной раскладке, а не оставляет
  одинокий вертикальный контейнер.
- **Лёгкость.** Около 2 МБ RSS в релизной сборке, 0% CPU в простое,
  чисто под AddressSanitizer и UndefinedBehaviorSanitizer.
- **Проверенность.** 316 unit-проверок и 8 E2E-сценариев / 61 проверка
  в CI на каждый push, в Release и Debug (ASan/UBSan).

### Как это работает

`sway-mstack` подписывается на события `window` и `workspace` Sway. На
каждом событии он вычисляет каноническое дерево затронутого воркспейса:

```text
splith {
    мастер _m<ws>
    splitv-стопка _s<ws>
}
```

Затем нужные команды уходят одной пачкой `run_command` с гвардами по
имени воркспейса — окно, которое успели перенести на другой стол, не
будет задето по ошибке. Здоровые раскладки запоминаются как снимки, а
после структурных изменений короткий проход `reconcile` проверяет
результат. Своего состояния вне Sway у демона нет: источник истины —
само дерево, а метки хранят мастера, якорь стопки и партнёра зума.

### Чем отличается от соседей

- **Штатный Sway** даёт ручные сплиты и контейнеры. Он не поддерживает
  мастер-зону, порядок стопки и память зума. `sway-mstack` добавляет
  это, не заменяя Sway.
- **autotiling** переключает направление сплита при открытии окон. Он не
  держит инвариант master/stack, порядок стопки и состояние зума.
- **swayr** — переключатель окон и воркспейсов с командной палитрой, а
  не демон автоматической раскладки.
- **SwayFX** — форк Sway с анимациями, блюром и скруглениями.
  `sway-mstack` работает и на штатном Sway, и на SwayFX; SwayFX нужен,
  если хочется плавных переходов.
- **i3-layouts** рассчитан на i3 в X11. `sway-mstack` нативный для
  Sway IPC и Wayland-сессии.

### Кому не подходит

- Нужен ручной контроль над каждым сплитом и контейнером.
- Нужен workflow только с плавающими окнами или scratchpad.
- Нужны несколько динамических раскладок (grid, spiral, monocle) с
  переключением на ходу. Демон сознательно делает одну раскладку хорошо.
- Нужен полный десктоп-шелл, статус-бар или композитор.

### Настройка

```sh
sway-mstack 60   # доля мастера при запуске, 10..90, по умолчанию 55
```

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

### Структура проекта

```text
src/
  main.c          точка входа, сигналы, порядок включения
  sms.h           feature-макросы, include, тюнинг, общие флаги
  parts/          фрагменты единого translation unit
    ipc.inc       POSIX I/O и кадры Sway IPC
    json.inc      NULL-безопасные доступоры json-c
    layout.inc    гварды воркспейса, сбор тайлов, поиск индекса
    echo.inc      кэш эха move-команд
    command.inc   сборка пачек и get_tree
    snapshot.inc  пропорции, снимки, проверки выходов
    retile.inc    канонический ретайл и reconcile
    spawn.inc     быстрые пути спавна/смерти и вердикты
    events.inc    обработчики событий окна и воркспейса
    socket.inc    сокет, подписка, блокировка
```

Демон остаётся одним translation unit: CMake собирает только
`src/main.c`, который включает `sms.h` и части в порядке объявлений.
Разбиение нужно для навигации и не меняет кодогенерацию. Unit-тесты
включают `src/main.c` и поэтому видят все статические помощники.

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
