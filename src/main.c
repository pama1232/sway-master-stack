/* main.c — DWM-style master/stack tiling daemon для Sway (Sway IPC).
 *
 * Инвариант воркспейса — ровно две зоны:
 *     splith { мастер _m<ws> (M%) | splitv-стопка _s<ws> }
 * Доля мастера выбирается ЗАРАНЕЕ, при запуске: `sms [N]`, N = 10..90 —
 * доля мастера, % (например `sms 60` или `sms 50`); дефолт 55/45.
 * Контроль ±3% на каждом ретайле. Ручного ресайза нет.
 *
 * МОДЕЛЬ — как у dwm (сверено с исходниками, git.suckless.org/dwm):
 * раскладка — ЧИСТАЯ ФУНКЦИЯ порядка дерева. Здоровый стол — 0 команд.
 * Демон НЕ слушает команд: никаких биндов, никакого nop — только
 * события окон и успевание расставить их раньше первого кадра.
 *
 * Правила:
 *   1. НОВОЕ ОКНО — МАСТЕР (dwm attach), ПЕРЕХВАТ ДО ПЕРВОГО КАДРА:
 *      по снимку здорового стола пачка (свап с меткой мастера +
 *      переметки + снятый мастер вершиной стопки) уходит БЕЗ
 *      предварительного get_tree. Свап несёт место И геометрию —
 *      спавн выглядит нативным тайлингом, окно не мигает на чужом
 *      месте. Контрольный reconcile за пачкой проверяет и долечивает.
 *   2. ЗАКРЫТИЕ МАСТЕРА — верхушка стопки становится мастером И
 *      ПОЛУЧАЕТ ФОКУС — но только если умерший мастер его держал
 *      (g_focus_win из window::focus): фокус юзера на окне стопки
 *      не крадём. Слепой пачкой ТОЛЬКО по свежему снимку (гвард
 *      workspace в каждой команде). Мастер был один — фокус
 *      остаётся на опустевшем столе: демон не даёт увести его на
 *      другой выход, новое окно родится здесь.
 *   3. СЫРОЙ move left НА ОКНЕ СТОПКИ (бинды юзера) — ЗУМ: ОДИН свап
 *      с мастером — окно наследует место и геометрию, а бывший
 *      мастер встаёт ТОЧНО на слот зумированного (по снимку порядка;
 *      зум вершины и так точен). Память _p на снятом мастере.
 *   4. СЫРОЙ move right НА МАСТЕРЕ: есть память _p — ТОЧНЫЙ ВОЗВРАТ
 *      (партнёр выходит в мастер, снятый мастер — на его слот);
 *      памяти нет — РОТАЦИЯ с соседом: сосед выходит в мастер,
 *      снятый остаётся вершиной стопки (якорь _s); ротация ставит
 *      память — повторение возвращает всё. Тумблер всегда.
 *   5. Стопка — ОДИН splitv-контейнер; блудные окна съезжают к _s.
 *      move up/down внутри стопки — нативная перестановка (0 команд).
 *      «Крайние» движения (верх-вверх / низ-вниз) ломают ws в
 *      splitv — демон чинит за ОДИН раунд реструктуризацией
 *      «move left» (окно — первый ребёнок ws, ws=splith).
 *      Тот же флип от СЫРОГО move up/down НА МАСТЕРЕ — СВАП с краем
 *      стопки: вверх — с вершиной, вниз — с дном (точный обмен;
 *      sway рассыпает стол в плоскость, ПЕРЕВЕРНУВ стопку — пачка
 *      возвращает ДО-жестовый порядок по снимку/реверсу). Память
 *      _p — на снятом: move right на новом мастере вернёт всё.
 *
 * МЕТКИ (_m/_s/_p/_a): только mark --add и точечный unmark. Метки
 * глобально уникальны — «mark --add» крадёт метку у прежнего
 * держателя, не трогая прочие метки окна (юзерские переживают
 * демон); «--replace», срезающий ВЕСЬ набор меток окна, не
 * используется вовсе.
 *
 * СНИМОК СТОПКИ: после каждого здорового ретайла (0 команд) демон
 * запоминает порядок стола (мастер + стопка сверху вниз + имя ws).
 * Снимок гасит только команда, меняющая порядок/структуру (move/
 * swap/split/layout); чисто меточная пачка (mark/unmark), resize или
 * focus порядок не трогают — снимок остаётся верным. Снимок — ТОЛЬКО
 * ускоритель: нет его — честный путь с get_tree, поведение то же, но
 * на кадр медленнее.
 * Память _p живёт в самом дереве (переживает рестарт демона), гаснет
 * при любой другой смене мастера (спавн/смерть/сырые сдвиги) и при
 * движении партнёра своими биндами — после этого move right на
 * мастере = ротация, никогда ложный возврат.
 *
 * Слепые пачки адресуют команды с гвардом workspace="имя": окно,
 * родившееся на ЧУЖОМ столе (for_window-переносы), под критерий не
 * попадает — пачка молчит, медленный путь расставляет всё как надо.
 * Слепые команды по ГОЛОЙ метке (без снимка и гварда) запрещены:
 * метка живёт в дереве и могла уехать с окном на другой стол —
 * драг мышью её не перемаркировывает.
 *
 * КЭШ ЭХА: каждая move-команда демона порождает ровно один
 * window::move (замерено; swap/mark/resize — ноль). Демон помнит,
 * кого двигал сам, и глотает эти события БЕЗ пересчёта. Записи не
 * копятся вечно: close окна гасит их (echo_forget), неэхнувшие
 * стряхивает TTL. После пачки, менявшей структуру (g_struct_sent),
 * вызывающий путь делает контрольный reconcile.
 *
 * SIDE-EFFECT MOVE: move left X и move container to mark Y сдвигают
 * СОСЕДЕЙ, и те присылают window::move без нашей записи. Такое
 * событие не глотается, а запускает идемпотентный retile (лишний
 * проход безвреден); эхо ставится и расходуется только на явные
 * move-команды демона.
 *
 * Плавающие: при рождении/уходе — в центр; их move-события не читаем
 * (плавающие двигает сам sway — нативно).
 *
 * build: cc -std=c23 -Wall -Wextra -Wpedantic -Wconversion -Wshadow \
 *   -Wformat=2 -Werror=format-security -O2 main.c -ljson-c -o sms
 */

#include "sms.h"

/* Мягкая остановка: SIGTERM/SIGINT — просто выйти (нечего снимать,
   демон ничего не ставил в конфиге юзера). Читается и EINTR-циклами
   I/O — не дожидаться кадра на выходе. */
static volatile sig_atomic_t g_stop = 0;
static void on_stop(int sig) {
  (void)sig;
  g_stop = 1;
}

/* Фрагменты единого translation unit. Порядок = порядок объявлений:
   каждый следующий фрагмент видит определения предыдущих. */
#include "parts/ipc.inc"
#include "parts/json.inc"
#include "parts/layout.inc"
#include "parts/echo.inc"
#include "parts/command.inc"
#include "parts/snapshot.inc"
#include "parts/retile.inc"
#include "parts/spawn.inc"
#include "parts/events.inc"
#include "parts/socket.inc"

int main(int argc, char **argv) {
  signal(SIGPIPE, SIG_IGN);

  struct sigaction sa;
  memset(&sa, 0, sizeof sa);
  sa.sa_handler = on_stop;
  sigemptyset(&sa.sa_mask);
  sigaction(SIGTERM, &sa, nullptr);
  sigaction(SIGINT, &sa, nullptr);

  g_debug = getenv("SMS_DEBUG") != nullptr;

  if (argc > 1) {
    char *end = nullptr;
    long v = strtol(argv[1], &end, 10);
    if (argc > 2 || end == argv[1] || *end != '\0' || v < 10 || v > 90) {
      fprintf(stderr,
              "использование: %s [N]   # N = 10..90, доля мастера, %%\n",
              argv[0]);
      return 1;
    }
    MASTER_PCT = (int)v;
  }

  if (!grab_lock()) {
    fprintf(stderr, "sms: уже запущен другой экземпляр\n");
    return 1;
  }

  int sub_fd = sway_connect();
  if (sub_fd < 0) {
    fprintf(stderr, "sms: нет $SWAYSOCK\n");
    close_lock();
    return 1;
  }
  int cmd_fd = sway_connect();
  if (cmd_fd < 0) {
    close(sub_fd);
    close_lock();
    return 1;
  }
  if (!subscribe_events(sub_fd)) {
    fprintf(stderr, "sms: подписка не удалась\n");
    close(sub_fd);
    close(cmd_fd);
    close_lock();
    return 1;
  }

  printf("sms: запущен (%d/%d)\n", MASTER_PCT, 100 - MASTER_PCT);
  fflush(stdout);

  {
    json_object *tree = get_tree(cmd_fd);
    if (tree != nullptr) {
      json_object *fws = nullptr;
      json_object *leaf = find_focused(tree, nullptr, &fws);
      if (fws != nullptr)
        g_focus_ws = jint(fws, "id");
      if (leaf != nullptr)
        g_focus_win = jint(leaf, "id");
      bool any = retile_all(cmd_fd, tree, tree, &ACT_NONE, 0);
      json_object_put(tree);
      if (any)
        reconcile(cmd_fd);
    }
  }

  struct pollfd pf;
  memset(&pf, 0, sizeof pf);
  pf.fd = sub_fd;
  pf.events = POLLIN;
  for (;;) {
    if (g_pend_n > 0) {
      if (g_stop)
        break;
      uint32_t type = g_pend_type[0];
      json_object *ev = g_pend[0];
      g_pend_n--;
      if (g_pend_n > 0) {
        memmove(g_pend, g_pend + 1, (size_t)g_pend_n * sizeof g_pend[0]);
        memmove(g_pend_type, g_pend_type + 1,
                (size_t)g_pend_n * sizeof g_pend_type[0]);
      }
      if (type == EV_WINDOW)
        handle_event(sub_fd, cmd_fd, ev);
      else if (type == EV_WORKSPACE)
        handle_ws_event(cmd_fd, ev);
      json_object_put(ev);
      continue;
    }
    int r = poll(&pf, 1, -1);
    if (g_stop)
      break;
    if (r < 0) {
      if (errno == EINTR)
        continue;
      break;
    }
    uint32_t type;
    size_t len;
    uint8_t *payload = ipc_recv(sub_fd, &type, &len);
    if (payload == nullptr)
      break;
    json_object *ev = parse_json_exact((const char *)payload, len);
    free(payload);
    if (ev == nullptr)
      continue;
    if (type == EV_WINDOW)
      handle_event(sub_fd, cmd_fd, ev);
    else if (type == EV_WORKSPACE)
      handle_ws_event(cmd_fd, ev);
    json_object_put(ev);
  }

  for (int i = 0; i < g_pend_n; i++)
    json_object_put(g_pend[i]);
  g_pend_n = 0;

  close(sub_fd);
  close(cmd_fd);
  close_lock();
  return 0;
}
