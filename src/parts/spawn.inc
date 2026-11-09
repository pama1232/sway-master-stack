/* spawn.inc — Слепые перехваты и вердикт спавна.
 *
 * Фрагмент единого translation unit: включается из main.c после sms.h.
 * Порядок включения повторяет прежний порядок объявлений в main.c.
 * spawn_fast, death_fast, очередь подглядывания и вердикты. */

#include "../sms.h"

/* ── СЛЕПОЙ ПЕРЕХВАТ СПАВНА: пачка уходит ДО любого чтения дерева —
   успеть до первого кадра. Снимок здорового стола + метки заменяют
   get_tree: свап несёт место И геометрию мастера (нативный тайлинг
   без мигания), снятый мастер — вершиной стопки (dwm). Гвард
   workspace="имя": родилось на чужом столе — пачка молчит, reconcile
   с act.promote расставит на СВОЁМ столе. Гвард tiling на командах
   окна-новичка — страховка от гонки «мс-задержки» вердикта: если
   окно успели всплавить for_window-правилом ДО ИСПОЛНЕНИЯ пачки,
   свап/переметка просто не сработают.

   Стопка пуста (e->n == 0, был только мастер X): N свапнут с mark _m
   на место X; X после swap — отдельный узел справа от N. Пачка сразу
   доделывает канон: mark _s X, split vertical X, resize N в M% ppt.
   Без этих трёх команд reconcile сделал бы их во втором раунде
   после get_tree — лишний кадр и мерцание.

   SIDE-EFFECT MOVE: swap с мастером и move container сдвигают соседей
   (стек, displaced), они дают window::move без нашей записи. Такое
   событие не глотается, а запускает идемпотентный retile; кэш эха
   ставится только на явные move-команды и на них же только
   расходуется. ── */

static bool spawn_fast(int fd, int64_t nid) {
  if (nid <= 0)
    return false;
  Snap *e = snap_get(g_focus_ws);
  if (e == nullptr || e->master <= 0 || e->name[0] == '\0')
    return false; /* нет свежего снимка — честный путь */
  char g[GUARD_MAX];
  if (!ws_guard(g, sizeof g, e->name))
    return false; /* имя не экранируется — честный путь */
  char mk_m[24], mk_s[24], mk_p[24];
  snprintf(mk_m, sizeof mk_m, "_m%" PRId64, g_focus_ws);
  snprintf(mk_s, sizeof mk_s, "_s%" PRId64, g_focus_ws);
  snprintf(mk_p, sizeof mk_p, "_p%" PRId64, g_focus_ws);

  char buf[FAST_BATCH], one[ONE_MAX];
  size_t off = 0;
  buf[0] = '\0';
  snprintf(one, sizeof one,
           "[con_id=%" PRId64 " %s tiling] swap container with mark %s", nid, g,
           mk_m);
  cmd_struct(buf, sizeof buf, &off, one);
  snprintf(one, sizeof one, "[con_id=%" PRId64 " %s tiling] mark --add %s", nid,
           g, mk_m);
  cmd_append(buf, sizeof buf, &off, one);
  /* спавн — смена мастера «со стороны»: память возврата гасим.
     Снимок знает держателя _p: нет его — команда была бы no-op. */
  if (e->partner > 0) {
    snprintf(one, sizeof one, "[con_mark=%s] unmark %s", mk_p, mk_p);
    cmd_append(buf, sizeof buf, &off, one);
  }
  if (e->n >= 1) { /* стопка жива: снятый — вершина */
    snprintf(one, sizeof one,
             "[con_id=%" PRId64 " %s] move container to mark %s", e->master, g,
             mk_s);
    cmd_move(buf, sizeof buf, &off, one, e->master);
    snprintf(one, sizeof one,
             "[con_id=%" PRId64 " %s] swap container with mark %s", e->master,
             g, mk_s);
    cmd_struct(buf, sizeof buf, &off, one);
    /* якорная метка — на снятого мастера (нового верховика) */
    snprintf(one, sizeof one, "[con_id=%" PRId64 " %s] mark --add %s",
             e->master, g, mk_s);
    cmd_append(buf, sizeof buf, &off, one);
  } else {
    /* e->n == 0: X был один; после swap X справа от N — обернуть
       в splitv и пометить _s, затем пропорция N */
    snprintf(one, sizeof one, "[con_id=%" PRId64 " %s] mark --add %s",
             e->master, g, mk_s);
    cmd_append(buf, sizeof buf, &off, one);
    snprintf(one, sizeof one, "[con_id=%" PRId64 " %s] split vertical",
             e->master, g);
    cmd_struct(buf, sizeof buf, &off, one);
    snprintf(one, sizeof one, "[con_id=%" PRId64 " %s] resize set width %d ppt",
             nid, g, MASTER_PCT);
    cmd_append(buf, sizeof buf, &off, one);
  }
  if (g_debug)
    fprintf(stderr, "spawn-fast: ws=%" PRId64 " окно=%" PRId64 "\n", g_focus_ws,
            nid);
  send_cmd(fd, buf);
  snap_evict(g_focus_ws);
  Act act = {.promote = nid, .moved = 0, .died_ws = 0, .died_win = 0};
  /* адресный reconcile по cid новичка: гвард пачки адресован одному
     столу, чужие лечит их собственное следующее событие */
  reconcile_act(fd, &act, nid);
  return true;
}

/* ── СЛЕПАЯ СМЕРТЬ МАСТЕРА: пачка до get_tree — ТОЛЬКО по свежему
   снимку: он даёт id верхушки, имя стола (гвард workspace="^имя$" в
   каждой команде — окно с чужого стола под критерий не попадёт) и
   размер стопки. Стопка жива (n≥2): верхушка (держатель _s) выходит
   в мастер «move left» — обёртка мульти-ребёнок, это побег НАВЕРХ
   (не варп). ПОСЛЕДНЕЕ окно стопки (n==1) — НЕ ДВИГАЕТСЯ ВООБЩЕ:
   move left на синглтоне (единственный ребёнок обёртки-под-ws или
   единственное окно стола) в новых sway УНОСИТ ОКНО НА СОСЕДНИЙ
   ВЫХОД (i3: «treat singleton children as if at workspace level»
   → container_move_to_next_output) — у юзера монитор слева уносил
   выжившее окно к себе. Выжившее и так на весь стол: только метка
   мастера (+фокус, если умерший мастер его держал); канон достроит
   следующий спавн/зом. Снимка нет / имя пусто / мастер был один — честный
   путь с get_tree. ── */

static bool death_fast(int fd, int64_t wsid, int64_t dead_id) {
  if (wsid <= 0)
    return false;
  Snap *e = snap_get(wsid);
  if (e == nullptr || e->n < 1 || e->name[0] == '\0')
    return false; /* нет снимка / мастер был один — честный путь */
  char g[GUARD_MAX];
  if (!ws_guard(g, sizeof g, e->name))
    return false; /* имя не экранируется — честный путь */
  char mk_m[24], mk_s[24], mk_p[24];
  snprintf(mk_m, sizeof mk_m, "_m%" PRId64, wsid);
  snprintf(mk_s, sizeof mk_s, "_s%" PRId64, wsid);
  snprintf(mk_p, sizeof mk_p, "_p%" PRId64, wsid);

  char buf[FAST_BATCH], one[ONE_MAX];
  size_t off = 0;
  buf[0] = '\0';
  int64_t top = e->stack[0];
  if (e->n >= 2) { /* стопка жива: верхушка — в мастер */
    snprintf(one, sizeof one, "[con_id=%" PRId64 " %s] move left", top, g);
    cmd_move(buf, sizeof buf, &off, one, top);
    snprintf(one, sizeof one, "[con_id=%" PRId64 " %s] mark --add %s", top, g,
             mk_m);
    cmd_append(buf, sizeof buf, &off, one);
    snprintf(one, sizeof one, "[con_id=%" PRId64 " %s] resize set width %d ppt",
             top, g, MASTER_PCT);
    cmd_append(buf, sizeof buf, &off, one);
    /* якорная метка — на нового верховика стопки */
    snprintf(one, sizeof one, "[con_id=%" PRId64 " %s] mark --add %s",
             e->stack[1], g, mk_s);
    cmd_append(buf, sizeof buf, &off, one);
  } else {
    /* ПОСЛЕДНЕЕ окно стопки: НЕ ДВИГАЕМ — move left на синглтоне
       (i3-семантика новых sway) = ВАРП НА СОСЕДНИЙ ВЫХОД. Только
       метка мастера (и снять якорь: мастер не держит _s);
       выжившее и так на весь стол, канон достроит следующий спавн.
       Но Sway после смерти мастера оставляет singleton splitv-
       обёртку (upstream #6116): следующий спавн ляжет В НЕЁ и даст
       вертикальный «мастер». split none распускает ровно такого
       родителя (container_flatten) и, в отличие от move, НЕ уводит
       окно на соседний выход. На плоском синглтоне — no-op. */
    snprintf(one, sizeof one, "[con_id=%" PRId64 " %s] mark --add %s", top, g,
             mk_m);
    cmd_append(buf, sizeof buf, &off, one);
    snprintf(one, sizeof one, "[con_id=%" PRId64 " %s] unmark %s", top, g,
             mk_s);
    cmd_append(buf, sizeof buf, &off, one);
    snprintf(one, sizeof one, "[con_id=%" PRId64 " %s] split none", top, g);
    cmd_struct(buf, sizeof buf, &off, one);
  }
  /* память возврата гасим, только если снимок знает живого держателя:
     умерший мастер унёс бы метку с собой, пустой снимок — no-op. */
  if (e->partner > 0 && e->partner != dead_id) {
    snprintf(one, sizeof one, "[con_mark=%s] unmark %s", mk_p, mk_p);
    cmd_append(buf, sizeof buf, &off, one);
  }
  /* фокус был на умершем мастере И юзер ещё на этом столе —
     подсветить нового; иначе не крадём */
  if (g_focus_ws == wsid && g_focus_win == dead_id) {
    snprintf(one, sizeof one, "[con_id=%" PRId64 " %s] focus", top, g);
    cmd_append(buf, sizeof buf, &off, one);
  }
  if (g_debug)
    fprintf(stderr, "death-fast: ws=%" PRId64 "\n", wsid);
  send_cmd(fd, buf);
  snap_evict(wsid);
  reconcile_ws(fd, wsid);
  return true;
}

/* ── ВЕРДИКТ СПАВНА (трей-окна и for_window-правила). Событие
   window::new стреляет В НАЧАЛЕ view_map — ДО применения критериев
   for_window: в JSON события окно всегда «auto_off», отличить
   правило-float от тайлового в момент события НЕЛЬЗЯ. Но sway
   однопотен: он ДОПИШЕТ события того же view_map (floating от
   правила, focus после критериев) в сокет МИКРОСЕКУНДАМИ позже — и
   ТОЛЬКО ПОТОМ возьмётся за нашу пачку. Значит к моменту решения
   вердикт уже лежит в буфере сокета. Слепая пачка по плавающему
   окну — яд: «swap» плавающего с тайловым мастером ОБМЕНЯЕТ их
   состояния (swap_places: плавящееся вталкивается в тайл-слот,
   тайловое — workspace_add_floating) — трей-окно становилось
   тайловым мастером, а ни в чём не повинное окно стопки вылетало
   в floating (жалоба: «выкидывает одно окно в floating»).

   Алгоритм: сперва ОЧЕРЕДЬ (события, вычитанные чужим вердиктом,
   в сокет не вернутся), затем всё, что уже в буфере (не блокируя);
   если мало — до 3×1мс подождать. floating-для-новичка → ПЛАВАЮЩИЙ
   (пачку не шлём вовсе; само событие floating встаёт в очередь и
   его штатный обработчик делает «move position center» + reconcile).
   close-для-новичка → окно умерло (смерть разрулит штатный close из
   очереди). focus-для-новичка → критерии уже отработали и НЕ
   всплавили → тайловый, пачка немедленно. Тишина до конца грации →
   тайловый (страховка — гвард tiling в пачке + reconcile). ── */

enum { SV_TILED = 0, SV_FLOAT = 1, SV_DEAD = 2, SV_UNKNOWN = 3 };

#define PEND_MAX 128
/* Очередь подглядывания хранит уже РАСПАРСЕННЫЙ объект: spawn_drain
   парсит один раз, event_verdict и handle_event читают готовое дерево
   без повторного json_tokener_parse. Утечек нет: caller передаёт
   владение в pend_push, главный цикл делает json_object_put. */
static json_object *g_pend[PEND_MAX];
static uint32_t g_pend_type[PEND_MAX];
static int g_pend_n = 0;

/* Владение ev переходит очереди: caller НЕ освобождает после
   успешного pend_push. При переполнении ev освобождается здесь же. */
static void pend_push(uint32_t type, json_object *ev) {
  if (ev == nullptr)
    return;
  if (g_pend_n >= PEND_MAX) {
    static bool warned = false;
    if (!warned) {
      warned = true;
      fprintf(stderr, "sms: очередь подглядываний переполнена — "
                      "событие потеряно (не должно случаться)\n");
    }
    json_object_put(ev);
    return;
  }
  g_pend[g_pend_n] = ev;
  g_pend_type[g_pend_n] = type;
  g_pend_n++;
}

/* Вердикт одного события окна для новичка nid; -1 = не о нём.
   ev — уже распарсенный (см. pend_push / spawn_drain); не освобождает.
   nid <= 0 отсекается сразу: jint() для отсутствующего container даёт
   -1, и без этой проверки событие без container в очереди ложно
   совпало бы с новичком, у которого тоже нет container. */
static int event_verdict(json_object *ev, int64_t nid) {
  int v = -1;
  if (nid <= 0)
    return v;
  const char *ch = jstr(ev, "change");
  json_object *con = jget(ev, "container");
  int64_t id = (con != nullptr) ? jint(con, "id") : -1;
  if (ch != nullptr && id == nid) {
    if (strcmp(ch, "floating") == 0 && is_floating(con))
      v = SV_FLOAT;
    else if (strcmp(ch, "close") == 0)
      v = SV_DEAD;
    else if (strcmp(ch, "focus") == 0)
      v = SV_TILED; /* критерии отработали — не всплыло */
  }
  return v;
}

/* Вердикт по ОЧЕРЕДИ: floating/focus новичка могли быть вычитаны из
   сокета РАНЬШЕ (чужим вердиктом) и лежать в g_pend — spawn_drain
   их уже не увидит, а именно они решают дело. */
static int pend_verdict(int64_t nid) {
  for (int i = 0; i < g_pend_n; i++) {
    if (g_pend_type[i] != EV_WINDOW)
      continue;
    int v = event_verdict(g_pend[i], nid);
    if (v >= 0)
      return v;
  }
  return -1;
}

/* вычитать доступное без блокировки; -1 = вердикта в буфере нет.
   Событие парсится здесь же: если успех — владение уходит в g_pend;
   если парсинг провалился — событие тихо пропускается (битый JSON
   всё равно бы не обработался). */
static int spawn_drain(int sub_fd, int64_t nid) {
  struct pollfd pf;
  memset(&pf, 0, sizeof pf);
  pf.fd = sub_fd;
  pf.events = POLLIN;
  for (;;) {
    if (g_pend_n >= PEND_MAX)
      break; /* очередь полна — НЕ вычитываем:
                событие останется в сокете и
                дождётся освобождения */
    pf.revents = 0;
    if (poll(&pf, 1, 0) <= 0 || !(pf.revents & POLLIN))
      break;
    uint32_t type;
    size_t len;
    uint8_t *payload = ipc_recv(sub_fd, &type, &len);
    if (payload == nullptr)
      break; /* sway умер — цикл увидит EOF */
    json_object *ev = parse_json_exact((const char *)payload, len);
    free(payload);
    if (ev == nullptr)
      continue; /* битый JSON — пропускаем */
    int v = (type == EV_WINDOW) ? event_verdict(ev, nid) : -1;
    pend_push(type, ev);
    if (v >= 0)
      return v;
  }
  return -1;
}

static int spawn_verdict(int sub_fd, int64_t nid) {
  /* new-событие без container (битый/чужой JSON) — вердикта нет,
     идём честным путём; не жжём 3мс грации */
  if (nid <= 0)
    return SV_UNKNOWN;
  int v = pend_verdict(nid);
  if (v < 0)
    v = spawn_drain(sub_fd, nid);
  if (v >= 0)
    goto out;
  struct pollfd pf;
  memset(&pf, 0, sizeof pf);
  pf.fd = sub_fd;
  pf.events = POLLIN;
  for (int i = 0; i < 3; i++) {
    pf.revents = 0;
    if (poll(&pf, 1, 1) <= 0 || !(pf.revents & POLLIN))
      break;
    v = spawn_drain(sub_fd, nid);
    if (v >= 0)
      goto out;
  }
  /* Тишина — НЕ доказательство тайлового: окно с for_window-переносом
     на другой стол не даёт focus, а слепая пачка по снимку старого
     стола применилась бы наполовину. Идём честным путём get_tree;
     обычный тайловый спавн получает focus-вердикт и остаётся слепым. */
  v = SV_UNKNOWN;
out:
  if (g_debug)
    fprintf(stderr, "spawn-verdict: окно=%" PRId64 " → %s\n", nid,
            v == SV_FLOAT     ? "плавающий"
            : v == SV_DEAD    ? "умер"
            : v == SV_UNKNOWN ? "неизвестно"
                              : "тайловый");
  return v;
}

/* Проверить, есть ли на окне даemon-метка (_m/_s/_p любого стола).
   Нужно в handle_event для move-события: если окно с меткой уехало на
   другой стол, СТАРЫЙ стол остался без мастера/якоря/памяти — адресный
   retile по cid ретилит только НОВЫЙ стол, старый не тронется. Полный
   обход обязателен: вершина старой стопки станет мастером, якорь _s
   переедет на неё, stale-метка на уехавшем окне перестанет быть целью
   слепых команд. */

