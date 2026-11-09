# sway-mstack

[![CI](https://github.com/pama1232/sway-master-stack/actions/workflows/ci.yml/badge.svg)](https://github.com/pama1232/sway-master-stack/actions/workflows/ci.yml)
[![Version](https://img.shields.io/badge/version-2.1.0-blue.svg)](https://github.com/pama1232/sway-master-stack/releases/tag/v2.1.0)
[![Standard](https://img.shields.io/badge/C23-ISO%2FIEC%209899%3A2024-00599C.svg)](https://en.wikipedia.org/wiki/C23_(C_standard_revision))
[![Platform](https://img.shields.io/badge/Sway%20%7C%20SwayFX-orange.svg)](https://swaywm.org/)
[![License](https://img.shields.io/badge/license-MIT-green.svg)](LICENSE)

[English](#english) | [Русский](#русский)

[Demo (14 MB GIF, external)](https://files.catbox.moe/pf5914.gif). It shows
SwayFX with 250 ms animations. Vanilla Sway applies layout changes as discrete
transactions, so windows may jump.

Демо (GIF, 14 МБ, внешняя ссылка). Показан SwayFX с анимациями 250 мс. Ванильный
Sway применяет изменения раскладки дискретными транзакциями, поэтому окна могут
дёргаться.

---

## English

Automatic master/stack tiling for Sway, implemented as a small IPC daemon.

Sway has manual tiling: you split, move and resize containers yourself.
`sway-mstack` adds the missing layout. One window becomes the master on the
left, the rest go into a stack on the right, and the standard `move left` /
`move right` bindings give you dwm-style zoom. The daemon runs as a normal Sway
client, talks over `$SWAYSOCK`, and does not patch the compositor or require new
keybindings.

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

- Automatic master/stack layout on every workspace. The first window becomes the
  master; each new window joins the stack.
- Zoom with standard bindings. `move left` promotes a stacked window to master;
  `move right` returns it.
- No compositor patch, plugin, custom Sway build or config file.
- Layout changes are sent as one IPC batch, usually without a `get_tree` round
  trip.
- Floating windows, trays and popups are left alone. The floating verdict is
  read from the event stream before a new window is touched.
- Works well with multiple monitors. The daemon was designed and tested with
  several outputs. Windows stay on their output when the master closes, and
  focus stays on the workspace.
- When only one stacked window remains, the workspace returns to a horizontal
  master/stack instead of leaving a lone vertical container.
- About 2 MB RSS in a release build and 0% CPU when idle. Clean under
  AddressSanitizer and UndefinedBehaviorSanitizer.
- 316 unit checks and 8 E2E scenarios (61 checks), run in CI in Release and
  Debug.

### Install

No package yet. The build is small and takes under a minute.

Requirements: clang >= 19 (C23), CMake >= 3.25, lld, json-c, pkgconf.

Arch Linux:

```sh
sudo pacman -S --needed clang lld cmake json-c pkgconf
```

Ubuntu 24.04 ships clang 18, which lacks C23 `constexpr`. Install clang 19 or
newer from [apt.llvm.org](https://apt.llvm.org/).

Build and install:

```sh
git clone https://github.com/pama1232/sway-master-stack.git
cd sway-master-stack

CC=clang cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
sudo cmake --install build
```

`cmake --install` also creates the `sway-master-stack` compatibility symlink.
The canonical binary is `sway-mstack`.

For a portable x86-64 binary that is not tied to the build host CPU, add
`-DSMS_NATIVE=OFF`.

If you want to maintain an AUR or distro package, open an issue.

### Setup

Add to `~/.config/sway/config`:

```sway
exec sway-mstack
```

Optional master share:

```sway
exec sway-mstack 60
```

Range 10..90, default 55.

### Troubleshooting

Run the daemon with debug logging:

```sh
SMS_DEBUG=1 sway-mstack
```

Make sure it starts inside a Sway session, so `$SWAYSOCK` is set.

### How it works

`sway-mstack` subscribes to Sway's `window` and `workspace` events. On each
event it computes the canonical tree for the affected workspace:

```text
splith {
    master _m<ws>
    splitv stack _s<ws>
}
```

It sends the required commands as one `run_command` batch, with workspace guards
so a window moved to another workspace is never touched by mistake. Healthy
layouts are stored as snapshots, and a short `reconcile` pass verifies the
result after structural changes. The daemon keeps no state outside Sway: the
tree is the source of truth, and marks record the master, stack anchor and zoom
partner.

### Scope

One layout, one process, Sway IPC only. No compositor patches, no plugin API, no
new keybindings, no runtime layout switching. This is a design choice, not a
missing feature. If you need grid, spiral or monocle layouts, use a tool that
provides them.

### How it compares

- **Stock Sway** has manual splits and containers. It does not maintain a master
  area, stack order or zoom memory. `sway-mstack` adds those without replacing
  Sway.
- **autotiling** flips the split orientation as windows open. It does not
  maintain a master/stack invariant, stack order or zoom state.
- **swayr** is a window and workspace switcher, not an automatic layout daemon.
- **SwayFX** is a Sway fork with animations, blur and rounded corners.
  `sway-mstack` runs on both stock Sway and SwayFX; use SwayFX if you want
  animated transitions.
- Older master/stack tools for Sway and i3, such as layman and stacki3, are
  archived. This project is maintained for Sway.

### Development

```sh
# Unit tests (Release)
ctest --test-dir build --output-on-failure

# E2E: a private headless Sway per scenario
tests/e2e/run.sh
```

Unit tests are plain C with no framework. Debug builds run them under
AddressSanitizer and UndefinedBehaviorSanitizer. The E2E suite starts its own
headless Sway with an isolated `XDG_RUNTIME_DIR`, socket and lock, so your live
session is not touched. See [tests/README.md](tests/README.md).

The daemon is one translation unit. CMake compiles only `src/main.c`, which
includes `src/sms.h` and ten fragments in `src/parts/`. The split is for
navigation and does not change code generation. Start with
[ARCHITECTURE_en.md](ARCHITECTURE_en.md) for the state model and event pipeline,
and [CONTRIBUTING.md](CONTRIBUTING.md) for the contribution rules.

### License

[MIT](LICENSE) © 2026 pama1232

### Acknowledgements

AI assistance: GLM 5.3 and DeepSeek V4.1 Flash.

---

## Русский

Master/stack для Sway в стиле dwm, реализованный как небольшой IPC-демон.

В Sway тайлинг ручной: контейнеры делятся, двигаются и ресайзятся вручную.
`sway-mstack` добавляет недостающую раскладку. Первое окно становится мастером
слева, остальные уходят в стопку справа, а штатные `move left` / `move right`
дают зум в стиле dwm. Демон работает как обычный клиент Sway, общается через
`$SWAYSOCK`, не патчит композитор и не требует новых биндов.

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

- Автоматическая раскладка master/stack на каждом воркспейсе. Первое окно
  становится мастером, каждое новое уходит в стопку.
- Зум штатными биндами. `move left` поднимает окно из стопки в мастер,
  `move right` возвращает его обратно.
- Без патча композитора, плагина, кастомной сборки Sway и конфигурационного
  файла.
- Изменения раскладки уходят одной IPC-пачкой, обычно без обратного `get_tree`.
- Плавающие окна, треи и попапы не трогаются. Вердикт по плаванию читается из
  потока событий до того, как демон тронет новое окно.
- Отлично работает с несколькими мониторами. Демон проектировался и
  тестировался под несколько выходов. Окна остаются на своём выходе при закрытии
  мастера, а фокус не перескакивает на другой монитор.
- Если в стопке осталось одно окно, воркспейс возвращается к горизонтальной
  раскладке, а не оставляет одинокий вертикальный контейнер.
- Около 2 МБ RSS в релизной сборке и 0% CPU в простое. Чисто под
  AddressSanitizer и UndefinedBehaviorSanitizer.
- 316 unit-проверок и 8 E2E-сценариев (61 проверка), в CI на каждый push, в
  Release и Debug.

### Установка

Пакета пока нет. Сборка небольшая и занимает меньше минуты.

Зависимости: clang >= 19 (C23), CMake >= 3.25, lld, json-c, pkgconf.

Arch Linux:

```sh
sudo pacman -S --needed clang lld cmake json-c pkgconf
```

В Ubuntu 24.04 пакетный clang 18, в нём нет C23 `constexpr`. Поставьте clang 19
или новее из [apt.llvm.org](https://apt.llvm.org/).

Сборка и установка:

```sh
git clone https://github.com/pama1232/sway-master-stack.git
cd sway-master-stack

CC=clang cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
sudo cmake --install build
```

`cmake --install` также создаёт совместимую ссылку `sway-master-stack`. Основное
имя бинарника: `sway-mstack`.

Для переносимого x86-64-бинарника, не привязанного к CPU сборочной машины,
добавьте `-DSMS_NATIVE=OFF`.

Если хотите поддерживать пакет в AUR или дистрибутиве, откройте issue.

### Подключение

Добавьте в `~/.config/sway/config`:

```sway
exec sway-mstack
```

Если нужна другая доля мастера:

```sway
exec sway-mstack 60
```

Диапазон 10..90, по умолчанию 55.

### Отладка

Запустите демон с подробным логом:

```sh
SMS_DEBUG=1 sway-mstack
```

Демон должен запускаться внутри сессии Sway, чтобы `$SWAYSOCK` был установлен.

### Как это работает

`sway-mstack` подписывается на события `window` и `workspace` Sway. На каждом
событии он вычисляет каноническое дерево затронутого воркспейса:

```text
splith {
    мастер _m<ws>
    splitv-стопка _s<ws>
}
```

Затем нужные команды уходят одной пачкой `run_command` с гвардами по имени
воркспейса: окно, которое успели перенести на другой стол, не будет задето по
ошибке. Здоровые раскладки запоминаются как снимки, а после структурных
изменений короткий проход `reconcile` проверяет результат. Своего состояния вне
Sway у демона нет. Источник истины: само дерево, а метки хранят мастера, якорь
стопки и партнёра зума.

### Границы проекта

Одна раскладка, один процесс, только Sway IPC. Без патчей композитора, плагинов,
новых биндов и переключения раскладок на ходу. Это осознанное решение, а не
недостающая функция. Если нужны grid, spiral или monocle, используйте
инструмент, который их поддерживает.

### Чем отличается от соседей

- **Штатный Sway** даёт ручные сплиты и контейнеры. Он не поддерживает
  мастер-зону, порядок стопки и память зума. `sway-mstack` добавляет это, не
  заменяя Sway.
- **autotiling** переключает направление сплита при открытии окон. Он не держит
  инвариант master/stack, порядок стопки и состояние зума.
- **swayr** переключает окна и воркспейсы, а не управляет раскладкой.
- **SwayFX**: форк Sway с анимациями, блюром и скруглениями. `sway-mstack`
  работает и на штатном Sway, и на SwayFX; SwayFX нужен, если хочется плавных
  переходов.
- Старые master/stack-инструменты для Sway и i3, например layman и stacki3,
  архивированы. Этот проект поддерживается для Sway.

### Разработка

```sh
# Unit-тесты (Release)
ctest --test-dir build --output-on-failure

# E2E: приватная headless-сессия Sway на каждый сценарий
tests/e2e/run.sh
```

Unit-тесты: обычный C без фреймворка. В Debug они гоняются под AddressSanitizer
и UndefinedBehaviorSanitizer. E2E поднимает свой headless Sway с изолированными
`XDG_RUNTIME_DIR`, сокетом и lock-файлом, поэтому живая сессия не затрагивается.
Подробности в [tests/README.md](tests/README.md).

Демон остаётся одним translation unit. CMake собирает только `src/main.c`,
который включает `src/sms.h` и десять фрагментов в `src/parts/`. Разбиение нужно
для навигации и не меняет кодогенерацию. Начните с
[ARCHITECTURE_ru.md](ARCHITECTURE_ru.md): там модель состояния и конвейер
событий. Правила участия в [CONTRIBUTING_ru.md](CONTRIBUTING_ru.md).

### Лицензия

[MIT](LICENSE) © 2026 pama1232

### Благодарности

Помощь ИИ: GLM 5.3 и DeepSeek V4.1 Flash.
