#!/usr/bin/env bash
# bench.sh — воспроизводимый стенд sway-mstack.
#
# Профиль: 5 окон, 20 циклов «зум второго сверху окна стопки -> возврат».
# Метрики: IPC-пачки/команды, GET_TREE, байты debug-лога, CPU-тики, RSS,
# простой 10 с и задержка move->смена мастера.
#
#   tests/bench/bench.sh                       # полный прогон
#   CYCLES=0 tests/bench/bench.sh              # только простой/латентность
#   E2E_BIN=build-debug/sway-mstack tests/bench/bench.sh
set -uo pipefail

HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
E2E_ROOT=${E2E_ROOT:-$(cd "$HERE/../.." && pwd)}
EXPECTED=${EXPECTED:-5}
CYCLES=${CYCLES:-20}
LAT_CYCLES=${LAT_CYCLES:-10}
SMS_BIN=${E2E_BIN:-$E2E_ROOT/build/sway-mstack}
export E2E_ROOT E2E_BIN=$SMS_BIN
export E2E_WORK=${BENCH_WORK:-${E2E_WORK:-$(mktemp -d "${TMPDIR:-/tmp}/sms-bench.XXXXXX")}}
export E2E_SETTLE=${E2E_SETTLE:-0.4}
export E2E_KEEP=1
rm -rf "$E2E_WORK"
mkdir -p "$E2E_WORK"
source "$E2E_ROOT/tests/e2e/lib.sh"
e2e_init || exit 2
e2e_case_begin bench || exit 3

CLK=$(getconf CLK_TCK)
cpu_ticks() { awk '{print $14 + $15}' "/proc/$E2E_DAEMON_PID/stat"; }
rss_kb() { awk '/^VmRSS:/ {print $2}' "/proc/$E2E_DAEMON_PID/status"; }
log_metrics() {
  local log=$1
  local batches cmds trees bytes
  batches=$(grep -c '^cmd:' "$log" 2>/dev/null || true)
  cmds=$(awk '/^cmd:/ {n += gsub(/;/, ";") + 1} END {print n + 0}' "$log")
  trees=$(grep -c '^tree: read' "$log" 2>/dev/null || true)
  bytes=$(wc -c <"$log")
  printf 'batches=%s cmds=%s get_tree=%s log_bytes=%s\n' \
    "$batches" "$cmds" "$trees" "$bytes"
}

# ── прогрев: 5 окон ──
for i in $(seq 1 "$EXPECTED"); do
  e2e_spawn_until "$i" "$i" || exit 3
done
e2e_wait_master "$(e2e_app "$EXPECTED")" || exit 3
e2e_settle 0.6
echo "canonical: master=$(e2e_master_app) stack=[$(e2e_stack_apps)]"

# ── рабочий профиль ──
if [ "$CYCLES" -gt 0 ]; then
  : >"$E2E_DAEMON_LOG"
  cpu0=$(cpu_ticks)
  t0=$(date +%s%N)
  for ((c = 0; c < CYCLES; c++)); do
    target=$(e2e_stack_apps | awk '{print $2}')
    [ -n "$target" ] || {
      echo "нет второго окна стопки" >&2
      exit 3
    }
    e2e_focus_app "$target" || exit 3
    e2e_move left
    e2e_wait_master "$target" || exit 3
    e2e_move right
    e2e_wait_master "$(e2e_app "$EXPECTED")" || exit 3
  done
  t1=$(date +%s%N)
  cpu1=$(cpu_ticks)
  wall_ms=$(((t1 - t0) / 1000000))
  echo "workload: cycles=$CYCLES wall=${wall_ms}ms cpu=$(((cpu1 - cpu0) * 1000 / CLK))ms"
  log_metrics "$E2E_DAEMON_LOG"
  cp "$E2E_DAEMON_LOG" "$E2E_WORK/daemon-workload.log"
fi

# ── простой 10 с (с debug-логом) ──
: >"$E2E_DAEMON_LOG"
cpu0=$(cpu_ticks)
sleep 10
cpu1=$(cpu_ticks)
rss=$(rss_kb)
echo "idle10s debug: cpu=$(((cpu1 - cpu0) * 1000 / CLK))ms rss=${rss}KB"
log_metrics "$E2E_DAEMON_LOG"
cp "$E2E_DAEMON_LOG" "$E2E_WORK/daemon-idle-debug.log"

# ── простой 10 с (без debug: реальный режим) ──
e2e_stop_daemon
"$E2E_BIN" >"$E2E_WORK/daemon-quiet.log" 2>&1 &
E2E_DAEMON_PID=$!
for ((i = 0; i < 100; i++)); do
  grep -q 'sms: запущен' "$E2E_WORK/daemon-quiet.log" 2>/dev/null && break
  sleep 0.05
done
sleep 0.3
cpu0=$(cpu_ticks)
sleep 10
cpu1=$(cpu_ticks)
rss=$(rss_kb)
echo "idle10s quiet: cpu=$(((cpu1 - cpu0) * 1000 / CLK))ms rss=${rss}KB"

# ── задержка move -> смена _m ──
if [ "$LAT_CYCLES" -gt 0 ]; then
  command -v python3 >/dev/null 2>&1 || {
    echo "bench: для задержек нужен python3" >&2
    exit 3
  }
  python3 "$HERE/latency.py" "$SWAYSOCK" "$LAT_CYCLES" || exit 3
fi

e2e_case_end
exit 0
