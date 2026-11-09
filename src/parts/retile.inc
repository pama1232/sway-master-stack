/* retile.inc — Канонический ретайл и reconcile.
 *
 * Фрагмент единого translation unit: включается из main.c после sms.h.
 * Порядок включения повторяет прежний порядок объявлений в main.c.
 * retile_ws/retile_all и контрольные проходы до сходимости. */

#include "../sms.h"

/* ── Retile: канонический ребилд одного воркспейса.
 * Возвращает: отправлялись ли команды вообще (стол был болен).
 * Пачка, менявшая порядок/структуру, гасит снимок — вызывающий путь
 * обязан прогнать контрольный reconcile: он долечит и засеет снимки
 * для слепых путей. Чисто меточная пачка снимок сохраняет по t. ── */

static bool retile_ws(int fd, json_object *root, json_object *ws,
                      const Act *act) {
  int64_t wid = jint(ws, "id");
  if (wid <= 0)
    return false;
  char mk_m[24], mk_s[24], mk_p[24];
  snprintf(mk_m, sizeof mk_m, "_m%" PRId64, wid);
  snprintf(mk_s, sizeof mk_s, "_s%" PRId64, wid);
  snprintf(mk_p, sizeof mk_p, "_p%" PRId64, wid);

  Tiled t;
  memset(&t, 0, sizeof t);
  t.master_id = -1;
  t.stack_id = -1;
  t.partner_id = -1;
  json_object *wr = jget(ws, "rect");
  t.ws_w = (wr != nullptr) ? jint(wr, "width") : 0;
  collect(ws, ws, mk_m, mk_s, mk_p, &t);
  if (t.n == 0) { /* пустой стол — не трогаем */
    snap_evict(wid);
    /* мастер умер последним — фокус ОСТАЁТСЯ на этом столе:
       некоторые сборки/конфиги sway уводят его на другой выход,
       а юзер ждёт следующее окно здесь. НЕ через критерии:
       `[workspace="…"] focus` на пустом столе матчит ноль
       контейнеров — только прямая команда `workspace`. Стол
       обязан быть ещё сфокусирован (g_focus_ws): close
       асинхронен, юзер мог успеть уйти на пустой стол, а
       g_focus_win этого не видит — переход на пустой стол не
       порождает window::focus */
    if (act->died_ws == wid && g_focus_ws == wid &&
        g_focus_win == act->died_win) {
      char keep[WSNAME_MAX + 48];
      if (ws_focus_cmd(keep, sizeof keep, jstr(ws, "name"))) {
        send_cmd(fd, keep);
        return true;
      }
    }
    return false;
  }
  if (g_debug)
    fprintf(stderr,
            "retile: ws=%" PRId64 " n=%d p0ws=%d splith=%d "
            "m=%" PRId64 " mid=%" PRId64 " sid=%" PRId64 "\n",
            wid, t.n, t.parent[0] == ws, layout_is(ws, "splith"), t.id[0],
            t.master_id, t.stack_id);

  char buf[RETILE_BATCH], one[192];
  size_t off = 0;
  buf[0] = '\0';
  uint64_t struct_before = g_struct_sent;

  int pi = idx_of(&t, act->moved);
  int64_t displaced = -1; /* снятый мастер → вершина стопки */
  bool zoomed = false;    /* снятие мастера — зум (не спавн) */

  /* ═══ ЯД «КРАЙНИХ» ДВИЖЕНИЙ (move up на верхнем / move down на
     нижнем): окно вырвалось на уровень ws, а ws перевернулся в
     splitv. Лечение — реструктуризация «[окно] move left»: окно
     встаёт ПЕРВЫМ ребёнком, ws=splith, лишние обёртки sway
     растворяет сам (замерено). НИЖНЕЕ (последний тайл): старый
     мастер — на ДНО стопки (точный обмен) — вгоняем его туда ДО
     реструктуризации, переметив _s на текущий низ. Снимок после
     реструктуризации недостоверен — остаток доделает reconcile.
     n>1 — ОБЯЗАТЕЛЬНО: синглтон (ручной layout + потерянная _m)
     ядро не лечит — move left на нём = ВАРП НА СОСЕДНИЙ ВЫХОД,
     метку мастеру достроит общий путь ниже. ═══ */
  if (pi >= 0 && act->moved > 0 && act->moved != t.master_id && t.n > 1 &&
      t.parent[pi] == ws && !layout_is(ws, "splith")) {
    if (g_debug)
      fprintf(stderr, "яд: pi=%d n=%d mid=%" PRId64 "\n", pi, t.n, t.master_id);
    if (pi == t.n - 1 && t.master_id > 0) {
      json_object *sc = nullptr; /* контейнер стопки */
      for (int i = t.n - 1; i >= 1; i--)
        if (i != pi && t.parent[i] != ws) {
          sc = t.parent[i];
          break;
        }
      int sn = -1; /* низ стопки */
      if (sc != nullptr)
        for (int i = t.n - 1; i >= 1; i--)
          if (i != pi && t.parent[i] == sc) {
            sn = i;
            break;
          }
      int mi = idx_of(&t, t.master_id);
      if (sn > 0 && mi >= 0 && t.parent[mi] != sc) {
        if (g_debug)
          fprintf(stderr, "яд-T2: сн=%" PRId64 " мастер=%" PRId64 " на дно\n",
                  t.id[sn], t.id[mi]);
        snprintf(one, sizeof one, "[con_id=%" PRId64 "] mark --add %s",
                 t.id[sn], mk_s);
        cmd_append(buf, sizeof buf, &off, one);
        snprintf(one, sizeof one,
                 "[con_id=%" PRId64 "] move container to mark %s", t.id[mi],
                 mk_s);
        cmd_move(buf, sizeof buf, &off, one, t.id[mi]);
      }
    }
    snprintf(one, sizeof one, "[con_id=%" PRId64 "] move left", t.id[pi]);
    cmd_move(buf, sizeof buf, &off, one, t.id[pi]);
    goto finish;
  }

  /* ═══ move right ИЗ СТОПКИ НА СОСЕДНИЙ ВЫХОД: Sway выносит окно
     из splitv-стопки на уровень ws ПОСЛЕДНИМ (справа от контейнера
     стопки) и на соседний выход сам не идёт — окно ещё не было
     ребёнком ws. Это не зум: если справа есть выход, досылаем ещё
     один move right — теперь окно крайнее справа на уровне ws, и
     Sway уносит его туда. Без выхода справа — прежний путь (зум),
     поведение одиночного монитора не меняется.

     Снимок подтверждает, что окно пришло ИЗ СТОПКИ этого стола, а
     не только что приехало с другого выхода: иначе reconcile после
     переброса мог бы гнать окно дальше через все мониторы. ── */
  if (pi > 0 && act->moved > 0 && act->moved != t.master_id && t.n > 1 &&
      t.master_id == t.id[0] && t.parent[pi] == ws && pi == t.n - 1 &&
      layout_is(ws, "splith")) {
    /* n==2 (стопка из одного окна) сюда не попадает: у неё нет
       контейнера-стопки, и move left/right могут дать одинаковое
       дерево — направление по нему не восстановить. Ветка включается
       только когда ДРУГИЕ окна стопки реально сидят в контейнере. */
    bool stack_ctx = false;
    for (int i = 1; i < t.n; i++)
      if (i != pi && t.parent[i] != ws) {
        stack_ctx = true;
        break;
      }
    Snap *sn = snap_get(wid);
    bool from_stack = false;
    if (stack_ctx && sn != nullptr && sn->master == t.id[0] && sn->n == t.n - 1)
      for (int i = 0; i < sn->n; i++)
        if (sn->stack[i] == act->moved) {
          from_stack = true;
          break;
        }
    if (from_stack && output_right_of(root, ws)) {
      if (g_debug)
        fprintf(stderr, "выход вправо: %" PRId64 " → соседний выход\n",
                act->moved);
      snprintf(one, sizeof one, "[con_id=%" PRId64 "] move right", act->moved);
      cmd_move_short(buf, sizeof buf, &off, one, act->moved);
      goto finish;
    }
  }

  /* ═══ МАСТЕР УШЁЛ В СТОПКУ (сырой move right на мастере): Sway
     вбрасывает его ВНУТРЬ стопки — по геометрии, серединой, а не
     вершиной (замерено: 3-окна → слот 1). Есть память _p — ТОЧНЫЙ
     ВОЗВРАТ: свап с партнёром обменивает их слоты (партнёр сидит
     ровно на прежнем слоте мастера!), затем партнёр выходит в
     мастер — снятый остаётся на СВОЁМ месте, с геометрией слота.
     Памяти нет — РОТАЦИЯ: сосед уходит в мастер (при pi>0 — свап
     с вершиной + её побег; при pi==0 — побег соседа снизу), снятый
     остаётся вершиной стопки (якорь _s). Ротация ставит память —
     повторный move right возвращает всё. Побег сбрасывает ширину —
     resize в пачке. ═══ */
  if (pi >= 0 && act->moved > 0 && act->moved == t.master_id && t.n >= 2 &&
      t.parent[pi] != ws) {
    int ki = idx_of(&t, t.partner_id);
    if (ki < 0 || ki == pi || t.partner_id == t.master_id ||
        t.parent[ki] != t.parent[pi])
      ki = -1;             /* памяти нет/чужая — ротация */
    int64_t who = 0;       /* кто выходит в мастер */
    bool rot = false;      /* ротация (не возврат) */
    bool top_mark = false; /* снятому мастеру — якорь _s */
    if (ki >= 0) {
      who = t.partner_id; /* точный возврат */
      /* партнёр был якорем (вершиной) до вставки мастера? */
      top_mark = (pi > 0) ? (ki == 0) : (ki == 1);
    } else {
      rot = true;
      if (pi == 0) {
        /* снятый мастер — уже первый в стопке; после побега
           соседа он ОСТАЁТСЯ вершиной: якорь _s на него */
        top_mark = true;
        if (t.parent[1] == t.parent[0])
          who = t.id[1]; /* окно под мастером (бывшая вершина) */
      } else {
        if (t.parent[0] == t.parent[pi])
          who = t.id[0]; /* вершина */
        top_mark = true; /* снятый встанет вершиной */
      }
    }
    if (who > 0) {
      if (g_debug)
        fprintf(stderr, "демо/возврат: pi=%d ki=%d who=%" PRId64 "%s\n", pi, ki,
                who, rot ? " ротация" : " возврат");
      if (rot && pi > 0) {
        /* свап с вершиной: снятый мастер — вершиной (место И
           геометрия) ещё ДО побега вершины */
        snprintf(one, sizeof one,
                 "[con_id=%" PRId64 "] swap container with con_id %" PRId64,
                 t.id[pi], who);
        cmd_struct(buf, sizeof buf, &off, one);
      } else if (!rot) {
        /* свап с партнёром: снятый мастер — на его слот */
        snprintf(one, sizeof one,
                 "[con_id=%" PRId64 "] swap container with con_id %" PRId64,
                 t.id[pi], who);
        cmd_struct(buf, sizeof buf, &off, one);
      }
      snprintf(one, sizeof one, "[con_id=%" PRId64 "] move left", who);
      cmd_move(buf, sizeof buf, &off, one, who);
      snprintf(one, sizeof one, "[con_id=%" PRId64 "] mark --add %s", who,
               mk_m); /* крадёт _m у снятого мастера */
      cmd_append(buf, sizeof buf, &off, one);
      if (t.partner_id == who) { /* _p был на нём — гасим явно */
        snprintf(one, sizeof one, "[con_id=%" PRId64 "] unmark %s", who, mk_p);
        cmd_append(buf, sizeof buf, &off, one);
      }
      if (rot) { /* ротация ставит память */
        snprintf(one, sizeof one, "[con_id=%" PRId64 "] mark --add %s",
                 t.id[pi], mk_p);
        cmd_append(buf, sizeof buf, &off, one);
      }
      if (top_mark) { /* снятый мастер — вершина: якорь */
        snprintf(one, sizeof one, "[con_id=%" PRId64 "] mark --add %s",
                 t.id[pi], mk_s);
        cmd_append(buf, sizeof buf, &off, one);
      }
      snprintf(one, sizeof one, "[con_id=%" PRId64 "] resize set width %d ppt",
               who, MASTER_PCT);
      cmd_append(buf, sizeof buf, &off, one);
      goto finish;
    }
    /* партнёр и сосед — чужие контейнеры: общий путь ниже (ПОБЕГ) */
  }

  /* ═══ ТОЧНЫЙ ЗУМ (сырой move left из ГЛУБИНЫ стопки): окно
     вырвалось на уровень ws, стол до этого был здоров (есть снимок).
     ОДИН свап с мастером — место И геометрия; бывший мастер — на
     ТОЧНЫЙ слот зумированного: метка _a на окно НАД слотом,
     «move container to mark» вставляет прямо за ним. Зумировали
     вершину (или снимка нет) — общий путь ниже, вершина и так
     точна. Память _p — на снятом мастере: возврат move right
     будет точным. ═══ */
  if (pi > 0 && act->moved > 0 && act->moved != t.master_id &&
      t.parent[pi] == ws && t.master_id > 0 && t.master_id == t.id[0]) {
    Snap *sn = snap_get(wid);
    int j = -1;
    if (sn != nullptr)
      for (int i = 0; i < sn->n; i++)
        if (sn->stack[i] == act->moved) {
          j = i;
          break;
        }
    if (j >= 1) {
      int wi = idx_of(&t, sn->stack[j - 1]); /* окно над слотом */
      if (wi > 0 && t.parent[wi] != ws) {
        /* Якорь _s уже стоит на вершине стопки. Если зумировали
           ВЕРШИНУ (j==1) и якорь в текущем дереве — на том же окне,
           old master можно вставить прямо за _s: минус две команды
           (mark _a + unmark _a) на самый частый зум. Иначе — временная
           метка _a, как раньше. */
        bool use_s = (j == 1 && t.stack_id == sn->stack[0]);
        char mk_a[24];
        if (!use_s)
          snprintf(mk_a, sizeof mk_a, "_a%" PRId64, wid);
        if (g_debug)
          fprintf(stderr, "точный зум: j=%d над=%" PRId64 "%s\n", j,
                  sn->stack[j - 1], use_s ? " (якорь _s)" : " (_a)");
        snprintf(one, sizeof one,
                 "[con_id=%" PRId64 "] swap container with mark %s", t.id[pi],
                 mk_m);
        cmd_struct(buf, sizeof buf, &off, one);
        snprintf(one, sizeof one, "[con_id=%" PRId64 "] mark --add %s",
                 t.id[pi], mk_m);
        cmd_append(buf, sizeof buf, &off, one);
        if (t.partner_id == t.id[pi]) { /* _p был на зумированном */
          snprintf(one, sizeof one, "[con_id=%" PRId64 "] unmark %s", t.id[pi],
                   mk_p);
          cmd_append(buf, sizeof buf, &off, one);
        }
        snprintf(one, sizeof one, "[con_id=%" PRId64 "] mark --add %s",
                 t.master_id, mk_p); /* память возврата */
        cmd_append(buf, sizeof buf, &off, one);
        if (!use_s) {
          snprintf(one, sizeof one, "[con_id=%" PRId64 "] mark --add %s",
                   sn->stack[j - 1], mk_a);
          cmd_append(buf, sizeof buf, &off, one);
        }
        snprintf(one, sizeof one,
                 "[con_id=%" PRId64 "] move container to mark %s", t.master_id,
                 use_s ? mk_s : mk_a);
        cmd_move(buf, sizeof buf, &off, one, t.master_id);
        if (!use_s) {
          snprintf(one, sizeof one, "unmark %s", mk_a);
          cmd_append(buf, sizeof buf, &off, one);
        }
        goto finish;
      }
    }
  }

  /* ═══ КАНДИДАТ: зум (вырвался на уровень ws) или новое окно.
     Живой держатель _m — ОДИН свап: обмен местами и геометрией,
     бывший мастер встаёт ТОЧНО на место кандидата. ═══ */
  int cand = -1;
  if (pi > 0 && act->moved != t.master_id && t.parent[pi] == ws)
    cand = pi; /* зум: move left вырвал на уровень ws */
  if (cand < 0 && act->promote > 0) {
    int ni = idx_of(&t, act->promote);
    if (ni > 0)
      cand = ni; /* новое окно — мастер (dwm attach) */
  }
  if (cand > 0 && t.master_id > 0) {
    displaced = t.id[0];   /* прежний мастер уходит под свап */
    zoomed = (cand == pi); /* снятие зумом, не спавном */
    snprintf(one, sizeof one,
             "[con_id=%" PRId64 "] swap container with mark %s", t.id[cand],
             mk_m);
    cmd_struct(buf, sizeof buf, &off, one);
    int64_t tid = t.id[cand];
    t.id[cand] = t.id[0];
    t.id[0] = tid;
    /* parent[] не трогаем: слоты позиционны, свап меняет окна,
       но не родителей слотов (обмен был бы багом: порождал ложный
       «побег» и висячее эхо от no-op move) */
  }

  /* ═══ ПОБЕГ ИЗ ПЛЕНА: мастер не на уровне ws — «move left».
     Встаёт ПЕРВЫМ ребёнком ws, ws=splith, контейнер стопки жив.

     ТРИ гварда на вход:
     (1) t.n > 1 — иначе move left на синглтоне = ВАРП НА СОСЕДНИЙ
         ВЫХОД (i3-семантика «singleton children as if at workspace
         level»).
     (2) sib >= 2 — сколько тайлов в обёртке мастера (t.parent[0]).
         Если 1 — обёртка-singleton: move left M трактуется sway
         как move left обёртки; если обёртка при этом первый ребёнок
         ws — ВАРП. t.n > 1 сам по себе НЕ гарантирует соседа: при
         ws=[wrapper[M], N] t.n=2, но wrapper содержит только M.
         Требуем реального соседа в обёртке: move left остаётся
         внутри обёртки (промоут), не выходит на уровень ws.
         Fallback — общий путь ниже: визуально канон сохранится
         (M на весь свой слот), варпа не будет.
     (3) tabbed/stacked-тюрьма — move left там переключение ТАБОВ
         (no-op без эха!) — сначала превращаем её в splitv (layout
         по листу целит в родителя), потом побег.

     parent[0] = ws обновляем вручную: общий путь ниже рассчитан
     на «мастер уже на уровне ws», ему это нужно знать. ═══ */
  bool escaped = false;
  int sib = 0;
  if (t.parent[0] != ws)
    for (int i = 0; i < t.n; i++)
      if (t.parent[i] == t.parent[0])
        sib++;
  if (t.parent[0] != ws && t.n > 1 && sib >= 2) {
    const char *pl = jstr(t.parent[0], "layout");
    if (pl != nullptr &&
        (strcmp(pl, "tabbed") == 0 || strcmp(pl, "stacked") == 0)) {
      snprintf(one, sizeof one, "[con_id=%" PRId64 "] layout splitv", t.id[0]);
      cmd_struct(buf, sizeof buf, &off, one);
    }
    snprintf(one, sizeof one, "[con_id=%" PRId64 "] move left", t.id[0]);
    cmd_move(buf, sizeof buf, &off, one, t.id[0]);
    t.parent[0] = ws;
    escaped = true;
  } else if (t.parent[0] == ws && !layout_is(ws, "splith") && t.n > 1) {
    /* ═══ ФЛИП МАСТЕРА (сырой move up/down на мастере): sway
       переворачивает ws в splitv и рассыпает стол в плоскость,
       ПЕРЕВЕРНУВ стопку (замерено: [M,A,B,C] → вверх:
       [M,C,B,A], вниз: [C,B,A,M]). Жест юзера — СВАП с краем
       стопки: вверх — с вершиной, вниз — с дном (точный
       обмен); порядок остальных — как ДО жеста: свежий снимок
       (если он ровно про этот стол), иначе реверс флипа.
       Побег в мастер сбрасывает ширину — resize в пачке;
       контейнер стопки и якорь достроит контрольный reconcile.
       Память _p — на снятом мастере: move right на новом
       вернёт всё (инволюция). ═══ */
    if (act->moved > 0 && act->moved == t.master_id && t.n >= 2) {
      int mi = idx_of(&t, act->moved);
      bool fup = (mi == 0);       /* мастер сверху — «вверх» */
      bool fdn = (mi == t.n - 1); /* мастер снизу — «вниз» */
      if (fup || fdn) {
        int64_t pre[TILED_MAX]; /* стопка ДО жеста: [0] — верх */
        int pn = 0;
        Snap *sn = snap_get(wid);
        bool sok =
            sn != nullptr && sn->n == t.n - 1 && sn->master == act->moved;
        if (sok)
          for (int i = 0; i < sn->n; i++)
            if (idx_of(&t, sn->stack[i]) < 0) {
              sok = false;
              break;
            }
        if (sok) {
          for (int i = 0; i < sn->n; i++)
            pre[pn++] = sn->stack[i];
        } else {
          /* флипнутые дети без мастера — в порядке флипа;
             разворот возвращает ДО-жестовый порядок */
          int64_t flip[TILED_MAX];
          int fn = 0;
          for (int i = 0; i < t.n; i++)
            if (t.id[i] != act->moved)
              flip[fn++] = t.id[i];
          for (int i = fn - 1; i >= 0; i--)
            pre[pn++] = flip[i];
        }
        int64_t who = fup ? pre[0] : pre[pn - 1];
        if (idx_of(&t, who) >= 0) {
          /* якорь-вершина новой стопки: вверх — снятый
             мастер, вниз — бывшая вершина (снятый встаёт
             дном: вставляется за якорем первым) */
          int64_t anch = fup ? act->moved : (pn >= 2 ? pre[0] : act->moved);
          if (g_debug)
            fprintf(stderr, "флип мастера: %s, в мастер %" PRId64 "\n",
                    fup ? "вверх" : "вниз", who);
          snprintf(one, sizeof one, "[con_id=%" PRId64 "] move left", who);
          cmd_move(buf, sizeof buf, &off, one, who);
          snprintf(one, sizeof one, "[con_id=%" PRId64 "] mark --add %s", who,
                   mk_m); /* крадёт _m у снятого */
          cmd_append(buf, sizeof buf, &off, one);
          if (t.partner_id == who) { /* _p был на новом мастере */
            snprintf(one, sizeof one, "[con_id=%" PRId64 "] unmark %s", who,
                     mk_p);
            cmd_append(buf, sizeof buf, &off, one);
          }
          if (t.partner_id > 0 && t.partner_id != act->moved &&
              t.partner_id != who) {
            snprintf(one, sizeof one, "[con_id=%" PRId64 "] unmark %s",
                     t.partner_id, mk_p);
            cmd_append(buf, sizeof buf, &off, one);
          }
          snprintf(one, sizeof one, "[con_id=%" PRId64 "] mark --add %s",
                   act->moved, mk_p); /* память возврата */
          cmd_append(buf, sizeof buf, &off, one);
          snprintf(one, sizeof one, "[con_id=%" PRId64 "] mark --add %s", anch,
                   mk_s);
          cmd_append(buf, sizeof buf, &off, one);
          if (fup) { /* снятый — вершина: [M, S1..Sk] */
            for (int i = pn - 1; i >= 1; i--) {
              snprintf(one, sizeof one,
                       "[con_id=%" PRId64 "] move container to mark %s", pre[i],
                       mk_s);
              cmd_move(buf, sizeof buf, &off, one, pre[i]);
            }
          } else { /* снятый — дно: [S0..Sk-1, M] */
            if (anch != act->moved) {
              snprintf(one, sizeof one,
                       "[con_id=%" PRId64 "] move container to mark %s",
                       act->moved, mk_s);
              cmd_move(buf, sizeof buf, &off, one, act->moved);
            }
            for (int i = pn - 2; i >= 1; i--) {
              snprintf(one, sizeof one,
                       "[con_id=%" PRId64 "] move container to mark %s", pre[i],
                       mk_s);
              cmd_move(buf, sizeof buf, &off, one, pre[i]);
            }
          }
          snprintf(one, sizeof one,
                   "[con_id=%" PRId64 "] resize set width %d ppt", who,
                   MASTER_PCT);
          cmd_append(buf, sizeof buf, &off, one);
          goto finish;
        }
      }
    }
    /* ═══ РЕСТРУКТУРИЗАЦИЯ: мастер УЖЕ на уровне ws, но ws не
       splith (пережиток чужого layout/яда без movers).

       Два подслучая:

       (а) ws САМ в tabbed/stacked — move left на прямом ребёнке
           ws = switch tab (no-op), layout не лечится. Сначала
           layout splith на мастере: layout по листу целит в
           родителя (ws), ws → splith; мастер уже первый на
           уровне ws. Общий reconcile потом обернёт стопку.

       (б) ws не splith по другой причине — move left M: мастер
           встаёт первым ребёнком, ws=splith, лишние обёртки
           sway растворяет сам.

       n==1 сюда НЕ ПОПАДАЕТ (гвард в if выше): ручной
       tabbed/splitv на столе с одним окном — окно и так на
       весь стол, а move left на синглтоне = ВАРП НА СОСЕДНИЙ
       ВЫХОД. ═══ */
    if (layout_is(ws, "tabbed") || layout_is(ws, "stacked")) {
      snprintf(one, sizeof one, "[con_id=%" PRId64 "] layout splith", t.id[0]);
      cmd_struct(buf, sizeof buf, &off, one);
      goto finish;
    }
    snprintf(one, sizeof one, "[con_id=%" PRId64 "] move left", t.id[0]);
    cmd_move(buf, sizeof buf, &off, one, t.id[0]);
    goto finish;
  }

  int64_t m = t.id[0];
  if (t.master_id != m) {
    snprintf(one, sizeof one, "[con_id=%" PRId64 "] mark --add %s", m, mk_m);
    cmd_append(buf, sizeof buf, &off, one);
    /* сменился мастер (НЕ нашим зумом/демо) — память возврата _p
       недействительна: место держит окно, занявшее место ДРУГОГО */
    if (t.partner_id > 0) {
      snprintf(one, sizeof one, "[con_id=%" PRId64 "] unmark %s", t.partner_id,
               mk_p);
      cmd_append(buf, sizeof buf, &off, one);
    }
  } else if (act->moved > 0 && act->moved == t.partner_id &&
             act->moved != t.master_id) {
    /* партнёр памяти уехал своими биндами — точный возврат
       стал бы ложным, память гасим (документированная семантика).
       Side-effect от наших move-команд гасится кэшем эха; событие
       без записи в кэше считаем ходом юзера. */
    snprintf(one, sizeof one, "unmark %s", mk_p);
    cmd_append(buf, sizeof buf, &off, one);
  }

  /* ═══ СМЕРТЬ МАСТЕРА ПРИ n=1 (честный путь без снимка): Sway
     оставляет singleton-обёртку (`splitv`/`tabbed`) вокруг выжившего
     (upstream #6116). Без неё следующий спавн ляжет внутрь и получит
     вертикальный «мастер». split none распускает ровно такого
     родителя и НЕ варпает окно на соседний выход. Гвард died_ws:
     одиночное окно, посаженное юзером в tabbed, не трогаем. ═══ */
  if (t.n == 1 && act->died_ws == wid && act->died_win > 0 &&
      t.parent[0] != ws) {
    snprintf(one, sizeof one, "[con_id=%" PRId64 "] split none", t.id[0]);
    cmd_struct(buf, sizeof buf, &off, one);
  }

  /* одно окно: структуру не трогаем; следующее событие достроит */
  if (t.n == 1)
    goto finish;

  /* ═══ СТОПКА: якорь — первый не-мастер, УЖЕ живущий в контейнере;
     если все плоско (рождение/частокол) — tiled[1] ═══ */
  int ai = 1;
  for (int i = 1; i < t.n; i++)
    if (t.parent[i] != ws) {
      ai = i;
      break;
    }
  int64_t a = t.id[ai];
  json_object *ac = t.parent[ai];
  bool flat = (ac == ws);

  /* ═══ СНЯТЫЙ МАСТЕР (контрольный раунд): держатель _m — не мастер
     и ВНЕ контейнера стопки → он бывший мастер → вершина (dwm).
     Внутри стопки (точный обмен от «низа-вниз») — не трогаем. ═══ */
  if (displaced < 0 && t.master_id > 0 && t.master_id != m) {
    int mi = idx_of(&t, t.master_id);
    if (mi > 0 && t.parent[mi] != ac)
      displaced = t.master_id;
  }

  if (t.stack_id != a) {
    /* --add: крадёт _s у старого держателя, не срезая _p/юзерские
       метки якоря (срез --replace губил память сразу после зума) */
    snprintf(one, sizeof one, "[con_id=%" PRId64 "] mark --add %s", a, mk_s);
    cmd_append(buf, sizeof buf, &off, one);
  }
  if (flat) {
    /* обернуть якорь в свежий splitv — контейнер стопки */
    snprintf(one, sizeof one, "[con_id=%" PRId64 "] split vertical", a);
    cmd_struct(buf, sizeof buf, &off, one);
  } else if (!layout_is(ac, "splitv")) {
    /* лист ИЗНУТРИ стопки → цель = контейнер стопки */
    snprintf(one, sizeof one, "[con_id=%" PRId64 "] layout splitv", a);
    cmd_struct(buf, sizeof buf, &off, one);
  }

  /* сбор: все не-мастера вне контейнера якоря едут к метке _s —
     вставка следующей за якорем = верх стопки. Обратный порядок
     сохраняет визуальный порядок стека. */
  for (int i = t.n - 1; i >= 1; i--) {
    if (i == ai)
      continue;
    if (t.id[i] == displaced)
      continue;
    if (!flat && t.parent[i] == ac)
      continue;
    snprintf(one, sizeof one, "[con_id=%" PRId64 "] move container to mark %s",
             t.id[i], mk_s);
    cmd_move(buf, sizeof buf, &off, one, t.id[i]);
  }

  /* ═══ СНЯТЫЙ МАСТЕР — ВЕРШИНА СТОПКИ (dwm: старый head → clients[1]).
     Вне стопки — сначала ввезти к якорю, затем один свап с якорем.
     Уже внутри (глубокий спавн/зум) — свап сразу. Зум (не спавн)
     ставит память _p — возврат move right будет точным. ═══ */
  if (displaced > 0 && displaced != a) {
    int di = idx_of(&t, displaced);
    if (di > 0) {
      if (t.parent[di] == ws) {
        snprintf(one, sizeof one,
                 "[con_id=%" PRId64 "] move container to mark %s", displaced,
                 mk_s);
        cmd_move(buf, sizeof buf, &off, one, displaced);
      }
      snprintf(one, sizeof one,
               "[con_id=%" PRId64 "] swap container with mark %s", displaced,
               mk_s);
      cmd_struct(buf, sizeof buf, &off, one);
      /* снятый теперь вершина = якорь: переметить _s сразу,
         чтобы контрольный раунд не трогал метки вовсе (и не
         рисковал свежепоставленную _p зума) */
      snprintf(one, sizeof one, "[con_id=%" PRId64 "] mark --add %s", displaced,
               mk_s);
      cmd_append(buf, sizeof buf, &off, one);
      if (zoomed) {
        snprintf(one, sizeof one, "[con_id=%" PRId64 "] mark --add %s",
                 displaced, mk_p);
        cmd_append(buf, sizeof buf, &off, one);
      }
    }
  }

  /* пропорция: свап несёт геометрию с собой, побег — сбрасывает
     в 50/50; в обоих случаях слот 0 должен стать M% */
  int64_t zone_w = 0; /* ширина контейнера стопки */
  if (!flat) {
    json_object *zr = jget(ac, "rect");
    if (zr != nullptr)
      zone_w = jint(zr, "width");
  }
  if (escaped || !proportions_ok(&t, zone_w)) {
    snprintf(one, sizeof one, "[con_id=%" PRId64 "] resize set width %d ppt", m,
             MASTER_PCT);
    cmd_append(buf, sizeof buf, &off, one);
  }

finish:
  /* ═══ СМЕРТЬ МАСТЕРА: фокус — новому мастеру, и ТОЛЬКО когда
     умерший мастер его держал (g_focus_win из window::focus; иначе
     крадём фокус у юзера, сидящего на окне стопки) И юзер ещё на
     этом столе (g_focus_ws): close асинхронен, юзер мог успеть
     уйти, а focus на окне невидимого стола переключает стол.
     Sway сам фокусирует ДНО стопки (замерено) — подсветка не
     попадает на мастера. ═══ */
  if (act->died_ws == wid && act->died_win > 0 && g_focus_ws == wid &&
      g_focus_win == act->died_win) {
    snprintf(one, sizeof one, "[con_id=%" PRId64 "] focus", t.id[0]);
    cmd_append(buf, sizeof buf, &off, one);
  }
  if (off > 0) {
    send_cmd(fd, buf); /* здоровый стол — off == 0 */
    /* Снимок гасит только пачка, менявшая порядок/иерархию
       (move/swap/split/layout). Чисто меточная пачка (unmark _p
       после нативного move партнёра, mark _s), resize или focus
       порядок не трогает — снимок по t остаётся верным и нужен
       следующему точному зуму. */
    if (g_struct_sent > struct_before)
      snap_evict(wid);
    else
      snap_store_tiles(wid, ws, &t);
  } else {
    /* 0 команд = дерево совпало с каноном: снимок верен. */
    snap_store_tiles(wid, ws, &t);
  }
  return off > 0;
}

/* есть ли в поддереве n узел с id == cid (включая сам n, nodes и
   floating_nodes): DFS по дереву, дешевле collect (только jint-сравнения,
   без marks/rect/layout). Нужен адресному retile: пропускаем чужие ws. */
[[nodiscard]] static bool ws_has_cid(json_object *n, int64_t cid) {
  if (cid <= 0)
    return false;
  if (jint(n, "id") == cid)
    return true;
  json_object *nodes = jarr(n, "nodes");
  if (nodes != nullptr) {
    size_t cnt = json_object_array_length(nodes);
    for (size_t i = 0; i < cnt; i++)
      if (ws_has_cid(json_object_array_get_idx(nodes, i), cid))
        return true;
  }
  json_object *fl = jarr(n, "floating_nodes");
  if (fl != nullptr) {
    size_t cnt = json_object_array_length(fl);
    for (size_t i = 0; i < cnt; i++)
      if (ws_has_cid(json_object_array_get_idx(fl, i), cid))
        return true;
  }
  return false;
}

/* retile_all: target_cid > 0 — ретилить ТОЛЬКО тот ws, в котором есть
   это окно (floating/new-fallback знают cid, и других столов они
   не касаются); target_cid == 0 — все столы (контрольный круг, старт,
   close — окна в дереве уже нет, а также move МАСТЕРА — он мог уехать
   на соседний выход, и СТАРЫЙ стол остался без мастера: адресный
   retile по cid ретилит только НОВЫЙ стол, старый не тронется —
   стопка растянется, пока следующее событие не дотянется. Полный
   обход обязателен: вершина старой стопки станет мастером). */
static bool retile_all(int fd, json_object *root, json_object *n,
                       const Act *act, int64_t target_cid) {
  bool any = false;
  const char *type = jstr(n, "type");
  if (type != nullptr && strcmp(type, "workspace") == 0) {
    if (target_cid > 0 && !ws_has_cid(n, target_cid))
      return false;                     /* чужой стол — мимо */
    return retile_ws(fd, root, n, act); /* воркспейсы не вложены */
  }
  json_object *nodes = jarr(n, "nodes");
  if (nodes == nullptr)
    return false;
  size_t cnt = json_object_array_length(nodes);
  for (size_t i = 0; i < cnt; i++)
    any |= retile_all(fd, root, json_object_array_get_idx(nodes, i), act,
                      target_cid);
  return any;
}

/* ── Контрольный reconcile: после любой пачки перечитать истинное
   состояние и доделать/проверить (максимум 2 раунда). Слепые
   перехваты передают сюда свой act — промоушен/смерть доводятся
   до конца, даже если пачка промолчала (чужой стол).
   target_cid > 0 — только стол с этим окном (адресный reconcile после
   спавна: гвард пачки физически адресован одному столу, чужие столы
   вылечит их собственное событие). 0 — все столы (контрольный круг). ── */

static void reconcile_act(int fd, const Act *act, int64_t target_cid) {
  for (int i = 0; i < 2; i++) {
    json_object *tree = get_tree(fd);
    if (tree == nullptr)
      return;
    bool any = retile_all(fd, tree, tree, act, target_cid);
    json_object_put(tree);
    if (g_debug)
      fprintf(stderr, "reconcile: раунд %d, команды=%d\n", i, any ? 1 : 0);
    if (!any)
      break; /* чистый круг — снимки свежие, хватит */
  }
}

static void reconcile(int fd) { reconcile_act(fd, &ACT_NONE, 0); }

/* найти узел workspace по id; воркспейсы не вложены — нашли или нет */
[[nodiscard]] static json_object *find_ws_by_id(json_object *n, int64_t wid) {
  const char *type = jstr(n, "type");
  if (type != nullptr && strcmp(type, "workspace") == 0)
    return (jint(n, "id") == wid) ? n : nullptr;
  json_object *nodes = jarr(n, "nodes");
  if (nodes == nullptr)
    return nullptr;
  size_t cnt = json_object_array_length(nodes);
  for (size_t i = 0; i < cnt; i++) {
    json_object *r = find_ws_by_id(json_object_array_get_idx(nodes, i), wid);
    if (r != nullptr)
      return r;
  }
  return nullptr;
}

/* reconcile ОДНОГО стола по wsid: те же ≤2 раунда, но collect —
   только по целевому ws. Смерть мастера по гварду физически не
   трогает чужие столы (если те больны — их вылечит своё событие). ── */
static void reconcile_ws(int fd, int64_t wsid) {
  if (wsid <= 0)
    return;
  for (int i = 0; i < 2; i++) {
    json_object *tree = get_tree(fd);
    if (tree == nullptr)
      return;
    json_object *ws = find_ws_by_id(tree, wsid);
    bool any = (ws != nullptr) && retile_ws(fd, tree, ws, &ACT_NONE);
    json_object_put(tree);
    if (!any)
      break;
  }
}
