#!/usr/bin/env bash
# run-all.sh — единая точка запуска тестов sway-mstack.
#
# Полный прогон (по умолчанию):
#   - сборка Release и Debug (ASan/UBSan);
#   - unit-тесты в обеих сборках, в Debug — с проверкой утечек;
#   - E2E в обеих сборках, в Debug — с проверкой отчётов санитайзеров;
#   - fuzz-раскладка на нескольких seed'ах;
#   - бенчмарк (информационно, без жёстких порогов).
#
# Быстрый прогон: tests/run-all.sh --quick
#
# Переменные окружения:
#   SMS_TEST_WORK    каталог логов (по умолчанию mktemp)
#   SMS_TEST_KEEP=1  не удалять каталог логов после успешного прогона
#   SMS_FUZZ_SEEDS   seed'ы fuzz (по умолчанию "42 7 99 1")
#   SMS_FUZZ_ITERS   итераций на seed (по умолчанию 60)
#   SMS_JOBS         параллельная сборка (по умолчанию nproc)
set -uo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
BUILD_DIR=${SMS_BUILD_DIR:-$ROOT/build}
DEBUG_DIR=${SMS_DEBUG_BUILD_DIR:-$ROOT/build-debug}
RUN_DIR=${SMS_TEST_WORK:-$(mktemp -d "${TMPDIR:-/tmp}/sms-tests.XXXXXX")}
JOBS=${SMS_JOBS:-$(nproc 2>/dev/null || echo 2)}
QUICK=0
DO_BUILD=1
DO_DEBUG=1
DO_E2E=1
DO_FUZZ=1
DO_BENCH=1
KEEP=${SMS_TEST_KEEP:-0}
FUZZ_SEEDS=${SMS_FUZZ_SEEDS:-"42 7 99 1"}
FUZZ_ITERS=${SMS_FUZZ_ITERS:-60}

PHASES=0
FAILS=0

usage() {
  cat <<'EOF'
Использование: tests/run-all.sh [опции]

  -q, --quick        быстрый прогон: Release unit + E2E + один seed fuzz
      --full         полный прогон (по умолчанию)
      --no-build     не собирать, использовать готовые бинарники
      --no-debug     пропустить Debug-сборку, unit и E2E
      --no-e2e       пропустить E2E
      --no-fuzz      пропустить fuzz
      --no-bench     пропустить бенчмарк
      --bench        включить бенчмарк (в полном прогоне и так включён)
      --fuzz-seeds S seed'ы через пробел (по умолчанию "42 7 99 1")
      --fuzz-iters N итераций fuzz на seed (по умолчанию 60)
      -j, --jobs N   параллельная сборка
  -k, --keep         сохранить каталог логов после успешного прогона
  -h, --help         эта справка
EOF
}

die() {
  printf 'run-all: %s\n' "$*" >&2
  exit 2
}

need_cmd() {
  command -v "$1" >/dev/null 2>&1 || die "не найдена команда '$1' (см. tests/README.md)"
}

while [ $# -gt 0 ]; do
  case $1 in
    -q | --quick)
      QUICK=1
      DO_DEBUG=0
      DO_BENCH=0
      FUZZ_SEEDS=${SMS_FUZZ_SEEDS:-"42"}
      FUZZ_ITERS=${SMS_FUZZ_ITERS:-30}
      ;;
    --full) QUICK=0 ;;
    --no-build) DO_BUILD=0 ;;
    --no-debug) DO_DEBUG=0 ;;
    --no-e2e) DO_E2E=0 ;;
    --no-fuzz) DO_FUZZ=0 ;;
    --no-bench) DO_BENCH=0 ;;
    --bench) DO_BENCH=1 ;;
    --fuzz-seeds)
      shift
      [ $# -gt 0 ] || die "--fuzz-seeds требует значение"
      FUZZ_SEEDS=$1
      ;;
    --fuzz-iters)
      shift
      [ $# -gt 0 ] || die "--fuzz-iters требует число"
      FUZZ_ITERS=$1
      ;;
    -j | --jobs)
      shift
      [ $# -gt 0 ] || die "--jobs требует число"
      JOBS=$1
      ;;
    -k | --keep) KEEP=1 ;;
    -h | --help)
      usage
      exit 0
      ;;
    *) die "неизвестный аргумент: $1 (см. --help)" ;;
  esac
  shift
done

case $JOBS in '' | *[!0-9]*) die "jobs: нужно целое число" ;; esac
case $FUZZ_ITERS in '' | *[!0-9]*) die "fuzz-iters: нужно целое число" ;; esac

mkdir -p "$RUN_DIR" || die "не удалось создать $RUN_DIR"

FAILED_PHASES=()
finish() {
  local rc=$?
  printf '\n===== ИТОГ =====\n'
  if [ "$FAILS" -eq 0 ]; then
    printf 'фаз: %d, провалов: 0\n' "$PHASES"
    if [ "$KEEP" != 1 ]; then
      rm -rf "$RUN_DIR"
      printf 'логи удалены (SMS_TEST_KEEP=1 или --keep, чтобы сохранить)\n'
    else
      printf 'логи: %s\n' "$RUN_DIR"
    fi
  else
    printf 'фаз: %d, провалов: %d\n' "$PHASES" "$FAILS"
    printf 'провалились: %s\n' "${FAILED_PHASES[*]}"
    printf 'логи: %s\n' "$RUN_DIR"
    [ "$rc" -eq 0 ] && rc=1
  fi
  exit "$rc"
}
trap finish EXIT
trap 'exit 130' INT TERM

phase() {
  local name=$1
  shift
  PHASES=$((PHASES + 1))
  printf '\n=== %s ===\n' "$name"
  if "$@" 2>&1 | tee "$RUN_DIR/$name.log"; then
    return 0
  fi
  FAILS=$((FAILS + 1))
  FAILED_PHASES+=("$name")
  return 1
}

build_one() {
  local dir=$1 type=$2
  cmake -S "$ROOT" -B "$dir" -DCMAKE_BUILD_TYPE="$type" -DSMS_E2E=ON || return 1
  cmake --build "$dir" -j "$JOBS" || return 1
}

unit_one() {
  local bin=$1 debug=$2
  [ -x "$bin" ] || {
    printf 'нет исполняемого файла %s\n' "$bin" >&2
    return 1
  }
  if [ "$debug" = 1 ]; then
    ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 \
      UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 "$bin"
  else
    "$bin"
  fi
}

e2e_one() {
  local bin=$1 debug=$2 work=$3
  [ -x "$bin" ] || {
    printf 'нет исполняемого файла %s\n' "$bin" >&2
    return 1
  }
  if [ "$debug" = 1 ]; then
    ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 \
      UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
      E2E_WORK="$work" E2E_KEEP=1 "$ROOT/tests/e2e/run.sh" -b "$bin"
  else
    E2E_WORK="$work" E2E_KEEP=1 "$ROOT/tests/e2e/run.sh" -b "$bin"
  fi
}

sanitizer_logs_clean() {
  local dir=$1 hits
  hits=$(grep -aEi 'AddressSanitizer|LeakSanitizer|runtime error|SUMMARY:' "$dir"/*.log 2>/dev/null | wc -l)
  if [ "$hits" -ne 0 ]; then
    printf 'найдены отчёты санитайзеров (%s строк), см. %s\n' "$hits" "$dir" >&2
    grep -aEi 'AddressSanitizer|LeakSanitizer|runtime error|SUMMARY:' "$dir"/*.log 2>/dev/null | head -20 >&2
    return 1
  fi
  return 0
}

fuzz_one() {
  local bin=$1 seed=$2 work=$3
  [ -x "$bin" ] || {
    printf 'нет исполняемого файла %s\n' "$bin" >&2
    return 1
  }
  FUZZ_WORK="$work" E2E_BIN="$bin" "$ROOT/tests/fuzz/layout.sh" "$FUZZ_ITERS" "$seed"
}

bench_one() {
  local bin=$1 work=$2
  [ -x "$bin" ] || {
    printf 'нет исполняемого файла %s\n' "$bin" >&2
    return 1
  }
  BENCH_WORK="$work" E2E_BIN="$bin" "$ROOT/tests/bench/bench.sh"
}

if [ "$DO_E2E" = 1 ] || [ "$DO_FUZZ" = 1 ]; then
  for c in sway swaymsg foot jq; do need_cmd "$c"; done
fi
[ "$DO_BENCH" = 1 ] && need_cmd python3

if [ "$DO_BUILD" = 1 ]; then
  need_cmd cmake
  phase "build-release" build_one "$BUILD_DIR" Release || exit 1
  if [ "$DO_DEBUG" = 1 ]; then
    phase "build-debug" build_one "$DEBUG_DIR" Debug || exit 1
  fi
fi

phase "unit-release" unit_one "$BUILD_DIR/sms_unit" 0
if [ "$DO_DEBUG" = 1 ]; then
  phase "unit-debug" unit_one "$DEBUG_DIR/sms_unit" 1
fi

if [ "$DO_E2E" = 1 ]; then
  phase "e2e-release" e2e_one "$BUILD_DIR/sway-mstack" 0 "$RUN_DIR/e2e-release"
  if [ "$DO_DEBUG" = 1 ]; then
    phase "e2e-debug" e2e_one "$DEBUG_DIR/sway-mstack" 1 "$RUN_DIR/e2e-debug"
    phase "e2e-debug-sanitizers" sanitizer_logs_clean "$RUN_DIR/e2e-debug"
  fi
fi

if [ "$DO_FUZZ" = 1 ]; then
  for seed in $FUZZ_SEEDS; do
    case $seed in '' | *[!0-9]*) die "fuzz seed должен быть целым: '$seed'" ;; esac
    phase "fuzz-$seed" fuzz_one "$BUILD_DIR/sway-mstack" "$seed" "$RUN_DIR/fuzz-$seed"
  done
fi

if [ "$DO_BENCH" = 1 ]; then
  phase "bench" bench_one "$BUILD_DIR/sway-mstack" "$RUN_DIR/bench"
fi

# finish() печатает итог и выставляет код возврата
