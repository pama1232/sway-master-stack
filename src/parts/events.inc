/* events.inc — Обработчики событий окна и воркспейса.
 *
 * Фрагмент единого translation unit: включается из main.c после sms.h.
 * Порядок включения повторяет прежний порядок объявлений в main.c.
 * Эхо/спавн/смерть/move, трекинг стола и поиск фокуса. */

#include "../sms.h"

static bool is_marked_daemon(json_object *con) {
  json_object *marks = jarr(con, "marks");
  if (marks == nullptr)
    return false;
  size_t mc = json_object_array_length(marks);
  for (size_t mi = 0; mi < mc; mi++) {
    json_object *m = json_object_array_get_idx(marks, mi);
    const char *s = m != nullptr ? json_object_get_string(m) : nullptr;
    if (s != nullptr && s[0] == '_' &&
        (s[1] == 'm' || s[1] == 's' || s[1] == 'p'))
      return true;
  }
  return false;
}

/* ── События окон: эхо глотаем, спавн/смерть мастера — слепые
   перехваты, остальное — ребилд + reconcile. ev — уже распарсенный
   (владение у caller, не освобождаем). ── */

static void handle_event(int sub_fd, int cmd_fd, json_object *ev) {
  if (ev == nullptr)
    return;

  Act act = {.promote = 0, .moved = 0, .died_ws = 0, .died_win = 0};
  bool need_retile = false;
  bool need_center = false;
  bool closed = false;
  bool full_retile = false; /* move мастера или уход в floating/скретчпад —
                               обход ВСЕХ столов */
  int64_t cid = -1;
  const char *change = jstr(ev, "change");
  json_object *con = jget(ev, "container");
  if (con != nullptr)
    cid = jint(con, "id");

  if (g_debug && change != nullptr)
    fprintf(stderr, "ev: %s id=%" PRId64 "\n", change, cid);

  if (change != nullptr) {
    if (strcmp(change, "new") == 0) {
      if (is_floating(con)) {
        need_center = true;
      } else {
        /* вердикт нужен ТОЛЬКО слепому пути */
        Snap *pre = snap_get(g_focus_ws);
        if (pre == nullptr || pre->master <= 0 || pre->name[0] == '\0') {
          need_retile = true;
          act.promote = cid;
        } else {
          int v = spawn_verdict(sub_fd, cid);
          if (v == SV_FLOAT || v == SV_DEAD) {
            /* пачки нет; floating/close уже в очереди */
          } else if (v != SV_TILED || !spawn_fast(cmd_fd, cid)) {
            /* нет доказательства, что новичок на g_focus_ws (тишина,
               битый JSON или отказ пачки) — честный путь */
            need_retile = true;
            act.promote = cid;
          }
        }
      }
    } else if (strcmp(change, "close") == 0) {
      echo_forget(cid); /* мёртвое окно не эхает — хвост гасим */
      if (!is_floating(con)) {
        int64_t dws = dead_master_ws(con, cid); /* 0 = не мастер */
        act.died_ws = dws;
        act.died_win = (dws > 0) ? cid : 0;
        bool fast = dws > 0 && death_fast(cmd_fd, dws, cid);
        if (!fast)
          need_retile = true;
      }
      closed = true;
    } else if (strcmp(change, "floating") == 0) {
      if (is_floating(con)) {
        need_center = true;
        need_retile = true;
        /* Уход в скретчпад: к моменту get_tree окно уже лежит в
           __i3_scratch, вне обычных столов. Адресный retile по cid
           не найдёт его ни на одном ws и молча пропустит старый
           стол: стопка растянется на весь экран, нового мастера не
           будет до следующего move. Событие не несёт id старого
           стола, поэтому обходим все столы. */
        full_retile = true;
      } else
        need_retile = true;
    } else if (strcmp(change, "move") == 0) {
      if (!is_floating(con)) {
        if (echo_consume(cid)) { /* наше эхо — без пересчёта */
          return;
        }
        need_retile = true;
        act.moved = cid;
        /* БАГ-ФИКС: если cid нёс _m/_s/_p — окно с меткой уехало
           (стрелкой на соседний выход или на другой стол). Старый стол
           остался без мастера/якоря/памяти, адресный retile по cid его
           не тронет. Полный обход обязателен. */
        if (is_marked_daemon(con))
          full_retile = true;
      }
    } else if (strcmp(change, "focus") == 0) {
      if (cid > 0)
        g_focus_win = cid;
    }
  }

  if (need_center && cid > 0) {
    char cmd[64];
    snprintf(cmd, sizeof cmd, "[con_id=%" PRId64 "] move position center", cid);
    send_cmd(cmd_fd, cmd);
  }
  if (need_retile) {
    json_object *tree = get_tree(cmd_fd);
    if (tree != nullptr) {
      /* Структурная пачка (move/swap/split/layout) могла оставить
         дерево недоделанным, даже если move-команд в ней не было
         (например, один split vertical на плоском n==2 или layout
         splith из tabbed). Контрольный круг нужен на ЛЮБУЮ структуру;
         чисто меточные/resize/focus-пачки его не требуют. */
      uint64_t struct_before = g_struct_sent;
      /* target_cid: move/floating/new-fallback — адресный;
         close — окна в дереве нет, полный; move МАСТЕРА —
         full_retile: старый стол остался без мастера */
      int64_t target_cid = 0;
      if (change != nullptr && strcmp(change, "close") != 0 && !full_retile)
        target_cid = cid;
      bool any = retile_all(cmd_fd, tree, tree, &act, target_cid);
      json_object_put(tree);
      if (any && g_struct_sent > struct_before)
        reconcile(cmd_fd);
    }
  }
  if (closed && g_focus_win == cid)
    g_focus_win = 0;
}

/* ── workspace-события: только focus — трекинг стола для слепых
   путей. reload/init/rename — тишина. ev — уже распарсенный.
   При СМЕНЕ стола g_focus_win гасим: «фокус» на окне невидимого
   стола неактуален — death_fast не должен отдавать фокус «по
   старой памяти». ── */

static void handle_ws_event(int cmd_fd, json_object *ev) {
  (void)cmd_fd;
  if (ev == nullptr)
    return;
  const char *change = jstr(ev, "change");
  if (change != nullptr && strcmp(change, "focus") == 0) {
    json_object *cur = jget(ev, "current");
    int64_t id = (cur != nullptr) ? jint(cur, "id") : 0;
    if (id > 0) {
      if (id != g_focus_ws)
        g_focus_win = 0;
      g_focus_ws = id;
    }
    if (g_debug)
      fprintf(stderr, "ws: focus=%" PRId64 "\n", g_focus_ws);
  }
}

/* ── Сфокусированный лист (тайл или плавающее) + его воркспейс;
   пустой сфокусированный стол отдаёт сам узел ws (focused:true) ── */

static json_object *find_focused(json_object *n, json_object *ws,
                                 json_object **ws_out) {
  const char *type = jstr(n, "type");
  if (type != nullptr && strcmp(type, "workspace") == 0) {
    ws = n;
    if (*ws_out == nullptr && jbool(n, "focused"))
      *ws_out = n;
  } else if (type != nullptr &&
             (strcmp(type, "con") == 0 || strcmp(type, "floating_con") == 0)) {
    json_object *nodes = jarr(n, "nodes");
    if ((nodes == nullptr || json_object_array_length(nodes) == 0) &&
        jbool(n, "focused")) {
      *ws_out = ws;
      return n;
    }
  }
  json_object *ch[2] = {jarr(n, "nodes"), jarr(n, "floating_nodes")};
  for (int k = 0; k < 2; k++) {
    if (ch[k] == nullptr)
      continue;
    size_t cnt = json_object_array_length(ch[k]);
    for (size_t i = 0; i < cnt; i++) {
      json_object *r =
          find_focused(json_object_array_get_idx(ch[k], i), ws, ws_out);
      if (r != nullptr)
        return r;
    }
  }
  return nullptr;
}
