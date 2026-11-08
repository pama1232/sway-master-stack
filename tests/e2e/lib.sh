#!/usr/bin/env bash
# lib.sh — общая обвязка E2E-тестов sway-mstack.
#
# Каждый сценарий получает ЛИЧНУЮ headless-сессию Sway:
#   - свой XDG_RUNTIME_DIR внутри E2E_WORK;
#   - свой SWAYSOCK и WAYLAND_DISPLAY;
#   - свой flock-файл демона (XDG_RUNTIME_DIR/sms.lock).
# Живая сессия пользователя не затрагивается: переменные окружения
# выставляются только внутри этого процесса и его детей.
#
# Требования: sway, swaymsg, foot, jq, собранный build/sway-mstack.

# shellcheck shell=bash

E2E_ROOT=${E2E_ROOT:-$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)}
E2E_BIN=${E2E_BIN:-$E2E_ROOT/build/sway-mstack}
E2E_WORK=${E2E_WORK:-}
E2E_SETTLE=${E2E_SETTLE:-0.45}
E2E_POLL=${E2E_POLL:-0.1}
E2E_TRIES=${E2E_TRIES:-100}
E2E_KEEP=${E2E_KEEP:-0}
E2E_SKIP_MULTI=${E2E_SKIP_MULTI:-0}

E2E_CHECKS=0
E2E_FAILURES=0
E2E_CASE="init"
E2E_CASE_WS="1"
E2E_APP_PREFIX="sms-"
E2E_SWAY_PID=""
E2E_DAEMON_PID=""
E2E_DAEMON_LOG=""
E2E_FOOT_PIDS=()
E2E_CLEANED=0

e2e_info() { printf '%s\n' "$*"; }

e2e_fail() {
  E2E_FAILURES=$((E2E_FAILURES + 1))
  printf '  FAIL %s\n' "$*" >&2
}

e2e_pass() { printf '  ok   %s\n' "$*"; }

e2e_check_eq() {
  local actual=$1 expected=$2 label=${3:-значение}
  E2E_CHECKS=$((E2E_CHECKS + 1))
  if [ "$actual" = "$expected" ]; then
    e2e_pass "$label: $actual"
  else
    e2e_fail "$label: ожидалось '$expected', получено '$actual'"
  fi
}

e2e_check_ne() {
  local actual=$1 unexpected=$2 label=${3:-значение}
  E2E_CHECKS=$((E2E_CHECKS + 1))
  if [ "$actual" != "$unexpected" ]; then
    e2e_pass "$label: $actual"
  else
    e2e_fail "$label: значение не должно быть '$unexpected'"
  fi
}

e2e_check_true() {
  local value=$1 label=${2:-условие}
  E2E_CHECKS=$((E2E_CHECKS + 1))
  case "$value" in
    true|1) e2e_pass "$label" ;;
    *) e2e_fail "$label: получено '$value'" ;;
  esac
}

e2e_check_near() {
  local actual=$1 expected=$2 tolerance=$3 label=${4:-значение}
  local delta=$((actual - expected))
  [ "$delta" -lt 0 ] && delta=$((-delta))
  E2E_CHECKS=$((E2E_CHECKS + 1))
  if [ "$delta" -le "$tolerance" ]; then
    e2e_pass "$label: $actual"
  else
    e2e_fail "$label: ожидалось $expected±$tolerance, получено $actual"
  fi
}

e2e_check_rect() {
  local app=$1 field=$2 expected=$3 tolerance=${4:-0} label=${5:-"$app.$field"}
  local actual
  actual=$(e2e_app_rect "$app" "$field")
  if [ -z "$actual" ]; then
    E2E_CHECKS=$((E2E_CHECKS + 1))
    e2e_fail "$label: окно '$app' не найдено"
    return 0
  fi
  e2e_check_near "$actual" "$expected" "$tolerance" "$label"
}

e2e_init() {
  local missing=0 c
  for c in sway swaymsg foot jq; do
    if ! command -v "$c" >/dev/null 2>&1; then
      printf 'E2E: не найдена команда %s\n' "$c" >&2
      missing=1
    fi
  done
  if [ ! -x "$E2E_BIN" ]; then
    printf 'E2E: нет исполняемого файла %s\n' "$E2E_BIN" >&2
    printf '     соберите: cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build\n' >&2
    missing=1
  fi
  [ "$missing" -eq 0 ] || return 1

  if [ -z "$E2E_WORK" ]; then
    E2E_WORK=$(mktemp -d "${TMPDIR:-/tmp}/sms-e2e.XXXXXX") || return 1
  else
    mkdir -p "$E2E_WORK" || return 1
  fi
  export E2E_WORK

  trap e2e_cleanup EXIT
  trap 'e2e_cleanup; exit 130' INT TERM

  e2e_info "E2E workdir: $E2E_WORK"
  e2e_info "E2E binary:  $E2E_BIN"
  return 0
}

e2e_start_sway() {
  e2e_stop_sway

  rm -rf "$E2E_WORK/run"
  mkdir -p "$E2E_WORK/run" || return 1
  chmod 700 "$E2E_WORK/run" || return 1

  cat > "$E2E_WORK/sway.conf" <<'EOF'
output HEADLESS-1 resolution 1280x720
default_border none
default_floating_border none
gaps inner 0
gaps outer 0
focus_follows_mouse no
workspace 1 output HEADLESS-1
EOF

  export XDG_RUNTIME_DIR="$E2E_WORK/run"
  unset WAYLAND_DISPLAY SWAYSOCK I3SOCK
  export WLR_BACKENDS=headless
  export WLR_LIBINPUT_NO_DEVICES=1
  export WLR_RENDERER=pixman

  sway -c "$E2E_WORK/sway.conf" >"$E2E_WORK/sway.log" 2>&1 &
  E2E_SWAY_PID=$!

  local i sock="" wl=""
  for ((i = 0; i < E2E_TRIES; i++)); do
    sock=$(find "$XDG_RUNTIME_DIR" -maxdepth 1 -type s \
      -name 'sway-ipc.*.sock' 2>/dev/null | head -1)
    if [ -n "$sock" ] &&
      SWAYSOCK="$sock" swaymsg -t get_version >/dev/null 2>&1; then
      break
    fi
    if ! kill -0 "$E2E_SWAY_PID" 2>/dev/null; then
      e2e_fail "sway завершился при старте"
      sed -n '1,40p' "$E2E_WORK/sway.log" >&2
      E2E_SWAY_PID=""
      return 1
    fi
    sleep "$E2E_POLL"
  done
  if [ -z "$sock" ]; then
    e2e_fail "sway IPC не поднялся"
    sed -n '1,40p' "$E2E_WORK/sway.log" >&2
    return 1
  fi
  export SWAYSOCK="$sock"

  for ((i = 0; i < E2E_TRIES; i++)); do
    wl=$(find "$XDG_RUNTIME_DIR" -maxdepth 1 -type s \
      -name 'wayland-*' ! -name '*.lock' 2>/dev/null | head -1)
    [ -n "$wl" ] && break
    sleep "$E2E_POLL"
  done
  if [ -z "$wl" ]; then
    e2e_fail "wayland-сокет не появился"
    return 1
  fi
  export WAYLAND_DISPLAY
  WAYLAND_DISPLAY=$(basename "$wl")

  return 0
}

e2e_stop_sway() {
  if [ -n "${E2E_SWAY_PID:-}" ] && kill -0 "$E2E_SWAY_PID" 2>/dev/null; then
    kill "$E2E_SWAY_PID" 2>/dev/null || true
    local i
    for ((i = 0; i < 40; i++)); do
      kill -0 "$E2E_SWAY_PID" 2>/dev/null || break
      sleep 0.05
    done
    kill -9 "$E2E_SWAY_PID" 2>/dev/null || true
    wait "$E2E_SWAY_PID" 2>/dev/null || true
  fi
  E2E_SWAY_PID=""
  unset SWAYSOCK WAYLAND_DISPLAY I3SOCK 2>/dev/null || true
}

e2e_start_daemon() {
  local pct=${1:-}
  e2e_stop_daemon

  E2E_DAEMON_LOG="$E2E_WORK/daemon-$E2E_CASE.log"
  if [ -n "$pct" ]; then
    SMS_DEBUG=1 "$E2E_BIN" "$pct" >"$E2E_DAEMON_LOG" 2>&1 &
  else
    SMS_DEBUG=1 "$E2E_BIN" >"$E2E_DAEMON_LOG" 2>&1 &
  fi
  E2E_DAEMON_PID=$!

  local i
  for ((i = 0; i < E2E_TRIES; i++)); do
    if grep -q 'sms: запущен' "$E2E_DAEMON_LOG" 2>/dev/null; then
      return 0
    fi
    if ! kill -0 "$E2E_DAEMON_PID" 2>/dev/null; then
      e2e_fail "демон завершился при старте"
      sed -n '1,40p' "$E2E_DAEMON_LOG" >&2
      E2E_DAEMON_PID=""
      return 1
    fi
    sleep "$E2E_POLL"
  done
  e2e_fail "демон не напечатал 'sms: запущен'"
  sed -n '1,40p' "$E2E_DAEMON_LOG" >&2
  return 1
}

e2e_stop_daemon() {
  if [ -n "${E2E_DAEMON_PID:-}" ] && kill -0 "$E2E_DAEMON_PID" 2>/dev/null; then
    kill -TERM "$E2E_DAEMON_PID" 2>/dev/null || true
    local i
    for ((i = 0; i < 40; i++)); do
      kill -0 "$E2E_DAEMON_PID" 2>/dev/null || break
      sleep 0.05
    done
    kill -9 "$E2E_DAEMON_PID" 2>/dev/null || true
    wait "$E2E_DAEMON_PID" 2>/dev/null || true
  fi
  E2E_DAEMON_PID=""
}

e2e_kill_foot() {
  local pid
  for pid in "${E2E_FOOT_PIDS[@]:-}"; do
    [ -n "$pid" ] && kill "$pid" 2>/dev/null || true
  done
  for pid in "${E2E_FOOT_PIDS[@]:-}"; do
    [ -n "$pid" ] && wait "$pid" 2>/dev/null || true
  done
  E2E_FOOT_PIDS=()
}

e2e_cleanup() {
  [ "$E2E_CLEANED" = 1 ] && return 0
  E2E_CLEANED=1
  e2e_stop_daemon
  e2e_kill_foot
  e2e_stop_sway
  if [ "$E2E_KEEP" != 1 ] && [ -n "${E2E_WORK:-}" ]; then
    rm -rf "$E2E_WORK"
  elif [ -n "${E2E_WORK:-}" ]; then
    printf 'E2E: логи сохранены в %s\n' "$E2E_WORK"
  fi
}

e2e_case_begin() {
  E2E_CASE=$1
  E2E_CASE_WS="1"
  E2E_APP_PREFIX="sms-${E2E_CASE}-"
  E2E_CASE_FAILURES=$E2E_FAILURES
  e2e_info ""
  e2e_info "=== $E2E_CASE ==="
  e2e_start_sway || return 1
  e2e_start_daemon || return 1
  e2e_switch_ws 1 || return 1
  e2e_settle
  return 0
}

e2e_case_end() {
  e2e_stop_daemon
  e2e_kill_foot
  e2e_stop_sway
  if [ "$E2E_FAILURES" -gt "$E2E_CASE_FAILURES" ]; then
    e2e_info "--- $E2E_CASE: FAIL ---"
  else
    e2e_info "--- $E2E_CASE: ok ---"
  fi
}

e2e_summary() {
  printf '\nE2E: %d проверок, %d провалов\n' "$E2E_CHECKS" "$E2E_FAILURES"
  [ "$E2E_FAILURES" -eq 0 ]
}

e2e_settle() { sleep "${1:-$E2E_SETTLE}"; }

# ── Sway / окна ──

e2e_current_ws() {
  swaymsg -t get_workspaces -r 2>/dev/null |
    jq -r '.[] | select(.focused) | .name' 2>/dev/null
}

e2e_switch_ws() {
  local ws=$1 i
  swaymsg workspace "$ws" >/dev/null 2>&1 || true
  for ((i = 0; i < E2E_TRIES; i++)); do
    if [ "$(e2e_current_ws)" = "$ws" ]; then
      E2E_CASE_WS=$ws
      return 0
    fi
    sleep "$E2E_POLL"
  done
  e2e_fail "не удалось переключиться на workspace '$ws'"
  return 1
}

e2e_rename_ws() {
  local name=$1 i
  swaymsg rename workspace to "$name" >/dev/null 2>&1 || true
  for ((i = 0; i < E2E_TRIES; i++)); do
    if [ "$(e2e_current_ws)" = "$name" ]; then
      E2E_CASE_WS=$name
      return 0
    fi
    sleep "$E2E_POLL"
  done
  e2e_fail "не удалось переименовать workspace в '$name'"
  return 1
}

e2e_app() { printf '%s%s' "$E2E_APP_PREFIX" "$1"; }

e2e_tree() { swaymsg -t get_tree -r 2>/dev/null; }

e2e_ws_json() {
  local ws=${1:-$E2E_CASE_WS}
  e2e_tree |
    jq -c --arg ws "$ws" \
      'first(.. | objects | select(.type? == "workspace" and .name == $ws)) // empty' \
      2>/dev/null
}

e2e_ws_id() {
  local j
  j=$(e2e_ws_json "${1:-$E2E_CASE_WS}")
  [ -n "$j" ] || return 0
  jq -r '.id // empty' <<<"$j"
}

e2e_ws_layout() {
  local j
  j=$(e2e_ws_json "${1:-$E2E_CASE_WS}")
  [ -n "$j" ] || return 0
  jq -r '.layout // empty' <<<"$j"
}

e2e_app_json_any() {
  local app=$1
  e2e_tree |
    jq -c --arg app "$app" \
      '[.. | objects | select(.app_id? == $app)] | first // empty' \
      2>/dev/null
}

e2e_app_json() {
  local app=$1 ws=${2:-$E2E_CASE_WS} j
  j=$(e2e_ws_json "$ws")
  [ -n "$j" ] || return 0
  jq -c --arg app "$app" \
    '[.. | objects | select(.app_id? == $app)] | first // empty' <<<"$j" \
    2>/dev/null
}

e2e_app_rect() {
  local app=$1 field=$2 j
  j=$(e2e_app_json_any "$app")
  [ -n "$j" ] || return 0
  jq -r --arg f "$field" '.rect[$f] // empty' <<<"$j" 2>/dev/null
}

e2e_app_ws_name() {
  local app=$1
  e2e_tree |
    jq -r --arg app "$app" '
      [.. | objects
       | select(.type? == "workspace" and ((.name | startswith("__")) | not)) as $ws
       | $ws | .. | objects | select(.app_id? == $app) | $ws.name]
      | first // empty' 2>/dev/null
}

e2e_app_output() {
  local app=$1
  e2e_tree |
    jq -r --arg app "$app" '
      [.. | objects
       | select(.type? == "workspace" and ((.name | startswith("__")) | not)) as $ws
       | $ws | .. | objects | select(.app_id? == $app) | $ws.output]
      | first // empty' 2>/dev/null
}

e2e_leaf_apps() {
  local ws=${1:-$E2E_CASE_WS} j
  j=$(e2e_ws_json "$ws")
  [ -n "$j" ] || return 0
  jq -r '
    [.. | objects
     | select(.type? == "con"
              and (((.nodes? // []) | length) == 0)
              and ((((.floating? // "auto_off") | test("_on$")) | not))
              and ((.scratchpad_state? // "none") == "none"))
     | .app_id // empty]
    | join(" ")' <<<"$j" 2>/dev/null
}

e2e_master_app() {
  local ws=${1:-$E2E_CASE_WS} j mark
  mark="_m$(e2e_ws_id "$ws")"
  j=$(e2e_ws_json "$ws")
  [ -n "$j" ] || return 0
  jq -r --arg mark "$mark" '
    [.. | objects
     | select(.type? == "con" and any(.marks[]?; . == $mark))
     | .app_id // empty]
    | first // empty' <<<"$j" 2>/dev/null
}

e2e_stack_apps() {
  local ws=${1:-$E2E_CASE_WS} j mark
  mark="_m$(e2e_ws_id "$ws")"
  j=$(e2e_ws_json "$ws")
  [ -n "$j" ] || return 0
  jq -r --arg mark "$mark" '
    [.. | objects
     | select(.type? == "con"
              and (((.nodes? // []) | length) == 0)
              and ((((.floating? // "auto_off") | test("_on$")) | not))
              and ((.scratchpad_state? // "none") == "none")
              and (((any(.marks[]?; . == $mark)) | not)))
     | .app_id // empty]
    | join(" ")' <<<"$j" 2>/dev/null
}

e2e_stack_layout() {
  local ws=${1:-$E2E_CASE_WS} j
  j=$(e2e_ws_json "$ws")
  [ -n "$j" ] || return 0
  jq -r '[.nodes[]? | select((((.nodes? // []) | length) > 0)) | .layout][0] // "none"' \
    <<<"$j" 2>/dev/null
}

e2e_leaf_count() {
  local n
  n=$(e2e_ws_json "${1:-$E2E_CASE_WS}" |
    jq '[.. | objects
         | select(.type? == "con"
                  and (((.nodes? // []) | length) == 0)
                  and ((((.floating? // "auto_off") | test("_on$")) | not))
                  and ((.scratchpad_state? // "none") == "none"))]
        | length' 2>/dev/null)
  printf '%s' "${n:-0}"
}

e2e_wait_app() {
  local app=$1 i
  for ((i = 0; i < E2E_TRIES; i++)); do
    [ -n "$(e2e_app_json_any "$app")" ] && return 0
    sleep "$E2E_POLL"
  done
  e2e_fail "окно '$app' не появилось"
  return 1
}

e2e_wait_gone() {
  local app=$1 i
  for ((i = 0; i < E2E_TRIES; i++)); do
    [ -z "$(e2e_app_json_any "$app")" ] && return 0
    sleep "$E2E_POLL"
  done
  e2e_fail "окно '$app' не закрылось"
  return 1
}

e2e_wait_leaf() {
  local want=$1 i
  for ((i = 0; i < E2E_TRIES; i++)); do
    [ "$(e2e_leaf_count)" -eq "$want" ] && return 0
    sleep "$E2E_POLL"
  done
  e2e_fail "на столе '$E2E_CASE_WS' окон $(e2e_leaf_count), ожидалось $want"
  return 1
}

e2e_wait_master() {
  local app=$1 i
  for ((i = 0; i < E2E_TRIES; i++)); do
    [ "$(e2e_master_app)" = "$app" ] && return 0
    sleep "$E2E_POLL"
  done
  e2e_fail "мастер не стал '$app' (сейчас '$(e2e_master_app)')"
  return 1
}

e2e_wait_stack_layout() {
  local want=$1 i
  for ((i = 0; i < E2E_TRIES; i++)); do
    [ "$(e2e_stack_layout)" = "$want" ] && return 0
    sleep "$E2E_POLL"
  done
  e2e_fail "layout стопки не стал '$want' (сейчас '$(e2e_stack_layout)')"
  return 1
}

e2e_wait_app_ws() {
  local app=$1 ws=$2 i
  for ((i = 0; i < E2E_TRIES; i++)); do
    [ "$(e2e_app_ws_name "$app")" = "$ws" ] && return 0
    sleep "$E2E_POLL"
  done
  e2e_fail "окно '$app' не на workspace '$ws' (сейчас '$(e2e_app_ws_name "$app")')"
  return 1
}

e2e_spawn() {
  local n=$1 app
  app=$(e2e_app "$n")
  foot -a "$app" -e sleep 3600 >/dev/null 2>&1 &
  E2E_FOOT_PIDS+=("$!")
  e2e_wait_app "$app" || return 1
  e2e_settle
  return 0
}

e2e_spawn_until() {
  local n=$1 want=$2
  e2e_spawn "$n" || return 1
  e2e_wait_leaf "$want" || return 1
  e2e_settle
  return 0
}

e2e_focus_app() {
  local app=$1 i j focused
  swaymsg "[app_id=\"$app\"] focus" >/dev/null 2>&1 || true
  for ((i = 0; i < E2E_TRIES; i++)); do
    j=$(e2e_app_json_any "$app")
    if [ -n "$j" ]; then
      focused=$(jq -r '.focused // false' <<<"$j" 2>/dev/null)
      if [ "$focused" = true ]; then
        e2e_settle 0.2
        return 0
      fi
    fi
    sleep "$E2E_POLL"
  done
  e2e_fail "не удалось сфокусировать '$app'"
  return 1
}

e2e_close_app() {
  local app=$1
  swaymsg "[app_id=\"$app\"] kill" >/dev/null 2>&1 || true
  e2e_wait_gone "$app" || return 1
  e2e_settle
  return 0
}

e2e_move() {
  swaymsg "move $1" >/dev/null 2>&1 || true
  e2e_settle
}

e2e_create_output() {
  local before after out i ws_on_out
  before=$(swaymsg -t get_outputs -r 2>/dev/null | jq 'length' 2>/dev/null || echo 0)
  swaymsg create_output >/dev/null 2>&1 || true
  for ((i = 0; i < E2E_TRIES; i++)); do
    after=$(swaymsg -t get_outputs -r 2>/dev/null | jq 'length' 2>/dev/null || echo 0)
    [ "${after:-0}" -ge "$((before + 1))" ] && break
    sleep "$E2E_POLL"
  done
  out=$(swaymsg -t get_outputs -r 2>/dev/null |
    jq -r '[.[] | select(.active and .name != "HEADLESS-1")][0].name // empty' \
      2>/dev/null)
  if [ -z "$out" ]; then
    e2e_fail "второй выход не создался (swaymsg create_output)"
    return 1
  fi
  swaymsg output "$out" resolution 1280x720 >/dev/null 2>&1 || true
  for ((i = 0; i < E2E_TRIES; i++)); do
    ws_on_out=$(swaymsg -t get_workspaces -r 2>/dev/null |
      jq -r --arg out "$out" '[.[] | select(.output == $out)][0].name // empty' \
        2>/dev/null)
    if [ -n "$ws_on_out" ]; then
      printf '%s' "$out"
      return 0
    fi
    sleep "$E2E_POLL"
  done
  e2e_fail "на выходе '$out' нет workspace"
  return 1
}
