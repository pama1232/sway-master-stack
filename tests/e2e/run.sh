#!/usr/bin/env bash
# run.sh — E2E-сценарии sway-mstack в приватной headless-сессии Sway.
#
# Каждый сценарий поднимает свой Sway (HEADLESS-1, 1280x720), свой
# демон и свои окна foot; живая сессия пользователя не затрагивается.
#
# Запуск:
#   tests/e2e/run.sh                      # все сценарии
#   tests/e2e/run.sh --keep               # сохранить логи и workdir
#   tests/e2e/run.sh --skip-multi         # без второго выхода
#   tests/e2e/run.sh --binary build-debug/sway-mstack
#   E2E_FILTER=zoom tests/e2e/run.sh      # только сценарии с подстрокой
#
# Требования: sway, swaymsg, foot, jq, собранный демон.
# Для сценария drag_center нужен ещё wlrctl (virtual pointer).
set -uo pipefail

HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=lib.sh
source "$HERE/lib.sh"

usage() {
  cat <<'EOF'
Использование: tests/e2e/run.sh [опции]

  -b, --binary PATH   путь к собранному sway-mstack
                      (по умолчанию build/sway-mstack)
  -k, --keep          не удалять workdir и логи после прогона
      --skip-multi    пропустить сценарий со вторым выходом
  -h, --help          эта справка

Переменные окружения:
  E2E_BIN             то же, что --binary
  E2E_FILTER          подстрока имени сценария (например, zoom)
  E2E_KEEP=1          то же, что --keep
  E2E_SKIP_MULTI=1    то же, что --skip-multi
  E2E_SETTLE          пауза после действий, сек (по умолчанию 0.45)
EOF
}

while [ $# -gt 0 ]; do
  case $1 in
    -b | --binary)
      shift
      [ $# -gt 0 ] || {
        printf '%s\n' '--binary требует путь' >&2
        exit 2
      }
      E2E_BIN=$1
      ;;
    -k | --keep) E2E_KEEP=1 ;;
    --skip-multi) E2E_SKIP_MULTI=1 ;;
    -h | --help)
      usage
      exit 0
      ;;
    *)
      printf 'неизвестный аргумент: %s\n' "$1" >&2
      usage >&2
      exit 2
      ;;
  esac
  shift
done

e2e_init || exit 2

# ── Сценарии ──

# Спавн и геометрия: мастер 55%, стопка 45% справа, ряды поровну.
t_spawn_geometry() {
  e2e_spawn_until 1 1 || return 1
  e2e_check_eq "$(e2e_master_app)" "$(e2e_app 1)" "n=1: мастер — единственное окно"
  e2e_check_rect "$(e2e_app 1)" x 0 2 "n=1: x"
  e2e_check_rect "$(e2e_app 1)" width 1280 2 "n=1: ширина"
  e2e_check_rect "$(e2e_app 1)" height 720 2 "n=1: высота"

  e2e_spawn_until 2 2 || return 1
  e2e_check_eq "$(e2e_master_app)" "$(e2e_app 2)" "n=2: мастер — новое окно"
  e2e_check_eq "$(e2e_stack_apps)" "$(e2e_app 1)" "n=2: стопка"
  e2e_check_eq "$(e2e_stack_layout)" splitv "n=2: layout стопки"
  e2e_check_rect "$(e2e_app 2)" x 0 2 "n=2: мастер x"
  e2e_check_rect "$(e2e_app 2)" width 704 2 "n=2: мастер ширина (55%)"
  e2e_check_rect "$(e2e_app 2)" height 720 2 "n=2: мастер высота"
  e2e_check_rect "$(e2e_app 1)" x 704 2 "n=2: стопка x"
  e2e_check_rect "$(e2e_app 1)" width 576 2 "n=2: стопка ширина (45%)"
  e2e_check_rect "$(e2e_app 1)" height 720 2 "n=2: стопка высота"

  e2e_spawn_until 3 3 || return 1
  e2e_check_eq "$(e2e_master_app)" "$(e2e_app 3)" "n=3: мастер — новое окно"
  e2e_check_eq "$(e2e_stack_apps)" "$(e2e_app 2) $(e2e_app 1)" \
    "n=3: порядок стопки сверху вниз"
  e2e_check_eq "$(e2e_leaf_count)" 3 "n=3: окон на столе"
  e2e_check_rect "$(e2e_app 3)" width 704 2 "n=3: мастер ширина"
  e2e_check_rect "$(e2e_app 2)" x 704 2 "n=3: верх стопки x"
  e2e_check_rect "$(e2e_app 2)" y 0 2 "n=3: верх стопки y"
  e2e_check_rect "$(e2e_app 2)" width 576 2 "n=3: верх стопки ширина"
  e2e_check_rect "$(e2e_app 2)" height 360 2 "n=3: верх стопки высота"
  e2e_check_rect "$(e2e_app 1)" y 360 2 "n=3: низ стопки y"
  e2e_check_rect "$(e2e_app 1)" height 360 2 "n=3: низ стопки высота"
}

# Смерть мастера: n=3 → n=2 (новый мастер из вершины стопки),
# затем n=2 → n=1 (singleton распускается, окно во весь стол).
t_master_close() {
  e2e_spawn_until 1 1 || return 1
  e2e_spawn_until 2 2 || return 1
  e2e_spawn_until 3 3 || return 1
  e2e_wait_master "$(e2e_app 3)" || return 1

  e2e_close_app "$(e2e_app 3)" || return 1
  e2e_wait_leaf 2 || return 1
  e2e_wait_master "$(e2e_app 2)" || return 1
  e2e_check_eq "$(e2e_master_app)" "$(e2e_app 2)" "n=3→2: мастер — вершина стопки"
  e2e_check_eq "$(e2e_stack_apps)" "$(e2e_app 1)" "n=3→2: стопка"
  e2e_check_eq "$(e2e_stack_layout)" splitv "n=3→2: layout стопки"
  e2e_check_rect "$(e2e_app 2)" width 704 2 "n=3→2: мастер ширина"

  e2e_close_app "$(e2e_app 2)" || return 1
  e2e_wait_leaf 1 || return 1
  e2e_wait_master "$(e2e_app 1)" || return 1
  e2e_check_eq "$(e2e_master_app)" "$(e2e_app 1)" "n=2→1: мастер — выжившее окно"
  e2e_check_rect "$(e2e_app 1)" x 0 2 "n=2→1: x"
  e2e_check_rect "$(e2e_app 1)" width 1280 2 "n=2→1: окно во всю ширину"
  e2e_check_rect "$(e2e_app 1)" height 720 2 "n=2→1: окно во всю высоту"
}

# Мастер ушёл в скретчпад: вершина стопки сразу становится мастером,
# стопка остаётся справа, а не растягивается на весь экран.
t_master_scratchpad() {
  e2e_spawn_until 1 1 || return 1
  e2e_spawn_until 2 2 || return 1
  e2e_spawn_until 3 3 || return 1
  e2e_spawn_until 4 4 || return 1
  e2e_wait_master "$(e2e_app 4)" || return 1

  e2e_focus_app "$(e2e_app 4)" || return 1
  swaymsg 'move scratchpad' >/dev/null 2>&1 || true
  e2e_wait_leaf 3 || return 1
  e2e_wait_master "$(e2e_app 3)" || return 1
  e2e_check_eq "$(e2e_master_app)" "$(e2e_app 3)" \
    "scratchpad: новый мастер — вершина стопки"
  e2e_check_eq "$(e2e_stack_apps)" "$(e2e_app 2) $(e2e_app 1)" \
    "scratchpad: порядок стопки"
  e2e_wait_stack_layout splitv || return 1
  e2e_check_eq "$(e2e_stack_layout)" splitv "scratchpad: layout стопки"
  e2e_check_rect "$(e2e_app 3)" x 0 2 "scratchpad: мастер x"
  e2e_check_rect "$(e2e_app 3)" width 704 2 "scratchpad: мастер ширина"
  e2e_check_rect "$(e2e_app 3)" height 720 2 "scratchpad: мастер высота"
  e2e_check_rect "$(e2e_app 2)" x 704 2 "scratchpad: стопка x"
  e2e_check_rect "$(e2e_app 2)" width 576 2 "scratchpad: стопка ширина"
  e2e_check_rect "$(e2e_app 2)" height 360 2 "scratchpad: верх стопки высота"
  e2e_check_rect "$(e2e_app 1)" y 360 2 "scratchpad: низ стопки y"

  # Возврат из скретчпада: окно плавающее, раскладка не меняется.
  swaymsg 'scratchpad show' >/dev/null 2>&1 || true
  e2e_settle
  e2e_check_eq "$(e2e_master_app)" "$(e2e_app 3)" \
    "scratchpad show: мастер не изменился"
  e2e_check_eq "$(e2e_stack_apps)" "$(e2e_app 2) $(e2e_app 1)" \
    "scratchpad show: стопка не изменилась"
}

# Зум: move left на окне стопки делает его мастером, move right
# возвращает прежний порядок. Проверяем низ и вершину стопки.
t_zoom() {
  e2e_spawn_until 1 1 || return 1
  e2e_spawn_until 2 2 || return 1
  e2e_spawn_until 3 3 || return 1
  e2e_wait_master "$(e2e_app 3)" || return 1

  e2e_focus_app "$(e2e_app 1)" || return 1
  e2e_move left
  e2e_wait_master "$(e2e_app 1)" || return 1
  e2e_check_eq "$(e2e_master_app)" "$(e2e_app 1)" "зум низа: мастер"
  e2e_check_eq "$(e2e_stack_apps)" "$(e2e_app 2) $(e2e_app 3)" \
    "зум низа: порядок стопки"
  e2e_move right
  e2e_wait_master "$(e2e_app 3)" || return 1
  e2e_check_eq "$(e2e_stack_apps)" "$(e2e_app 2) $(e2e_app 1)" \
    "возврат низа: порядок восстановлен"

  e2e_focus_app "$(e2e_app 2)" || return 1
  e2e_move left
  e2e_wait_master "$(e2e_app 2)" || return 1
  e2e_check_eq "$(e2e_master_app)" "$(e2e_app 2)" "зум вершины: мастер"
  e2e_check_eq "$(e2e_stack_apps)" "$(e2e_app 3) $(e2e_app 1)" \
    "зум вершины: порядок стопки"
  e2e_move right
  e2e_wait_master "$(e2e_app 3)" || return 1
  e2e_check_eq "$(e2e_stack_apps)" "$(e2e_app 2) $(e2e_app 1)" \
    "возврат вершины: порядок восстановлен"
}

# Крайние движения мастера: up отправляет мастера наверх стопки и
# поднимает её вершину; down — вниз (мастером становится низ стопки).
t_edge_moves() {
  e2e_spawn_until 1 1 || return 1
  e2e_spawn_until 2 2 || return 1
  e2e_spawn_until 3 3 || return 1
  e2e_wait_master "$(e2e_app 3)" || return 1

  e2e_focus_app "$(e2e_app 3)" || return 1
  e2e_move up
  e2e_wait_master "$(e2e_app 2)" || return 1
  e2e_check_eq "$(e2e_master_app)" "$(e2e_app 2)" "move up: новый мастер"
  e2e_check_eq "$(e2e_stack_apps)" "$(e2e_app 3) $(e2e_app 1)" \
    "move up: мастер ушёл наверх стопки"

  e2e_focus_app "$(e2e_app 2)" || return 1
  e2e_move down
  e2e_wait_master "$(e2e_app 1)" || return 1
  e2e_check_eq "$(e2e_master_app)" "$(e2e_app 1)" "move down: новый мастер"
  e2e_check_eq "$(e2e_stack_apps)" "$(e2e_app 3) $(e2e_app 2)" \
    "move down: мастер ушёл вниз стопки"
}

# Без выхода справа move right на мастере — ротация: мастер уходит
# наверх стопки, её вершина становится мастером.
t_master_right_rotates() {
  e2e_spawn_until 1 1 || return 1
  e2e_spawn_until 2 2 || return 1
  e2e_spawn_until 3 3 || return 1
  e2e_wait_master "$(e2e_app 3)" || return 1

  e2e_focus_app "$(e2e_app 3)" || return 1
  e2e_move right
  e2e_wait_master "$(e2e_app 2)" || return 1
  e2e_check_eq "$(e2e_master_app)" "$(e2e_app 2)" "ротация: новый мастер"
  e2e_check_eq "$(e2e_stack_apps)" "$(e2e_app 3) $(e2e_app 1)" \
    "ротация: прежний мастер наверху стопки"
}

# Стопка в tabbed: спавн перестраивает её в splitv (иначе новое окно
# получило бы вертикальный «мастер»), смерть мастера работает как обычно.
t_tabbed_stack() {
  e2e_spawn_until 1 1 || return 1
  e2e_spawn_until 2 2 || return 1
  e2e_spawn_until 3 3 || return 1
  e2e_wait_master "$(e2e_app 3)" || return 1

  e2e_focus_app "$(e2e_app 2)" || return 1
  swaymsg 'layout tabbed' >/dev/null 2>&1 || true
  e2e_wait_stack_layout tabbed || return 1
  e2e_check_eq "$(e2e_stack_layout)" tabbed "tabbed: стопка переключена"

  e2e_spawn_until 4 4 || return 1
  e2e_wait_master "$(e2e_app 4)" || return 1
  e2e_check_eq "$(e2e_master_app)" "$(e2e_app 4)" "tabbed: новое окно — мастер"
  e2e_check_eq "$(e2e_stack_apps)" \
    "$(e2e_app 3) $(e2e_app 2) $(e2e_app 1)" "tabbed: стопка после спавна"
  e2e_wait_stack_layout splitv || return 1
  e2e_check_eq "$(e2e_stack_layout)" splitv "tabbed: стопка снова splitv"

  e2e_close_app "$(e2e_app 4)" || return 1
  e2e_wait_leaf 3 || return 1
  e2e_wait_master "$(e2e_app 3)" || return 1
  e2e_check_eq "$(e2e_master_app)" "$(e2e_app 3)" "tabbed: мастер после close"
  e2e_check_eq "$(e2e_stack_apps)" "$(e2e_app 2) $(e2e_app 1)" \
    "tabbed: стопка после close"
}

# Имена workspace: длинные, с пробелами/спецсимволами/UTF-8 и за
# пределом WSNAME_MAX — спавн всё равно даёт канонический layout.
t_long_names() {
  e2e_spawn_until 1 1 || return 1
  e2e_spawn_until 2 2 || return 1

  local name1='dev workspace with spaces : and / dashes - and UTF-8 рабочий'
  e2e_rename_ws "$name1" || return 1
  e2e_check_eq "$(e2e_current_ws)" "$name1" "имя со спецсимволами сохранено"
  e2e_spawn_until 3 3 || return 1
  e2e_wait_master "$(e2e_app 3)" || return 1
  e2e_check_eq "$(e2e_stack_apps)" "$(e2e_app 2) $(e2e_app 1)" \
    "спецсимволы: порядок стопки"

  local name2
  name2=$(printf 'x%.0s' $(seq 1 120))
  e2e_rename_ws "$name2" || return 1
  e2e_spawn_until 4 4 || return 1
  e2e_wait_master "$(e2e_app 4)" || return 1
  e2e_check_eq "$(e2e_stack_apps)" \
    "$(e2e_app 3) $(e2e_app 2) $(e2e_app 1)" ">WSNAME_MAX: порядок стопки"
  e2e_check_eq "$(e2e_stack_layout)" splitv ">WSNAME_MAX: layout стопки"

  local name3
  name3=$(printf 'y%.0s' $(seq 1 96))
  e2e_rename_ws "$name3" || return 1
  e2e_spawn_until 5 5 || return 1
  e2e_wait_master "$(e2e_app 5)" || return 1
  e2e_check_eq "$(e2e_stack_apps)" \
    "$(e2e_app 4) $(e2e_app 3) $(e2e_app 2) $(e2e_app 1)" \
    "ровно WSNAME_MAX: порядок стопки"
  e2e_check_eq "$(e2e_stack_layout)" splitv "ровно WSNAME_MAX: layout стопки"
}

# Второй выход: move right из стопки уносит окно на соседний выход
# мастером; повторный move right — no-op; move left возвращает его
# мастером на исходный стол.
t_multi_output() {
  local out ws2
  out=$(e2e_create_output) || return 1
  ws2=$(swaymsg -t get_workspaces -r 2>/dev/null |
    jq -r --arg o "$out" \
      '[.[] | select(.output == $o)][0].name // empty' 2>/dev/null)
  if [ -z "$ws2" ]; then
    e2e_fail "не удалось определить workspace на выходе '$out'"
    return 1
  fi

  e2e_spawn_until 1 1 || return 1
  e2e_spawn_until 2 2 || return 1
  e2e_spawn_until 3 3 || return 1
  e2e_wait_master "$(e2e_app 3)" || return 1

  e2e_focus_app "$(e2e_app 2)" || return 1
  e2e_move right
  e2e_wait_app_ws "$(e2e_app 2)" "$ws2" || return 1
  e2e_wait_master "$(e2e_app 3)" || return 1
  e2e_check_eq "$(e2e_master_app 1)" "$(e2e_app 3)" \
    "move right: мастер на исходном столе"
  e2e_check_eq "$(e2e_stack_apps 1)" "$(e2e_app 1)" \
    "move right: стопка на исходном столе"
  e2e_check_eq "$(e2e_master_app "$ws2")" "$(e2e_app 2)" \
    "move right: окно стало мастером на втором выходе"

  e2e_move right
  e2e_check_eq "$(e2e_app_ws_name "$(e2e_app 2)")" "$ws2" \
    "повторный move right: окно осталось на втором выходе"

  e2e_move left
  e2e_wait_app_ws "$(e2e_app 2)" 1 || return 1
  e2e_wait_master "$(e2e_app 2)" || return 1
  e2e_check_eq "$(e2e_master_app 1)" "$(e2e_app 2)" \
    "move left: окно вернулось мастером"
  e2e_check_eq "$(e2e_stack_apps 1)" "$(e2e_app 3) $(e2e_app 1)" \
    "move left: прежний мастер ушёл наверх стопки"
}

# Свап центра при drag за заголовок: Sway's finalize_move зовёт
# container_swap без единого события. Сторож (WATCHDOG_MS) обязан
# привести метки и долю 55/45 сразу после drop, не дожидаясь
# следующего move. Проверяем оба направления: стопка→мастер и
# мастер→стопка.
t_drag_center() {
  e2e_spawn_until 1 1 || return 1
  e2e_spawn_until 2 2 || return 1
  e2e_spawn_until 3 3 || return 1
  e2e_wait_master "$(e2e_app 3)" || return 1

  # app2 (верх стопки) → центр мастера app3
  e2e_drag_center "$(e2e_app 2)" "$(e2e_app 3)" || return 1
  e2e_wait_master "$(e2e_app 2)" || return 1
  e2e_settle 0.3
  e2e_check_eq "$(e2e_master_app)" "$(e2e_app 2)" \
    "drag стопка→мастер: мастер — перетащенное окно"
  e2e_check_eq "$(e2e_stack_apps)" "$(e2e_app 3) $(e2e_app 1)" \
    "drag стопка→мастер: порядок стопки"
  e2e_check_eq "$(e2e_leaf_count)" 3 "drag стопка→мастер: окон на столе"
  e2e_check_rect "$(e2e_app 2)" x 0 2 "drag стопка→мастер: мастер x"
  e2e_check_rect "$(e2e_app 2)" width 704 2 "drag стопка→мастер: мастер 55%"
  e2e_check_rect "$(e2e_app 3)" x 704 2 "drag стопка→мастер: стопка x"
  e2e_check_rect "$(e2e_app 3)" width 576 2 "drag стопка→мастер: стопка 45%"

  # app2 (мастер) → центр app3 (верх стопки): места меняются,
  # мастером становится app3, app2 уходит наверх стопки.
  e2e_drag_center "$(e2e_app 2)" "$(e2e_app 3)" || return 1
  e2e_wait_master "$(e2e_app 3)" || return 1
  e2e_settle 0.3
  e2e_check_eq "$(e2e_master_app)" "$(e2e_app 3)" \
    "drag мастер→стопка: мастер — окно-цель"
  e2e_check_eq "$(e2e_stack_apps)" "$(e2e_app 2) $(e2e_app 1)" \
    "drag мастер→стопка: порядок стопки"
  e2e_check_eq "$(e2e_leaf_count)" 3 "drag мастер→стопка: окон на столе"
  e2e_check_rect "$(e2e_app 3)" x 0 2 "drag мастер→стопка: мастер x"
  e2e_check_rect "$(e2e_app 3)" width 704 2 "drag мастер→стопка: мастер 55%"
  e2e_check_rect "$(e2e_app 2)" x 704 2 "drag мастер→стопка: стопка x"
  e2e_check_rect "$(e2e_app 2)" width 576 2 "drag мастер→стопка: стопка 45%"
}

# ── Прогон ──

E2E_RUN=0
E2E_SKIPPED=0

run_case() {
  local name=$1 fn=$2
  if [ -n "${E2E_FILTER:-}" ]; then
    case $name in
      *"$E2E_FILTER"*) ;;
      *)
        E2E_SKIPPED=$((E2E_SKIPPED + 1))
        return 0
        ;;
    esac
  fi
  E2E_RUN=$((E2E_RUN + 1))
  if e2e_case_begin "$name"; then
    "$fn"
  else
    e2e_fail "сценарий '$name' не стартовал"
  fi
  e2e_case_end
}

run_case spawn_geometry t_spawn_geometry
run_case master_close t_master_close
run_case master_scratchpad t_master_scratchpad
if command -v wlrctl >/dev/null 2>&1; then
  E2E_SWAY_EXTRA='default_border normal
default_floating_border normal'
  run_case drag_center t_drag_center
  E2E_SWAY_EXTRA=
else
  e2e_info "E2E: drag_center пропущен (нет wlrctl)"
fi
run_case zoom t_zoom
run_case edge_moves t_edge_moves
run_case master_right_rotates t_master_right_rotates
run_case tabbed_stack t_tabbed_stack
run_case long_names t_long_names
if [ "$E2E_SKIP_MULTI" = 1 ]; then
  e2e_info "E2E: multi_output пропущен (E2E_SKIP_MULTI=1)"
else
  run_case multi_output t_multi_output
fi

if [ "$E2E_RUN" -eq 0 ]; then
  e2e_info "E2E: ни один сценарий не выбран (E2E_FILTER='${E2E_FILTER:-}')"
  exit 2
fi

e2e_info ""
e2e_info "E2E: сценариев $E2E_RUN, пропущено $E2E_SKIPPED"
e2e_summary
