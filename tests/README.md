# Тесты sway-mstack

Два независимых уровня:

| Уровень | Каталог | Что проверяет | Требования |
| --- | --- | --- | --- |
| Unit | `tests/unit/` | статические функции демона | CMake, clang, json-c |
| E2E | `tests/e2e/` | поведение в живом headless Sway | sway, swaymsg, foot, jq |

Unit входит в CTest (`ctest -L unit`). E2E запускается скриптом (или
через CTest, если проект сконфигурирован с `-DSMS_E2E=ON`).

---

## Unit-тесты

Файл `tests/unit/test_main.c` включает `src/main.c` целиком:

```c
#define main sms_main
#include "../../src/main.c"
#undef main
```

Демон — один translation unit: `src/main.c` подтягивает `src/sms.h` и
фрагменты `src/parts/*.inc`. Выносить статические помощники в отдельную
библиотеку ради тестов не нужно. Переименование `main` позволяет
оставить тестовый `main` свой.

Покрытие:

- `ws_guard`, `ws_focus_cmd` — экранирование имён воркспейсов;
- JSON-доступоры (`jget`, `jarr`, `jstr`, `jint`, `jbool`), `layout_is`,
  `is_floating`, `is_tiled`, `parse_json_exact`;
- `collect`, `idx_of`, `proportions_ok`, `dead_master_ws`;
- снимки воркспейсов (`snap_store`, `snap_get`, LRU, границы имён);
- кэш эха (`echo_track`, `echo_consume`, TTL, откат, переполнение);
- построение команд (`cmd_append`, `cmd_struct`, `cmd_move`);
- `output_right_of`, `is_marked_daemon`, `find_focused`;
- обработка `window::focus` и `workspace::focus`;
- IPC-фрейминг и POSIX I/O на `socketpair`.

Запуск:

```sh
cmake --build build --target sms_unit
ctest --test-dir build --output-on-failure
```

Или напрямую:

```sh
./build/sms_unit
```

Сборка Debug включает AddressSanitizer и UndefinedBehaviorSanitizer —
именно так тесты гоняются в CI. Отдельная ручная сборка с санитайзерами:

```sh
clang -std=c23 -O1 -g -fsanitize=address,undefined \
  -fno-omit-frame-pointer -Wall -Wextra -Wpedantic \
  -Wno-unused-function -Wno-unused-variable -Wno-conversion \
  tests/unit/test_main.c -o /tmp/sms_unit $(pkg-config --cflags --libs json-c)
/tmp/sms_unit
```

Тест печатает `unit: N проверок, M провалов`; ненулевой код возврата —
провал. При добавлении проверок используйте `CHECK`, `CHECK_STR` и
существующие хелперы; для JSON — `json(...)`, который печатает фатальную
ошибку, если тестовая строка не разбирается.

Новые тесты стоит добавлять в тот же файл: отдельный translation unit
не увидит статические функции без изменения структуры демона. Если
добавляете новый фрагмент `src/parts/*.inc`, просто включите его в
`src/main.c` — тест подхватит его автоматически.

---

## E2E-тесты

`tests/e2e/run.sh` поднимает **приватную** headless-сессию Sway на
каждый сценарий. Изоляция:

- свой `XDG_RUNTIME_DIR` внутри временного каталога;
- свой `SWAYSOCK`, `WAYLAND_DISPLAY`, `I3SOCK`;
- свой `sms.lock` — демон не конфликтует с запущенным в живой сессии;
- `WLR_BACKENDS=headless`, `WLR_LIBINPUT_NO_DEVICES=1`,
  `WLR_RENDERER=pixman`;
- окна — `foot` с уникальными `app_id` вида `sms-<сценарий>-N`.

Живая сессия пользователя не затрагивается: переменные окружения
выставляются только внутри процесса теста и его детей.

Запуск:

```sh
tests/e2e/run.sh
tests/e2e/run.sh --keep               # сохранить логи и workdir
tests/e2e/run.sh --skip-multi         # без второго выхода
tests/e2e/run.sh --binary build-debug/sway-mstack
E2E_FILTER=zoom tests/e2e/run.sh      # только подходящие сценарии
```

Сценарии:

| Имя | Что проверяет |
| --- | --- |
| `spawn_geometry` | геометрия n=1/2/3, мастер 55%, стопка 45%, ряды поровну |
| `master_close` | смерть мастера n=3→2→1, singleton распускается |
| `zoom` | зум низа и вершины стопки, возврат `move right` |
| `edge_moves` | `move up`/`move down` на мастере, крайние движения |
| `master_right_rotates` | ротация мастера без памяти `_p` |
| `tabbed_stack` | tabbed-стопка, спавн перестраивает её в splitv |
| `long_names` | пробелы, спецсимволы, UTF-8, длина > `WSNAME_MAX` |
| `multi_output` | `create_output`, переброс и возврат окна |

### Переменные окружения

| Переменная | По умолчанию | Смысл |
| --- | --- | --- |
| `E2E_BIN` | `build/sway-mstack` | путь к демону |
| `E2E_WORK` | `mktemp -d` | каталог для логов и сокетов |
| `E2E_SETTLE` | `0.45` | пауза после действий, сек |
| `E2E_KEEP` | `0` | не удалять workdir |
| `E2E_SKIP_MULTI` | `0` | пропустить `multi_output` |
| `E2E_FILTER` | пусто | подстрока имени сценария |

### Если сценарий упал

Перезапустите с `--keep` и посмотрите:

```text
$E2E_WORK/sway.log              — лог compositor
$E2E_WORK/daemon-<case>.log     — лог демона (SMS_DEBUG=1)
```

В логе демона видны решения быстрых путей:

```text
spawn-verdict: окно=9 → тайловый
spawn-fast: ws=4 окно=9
death-fast: ws=4
retile: ws=4 n=3 ...
reconcile: раунд 1, команды=0
```

Частые причины:

- **`sway` не найден** — установите `sway`, `swaymsg`, `foot`, `jq`;
- **`create_output` не поддерживается** — запустите с
  `--skip-multi`; на старых сборках Sway команда может отсутствовать;
- **таймауты на медленной машине** — увеличьте `E2E_SETTLE` и
  `E2E_TRIES`;
- **демон не стартует** — проверьте, что бинарник собран и что
  `$XDG_RUNTIME_DIR` доступен только владельцу (тест сам выставляет
  `0700`).

### Через CTest

```sh
cmake -B build -DSMS_E2E=ON
cmake --build build
ctest --test-dir build -L e2e --output-on-failure
```

E2E помечен таймаутом 900 секунд и не входит в конфигурацию по
умолчанию: для него нужны Sway, foot и jq.

---

## CI

`.github/workflows/ci.yml` собирает матрицу Release и Debug
(ASan/UBSan), гоняет unit-тесты и E2E в headless Sway на Ubuntu.
Локально CI не воспроизводится один в один, но команды те же:

```sh
cmake -B build -DCMAKE_BUILD_TYPE=Release -DSMS_NATIVE=OFF -DSMS_E2E=ON
cmake --build build -j
ctest --test-dir build -L unit --output-on-failure
ctest --test-dir build -L e2e --output-on-failure
```
