#!/usr/bin/env bash
# layout.sh — случайные операции в headless Sway и проверка канона
# master/stack после каждой из них. Ловит редкие структурные дрейфы,
# которые не покрывают детерминированные E2E-сценарии.
#
# Запуск: tests/fuzz/layout.sh [итерации] [seed]
#   FUZZ_WORK=...  каталог логов (по умолчанию mktemp)
set -uo pipefail

HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
export E2E_ROOT=$ROOT
export E2E_BIN=${E2E_BIN:-$ROOT/build/sway-mstack}
export E2E_KEEP=1
export E2E_SETTLE=0.0
export E2E_WORK=${FUZZ_WORK:-${E2E_WORK:-$(mktemp -d "${TMPDIR:-/tmp}/sms-fuzz.XXXXXX")}}
ITERS=${1:-60}
SEED=${2:-1}
case $ITERS in '' | *[!0-9]*)
  printf 'fuzz: итерации должны быть целым числом, получено "%s"\n' "$ITERS" >&2
  exit 2
  ;;
esac
case $SEED in '' | *[!0-9]*)
  printf 'fuzz: seed должен быть целым числом, получено "%s"\n' "$SEED" >&2
  exit 2
  ;;
esac
RANDOM=$SEED
rm -rf "$E2E_WORK"
source "$E2E_ROOT/tests/e2e/lib.sh"
e2e_init || exit 2
e2e_case_begin fuzz || exit 2

FAILED=0
SOFT=0
SPAWNED=0
CHECKS=0

raw_spawn() {
  SPAWNED=$((SPAWNED + 1))
  local app="fuzz-$SPAWNED"
  foot -a "$app" -e sleep 3600 >/dev/null 2>&1 &
  E2E_FOOT_PIDS+=("$!")
  printf '%s' "$app"
}

wait_leaves() {
  local want=$1 i
  for ((i = 0; i < 80; i++)); do
    [ "$(e2e_leaf_count)" -eq "$want" ] && return 0
    sleep 0.05
  done
  return 0
}

alive_apps() { e2e_leaf_apps; }

dump_state() {
  echo "--- ws tree ---"
  e2e_ws_json | jq '{id, name, layout, nodes: [.nodes[] | {id, layout, app_id, marks, rect, kids: [(.nodes//[])[] | {id, layout, app_id, marks, rect}]}]}'
  echo "--- daemon log (tail 80) ---"
  tail -80 "$E2E_DAEMON_LOG"
}

check_layout() {
  local label=$1 ws id layout leaves master
  ws=$(e2e_ws_json)
  [ -n "$ws" ] || return 0
  id=$(jq -r '.id' <<<"$ws")
  layout=$(jq -r '.layout' <<<"$ws")
  leaves=$(e2e_leaf_count)
  CHECKS=$((CHECKS + 1))
  master=$(jq -r --arg m "_m$id" '
    [.. | objects | select(.type? == "con" and any(.marks[]?; . == $m))] | first // empty
    | if . == null or . == "" then "" else (.app_id // "?") end' <<<"$ws")
  local ntop
  ntop=$(jq -r '.nodes | length' <<<"$ws")
  local err="" soft=""
  if [ "$leaves" -ge 2 ]; then
    [ "$layout" = "splith" ] || err="ws layout=$layout (want splith)"
    if [ -z "$err" ]; then
      local top0app top0x top0w top0layout top1layout top1x top1w top1kids
      local totalw top0pct
      top0app=$(jq -r '.nodes[0].app_id // "container"' <<<"$ws")
      top0x=$(jq -r '.nodes[0].rect.x' <<<"$ws")
      top0w=$(jq -r '.nodes[0].rect.width' <<<"$ws")
      top0layout=$(jq -r '.nodes[0].layout // "none"' <<<"$ws")
      top1layout=$(jq -r '.nodes[1].layout // "missing"' <<<"$ws")
      top1x=$(jq -r '.nodes[1].rect.x // -1' <<<"$ws")
      top1w=$(jq -r '.nodes[1].rect.width // 0' <<<"$ws")
      top1kids=$(jq -r '.nodes[1].nodes | length' <<<"$ws")
      [ "$ntop" = "2" ] || err="top-level nodes=$ntop (want 2)"
      [ -z "$err" ] && [ -z "$master" ] && err="no _m$id mark"
      [ -z "$err" ] && [ "$master" != "$top0app" ] && err="master=$master but nodes[0]=$top0app"
      [ -z "$err" ] && [ "$top0x" -gt 2 ] && err="master x=$top0x"
      [ -z "$err" ] && [ "$top0layout" != "none" ] && err="master wrapped in '$top0layout'"
      # Допуск как у демона: MASTER_PCT ± 3% (целочисленный процент,
      # как в proportions_ok), а не «идеальные» 704 px.
      totalw=$((top0w + top1w))
      top0pct=$((totalw > 0 ? top0w * 100 / totalw : 0))
      [ -z "$err" ] && { [ "$top0pct" -lt 52 ] || [ "$top0pct" -gt 58 ]; } &&
        err="master share=${top0pct}% (${top0w}px of ${totalw})"
      [ -z "$err" ] && { [ "$top1x" -lt $((top0w - 2)) ] || [ "$top1x" -gt $((top0w + 2)) ]; } &&
        err="stack x=$top1x (master ends at $top0w)"
      [ -z "$err" ] && { [ "$top1w" -lt $((totalw - top0w - 2)) ] ||
        [ "$top1w" -gt $((totalw - top0w + 2)) ]; } &&
        err="stack width=$top1w (want $((totalw - top0w)))"
      if [ -z "$err" ]; then
        if [ "$leaves" -eq 2 ]; then
          # Канон n=2 плоский: оба окна — прямые соседи, обёртки нет.
          [ "$top1layout" = "none" ] || err="n=2 stack layout=$top1layout (want none)"
          [ -z "$err" ] && [ "$top1kids" -ne 0 ] &&
            err="n=2 stack wrapped ($top1kids kids)"
        else
          [ "$top1layout" = "missing" ] && err="no stack container at nodes[1]"
          [ -z "$err" ] && [ "$top1kids" -lt 2 ] &&
            err="stack container has $top1kids kids (want >=2)"
          if [ -z "$err" ]; then
            case $top1layout in
              splitv|tabbed|stacked) ;;
              *) soft="stack layout=$top1layout" ;;
            esac
          fi
        fi
      fi
      if [ -z "$err" ] && [ -z "$soft" ]; then
        local bad
        bad=$(jq -r --arg m "_m$id" --argjson x "$top1x" '
          [.nodes[1] | .. | objects
           | select(.type? == "con" and ((.nodes? // []) | length) == 0)
           | select((((.floating? // "auto_off") | test("_on$")) | not))
           | select(((.scratchpad_state? // "none") == "none"))
           | select(any(.marks[]?; . == $m) | not)
           | select(((.rect.x - $x) | if . < 0 then -. else . end) > 2)
           | .app_id] | join(" ")' <<<"$ws")
        [ -n "$bad" ] && err="stack leaves off-column: $bad"
      fi
    fi
  elif [ "$leaves" = "1" ]; then
    [ "$layout" = "splith" ] || err="ws layout=$layout (want splith) for n=1"
    [ -z "$err" ] && [ -z "$master" ] && err="no _m$id mark for n=1"
  fi
  if [ -n "$err" ]; then
    FAILED=$((FAILED + 1))
    echo
    echo "### FAIL [$label] iter=$i seed=$SEED leaves=$leaves: $err"
    dump_state
    return 1
  fi
  if [ -n "$soft" ]; then
    SOFT=$((SOFT + 1))
    echo "### WARN [$label] iter=$i seed=$SEED leaves=$leaves: $soft"
  fi
  return 0
}

move_dir() {
  local d=$1 apps app
  apps=$(alive_apps)
  [ -z "$apps" ] && return 0
  app=$(tr ' ' '\n' <<<"$apps" | shuf -n1)
  swaymsg "[app_id=\"$app\"] focus" >/dev/null 2>&1 || true
  sleep 0.03
  swaymsg "move $d" >/dev/null 2>&1 || true
}

# baseline
baseline() {
  local i
  for ((i = 0; i < 3; i++)); do raw_spawn >/dev/null; done
  wait_leaves 3
  sleep 0.6
}

i=0
baseline
check_layout baseline || exit 4

for ((i = 1; i <= ITERS; i++)); do
  op=$((RANDOM % 100))
  if [ "$op" -lt 28 ]; then
    # burst spawn 1..3
    k=$((1 + RANDOM % 3))
    before=$(e2e_leaf_count)
    for ((j = 0; j < k; j++)); do raw_spawn >/dev/null; done
    wait_leaves $((before + k))
  elif [ "$op" -lt 46 ]; then
    # close a random window (sometimes the master)
    apps=$(alive_apps)
    if [ -n "$apps" ]; then
      if [ $((RANDOM % 100)) -lt 45 ]; then
        app=$(e2e_master_app)
        [ -z "$app" ] && app=$(tr ' ' '\n' <<<"$apps" | shuf -n1)
      else
        app=$(tr ' ' '\n' <<<"$apps" | shuf -n1)
      fi
      before=$(e2e_leaf_count)
      swaymsg "[app_id=\"$app\"] kill" >/dev/null 2>&1 || true
      wait_leaves $((before - 1))
    fi
  elif [ "$op" -lt 52 ]; then
    # close master and immediately spawn 1..2
    app=$(e2e_master_app)
    if [ -n "$app" ]; then
      swaymsg "[app_id=\"$app\"] kill" >/dev/null 2>&1 || true
      k=$((1 + RANDOM % 2))
      for ((j = 0; j < k; j++)); do raw_spawn >/dev/null; done
    fi
  elif [ "$op" -lt 70 ]; then
    case $((RANDOM % 4)) in
      0) move_dir left ;;
      1) move_dir right ;;
      2) move_dir up ;;
      3) move_dir down ;;
    esac
  elif [ "$op" -lt 78 ]; then
    # scratchpad round trip on a focused window
    apps=$(alive_apps)
    if [ -n "$apps" ]; then
      app=$(tr ' ' '\n' <<<"$apps" | shuf -n1)
      swaymsg "[app_id=\"$app\"] focus" >/dev/null 2>&1 || true
      sleep 0.03
      swaymsg 'move scratchpad' >/dev/null 2>&1 || true
      sleep 0.1
      swaymsg 'scratchpad show' >/dev/null 2>&1 || true
    fi
  elif [ "$op" -lt 88 ]; then
    # manual layout change on a random window, then maybe spawn
    apps=$(alive_apps)
    if [ -n "$apps" ]; then
      app=$(tr ' ' '\n' <<<"$apps" | shuf -n1)
      swaymsg "[app_id=\"$app\"] focus" >/dev/null 2>&1 || true
      layouts=(splith splitv tabbed stacked)
      swaymsg "layout ${layouts[$((RANDOM % 4))]}" >/dev/null 2>&1 || true
    fi
  elif [ "$op" -lt 94 ]; then
    # empty the workspace or refill it
    if [ $((RANDOM % 10)) -lt 3 ]; then
      apps=$(alive_apps)
      for app in $apps; do
        swaymsg "[app_id=\"$app\"] kill" >/dev/null 2>&1 || true
      done
      wait_leaves 0
      sleep 0.3
      k=$((1 + RANDOM % 3))
      for ((j = 0; j < k; j++)); do raw_spawn >/dev/null; done
    else
      raw_spawn >/dev/null
    fi
  else
    # workspace switch away and back
    e2e_switch_ws 2 >/dev/null 2>&1 || true
    sleep 0.1
    e2e_switch_ws 1 >/dev/null 2>&1 || true
  fi

  # settle and check
  sleep 0.55
  if ! check_layout "iter $i op $op"; then
    echo "fuzz: FAILED at iteration $i (seed $SEED)"
    exit 1
  fi
done

echo "fuzz: $ITERS iterations, $CHECKS checks, $SOFT soft warnings, 0 failures (seed $SEED)"
e2e_case_end
exit 0
