/* echo.inc — Кэш эха собственных move-команд.
 *
 * Фрагмент единого translation unit: включается из main.c после sms.h.
 * Порядок включения повторяет прежний порядок объявлений в main.c.
 * TTL, откат неудачных пачек и точечное снятие записей. */

#include "../sms.h"

/* ── Кэш эха: окна, которые ДВИГАЛИ наши команды. Их window::move —
   наше собственное эхо (одна move-команда = один window::move,
   замерено; swap/mark/resize эха не дают) — глотаем без пересчёта.
   Провалившаяся команда не эхает: её записи откатываются по reply
   ("success": false), иначе застрявший id проглотил бы будущий ход
   юзера. Close окна гасит записи (echo_forget), протухшее
   echo_consume стряхивает на месте.

   TTL — параметр записи, а не глобальная константа:
   - обычные наши move-команды — 500мс (эхо приходит за мс);
   - короткая move-команда «переброс на соседний выход» — 100мс:
     если Sway её проигнорировал, запись не должна долго висеть
     и глотать следующий жест юзера.
   Событие по окну под фокусом с короткой записью НЕ глотаем:
   move у Sway действует по фокусу, поэтому это скорее быстрый
   жест юзера (см. echo_consume).
   ── */

constexpr int64_t ECHO_TTL_EXPLICIT_MS = 500;
constexpr int64_t ECHO_TTL_SHORT_MS = 100;

static int64_t g_focus_ws = 0;  /* ws сфокусированного окна */
static int64_t g_focus_win = 0; /* сфокусированное окно (window::focus):
                                   смерть мастера отдаёт фокус новому,
                                   только если умерший его держал;
                                   сбрасывается при смене стола — на
                                   невидимом столе «фокус» неактуален,
                                   а window::focus при возврате
                                   восстановит */

typedef struct {
  int64_t id;
  int64_t t;    /* now_ms() записи */
  int64_t ttl;  /* explicit/short — разные окна */
  uint64_t seq; /* порядковый номер записи (откат неудачной пачки) */
} Echo;

static Echo g_echo[ECHO_MAX];
static int echo_n = 0;
static uint64_t g_echo_seq = 0;      /* счётчик записей */
static uint64_t g_echo_batch_lo = 0; /* seq до первой команды пачки */

static int64_t now_ms(void) {
  struct timespec ts = {0, 0};
  if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
    return 0; /* монотонные часы не должны падать; без мусора в ts */
  return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void echo_prune(void) {
  int64_t now = now_ms();
  int w = 0;
  for (int i = 0; i < echo_n; i++)
    if (now - g_echo[i].t < g_echo[i].ttl)
      g_echo[w++] = g_echo[i];
  echo_n = w;
}

static void echo_track_ttl(int64_t id, int64_t ttl) {
  if (id <= 0)
    return;
  if (echo_n >= ECHO_MAX)
    echo_prune(); /* стряхнуть протухшее */
  if (echo_n >= ECHO_MAX) {
    static bool warned = false;
    if (!warned) {
      warned = true;
      fprintf(stderr,
              "sms: переполнение кэша эха (%d) — лишнее "
              "эхо даст лишний ретайл, самолечится\n",
              ECHO_MAX);
    }
    return;
  }
  g_echo[echo_n].id = id;
  g_echo[echo_n].t = now_ms();
  g_echo[echo_n].ttl = ttl;
  g_echo[echo_n].seq = ++g_echo_seq;
  echo_n++;
}

/* Sway отклонил команды пачки — их window::move не придут. Записи,
   добавленные этой пачкой, иначе провисят TTL и могут съесть
   следующий жест юзера (та самая редкая поломка сетки). */
static void echo_rollback(uint64_t lo) {
  int w = 0;
  for (int i = 0; i < echo_n; i++)
    if (g_echo[i].seq <= lo)
      g_echo[w++] = g_echo[i];
  echo_n = w;
}

/* Обычная move-команда отменяет короткую запись про это же окно:
   две записи на один id = два ожидаемых события, но Sway шлёт одно
   (наше), и вторая съедает СЛЕДУЮЩИЙ жест юзера. */
static void echo_drop_implicit(int64_t id) {
  for (int i = echo_n - 1; i >= 0; i--)
    if (g_echo[i].id == id && g_echo[i].ttl != ECHO_TTL_EXPLICIT_MS)
      g_echo[i] = g_echo[--echo_n];
}

static void echo_track(int64_t id) {
  echo_drop_implicit(id);
  echo_track_ttl(id, ECHO_TTL_EXPLICIT_MS);
}

/* С КОНЦА: записи одного id идут подряд (несколько move по одному
   окну); swap-remove кладёт последний элемент на место i, поэтому
   прямой цикл его бы пропустил. Съедаем РОВНО ОДНУ запись (одна
   команда = одно эхо) и заодно стряхиваем протухшие. */
static bool echo_consume(int64_t id) {
  int64_t now = now_ms();
  for (int i = echo_n - 1; i >= 0; i--) {
    if (now - g_echo[i].t >= g_echo[i].ttl) {
      g_echo[i] = g_echo[--echo_n]; /* протухло — стряхиваем */
      continue;
    }
    if (g_echo[i].id != id)
      continue;
    /* Короткая запись (переброс на соседний выход) — не факт, что
       Sway её выполнил. Если окно под фокусом, это скорее быстрый
       жест юзера (move действует по фокусу): не глотаем.
       Лишний ретайл идемпотентен, а пропущенный жест оставил бы
       сетку сломанной до следующего события. */
    if (id == g_focus_win && g_echo[i].ttl != ECHO_TTL_EXPLICIT_MS) {
      g_echo[i] = g_echo[--echo_n];
      return false;
    }
    g_echo[i] = g_echo[--echo_n];
    return true;
  }
  return false;
}

/* окно умерло — его эха уже не будет, записи гасим; с конца — по той
   же причине: swap-remove + подряд идущие дубли */
static void echo_forget(int64_t id) {
  for (int i = echo_n - 1; i >= 0; i--)
    if (g_echo[i].id == id)
      g_echo[i] = g_echo[--echo_n];
}


