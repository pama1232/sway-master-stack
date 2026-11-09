/* snapshot.inc — Пропорции, действие раунда и снимки столов.
 *
 * Фрагмент единого translation unit: включается из main.c после sms.h.
 * Порядок включения повторяет прежний порядок объявлений в main.c.
 * Act, LRU-снимки здорового порядка, мёртвый мастер, соседний выход. */

#include "../sms.h"

/* ── Пропорции: мастер ≈ MASTER_PCT% зоны тайлинга? База — сумма
   ширин зон (мастер + контейнер стопки), а НЕ rect воркспейса:
   rect включает gaps, а «resize set … ppt» считается от контентной
   области — на конфигах с gaps rect-база расходилась с ppt и гоняла
   resize на каждом ретайле (снимки тогда не хранятся вовсе). ── */

[[nodiscard]] static bool proportions_ok(const Tiled *t, int64_t zone_w) {
  if (t->n < 2 || t->rw[0] <= 0)
    return true;
  int64_t base = (zone_w > 0) ? t->rw[0] + zone_w : t->ws_w;
  if (base <= 0)
    return true;
  int pct = (int)((t->rw[0] * 100) / base);
  return pct >= MASTER_PCT - PCT_TOLERANCE && pct <= MASTER_PCT + PCT_TOLERANCE;
}

/* ── Действие раунда (из события) ── */

typedef struct {
  int64_t promote;  /* новое окно → мастер; 0 = нет */
  int64_t moved;    /* окно от window::move; 0 = нет */
  int64_t died_ws;  /* ws, где умер держатель _m; 0 = нет */
  int64_t died_win; /* умерший держатель _m (для фокуса); 0 = нет */
} Act;

static const Act ACT_NONE = {
    .promote = 0, .moved = 0, .died_ws = 0, .died_win = 0};

/* ── СНИМОК СТОЛА: последний ЗДОРОВЫЙ порядок (0 команд = снимок
   верен; порядок/структуру меняет только move/swap/split/layout —
   такая пачка гасит, чисто меточная сохраняет).
   Читают: слепой спавн (мастер/стопка), слепая смерть (верхушка),
   точный зум (порядок стопки). Это ускоритель, не истина: нет
   снимка — честный путь с get_tree. ── */

#define WS_SNAP 16
typedef struct {
  int64_t ws;               /* id воркспейса; 0 = слот свободен */
  char name[WSNAME_MAX];    /* имя ws для гварда workspace="…" */
  int64_t master;           /* мастер (для слепого спавна) */
  int64_t partner;          /* держатель _p<ws>, -1 = памяти нет */
  int n;                    /* окон в стопке */
  int64_t stack[TILED_MAX]; /* порядок стопки сверху вниз */
  uint64_t tick;            /* LRU по ЧТЕНИЯМ */
} Snap;

static Snap g_snap[WS_SNAP];
static uint64_t g_snap_tick = 1;

static Snap *snap_get(int64_t ws) {
  if (ws <= 0)
    return nullptr;
  for (int i = 0; i < WS_SNAP; i++)
    if (g_snap[i].ws == ws) {
      g_snap[i].tick = ++g_snap_tick;
      return &g_snap[i];
    }
  return nullptr;
}

static void snap_evict(int64_t ws) {
  if (ws <= 0)
    return;
  for (int i = 0; i < WS_SNAP; i++)
    if (g_snap[i].ws == ws) {
      g_snap[i].ws = 0;
      return;
    }
}

/* name_len передаётся явно: длина уже известна (json_object_get_string_len)
   или из strlen в вызывающем — не делаем strlen второй раз */
static void snap_store(int64_t ws, const char *name, size_t name_len,
                       int64_t master, const int64_t *stack, int n) {
  if (ws <= 0 || n < 0 || n > TILED_MAX)
    return;
  Snap *s = nullptr;
  for (int i = 0; i < WS_SNAP; i++)
    if (g_snap[i].ws == ws) {
      s = &g_snap[i];
      break;
    }
  if (s == nullptr) {
    for (int i = 0; i < WS_SNAP; i++) {
      if (g_snap[i].ws == 0) {
        s = &g_snap[i];
        break;
      }
      if (s == nullptr || g_snap[i].tick < s->tick)
        s = &g_snap[i];
    }
    s->tick = ++g_snap_tick;
  }
  s->ws = ws;
  /* длинное имя не влезает в гвард — честнее отказаться от слепых
     путей для этого стола, чем ездить по обрезанному имени */
  if (name != nullptr && name_len > 0 && name_len < sizeof s->name) {
    memcpy(s->name, name, name_len);
    s->name[name_len] = '\0';
  } else {
    s->name[0] = '\0';
  }
  s->master = master;
  s->partner = -1; /* неизвестен: snap_store_tiles уточнит из Tiled */
  s->n = n;
  if (n > 0)
    memcpy(s->stack, stack, (size_t)n * sizeof stack[0]);
}

/* снимок по уже собранному порядку Tiled: [мастер] + стопка сверху
   вниз. Имя берём из JSON без второго strlen. */
static void snap_store_tiles(int64_t wid, json_object *ws, const Tiled *t) {
  json_object *name_obj = jget(ws, "name");
  const char *wsname = nullptr;
  size_t wsname_len = 0;
  if (name_obj != nullptr) {
    wsname = json_object_get_string(name_obj);
    int nl = json_object_get_string_len(name_obj);
    if (nl > 0)
      wsname_len = (size_t)nl;
  }
  snap_store(wid, wsname, wsname_len, t->id[0], &t->id[1], t->n - 1);
  /* _pN ставится только нашими структурными пачками (они гасят
     снимок), поэтому здесь partner точен: слепые spawn/death могут
     не тратить команду unmark, когда памяти на столе нет. */
  Snap *s = snap_get(wid);
  if (s != nullptr)
    s->partner = t->partner_id;
}

/* ws, на котором умер держатель _m — из marks закрытого окна; 0 = нет.
   После отказа от --replace окно может нести и СТАРЫЕ _m чужих столов
   (метка уникальна и слезает лишь когда там родится новый мастер):
   выбираем стол, где ЭТО окно — мастер по снимку, иначе первый. */
static int64_t dead_master_ws(json_object *con, int64_t cid) {
  json_object *marks = jarr(con, "marks");
  if (marks == nullptr)
    return 0;
  size_t cnt = json_object_array_length(marks);
  int64_t first = 0;
  for (size_t i = 0; i < cnt; i++) {
    json_object *m = json_object_array_get_idx(marks, i);
    const char *s = m != nullptr ? json_object_get_string(m) : nullptr;
    if (s != nullptr && s[0] == '_' && s[1] == 'm') {
      int64_t v = strtoll(s + 2, nullptr, 10);
      if (v > 0) {
        Snap *sn = snap_get(v);
        if (sn != nullptr && sn->master == cid)
          return v;
        if (first == 0)
          first = v;
      }
    }
  }
  return first;
}

/* ── Есть ли выход заметно правее того, на котором стоит ws.
   Нужно ветке «move right из стопки»: с соседним выходом Sway после
   второго move right уносит окно туда, без него — оставляет плоский
   ряд на том же столе. Выходы лежат в корне дерева (type=output),
   rect — в общих координатах. ── */
static bool output_right_of(json_object *root, json_object *ws) {
  const char *name = jstr(ws, "output");
  json_object *outs = jarr(root, "nodes");
  if (name == nullptr || outs == nullptr)
    return false;
  size_t cnt = json_object_array_length(outs);
  json_object *cur = nullptr;
  for (size_t i = 0; i < cnt; i++) {
    json_object *o = json_object_array_get_idx(outs, i);
    const char *t = jstr(o, "type");
    if (t == nullptr || strcmp(t, "output") != 0)
      continue;
    const char *on = jstr(o, "name");
    if (on != nullptr && strcmp(on, name) == 0) {
      cur = o;
      break;
    }
  }
  json_object *cr = jget(cur, "rect");
  if (cr == nullptr)
    return false;
  int64_t cx = jint(cr, "x");
  int64_t cw = jint(cr, "width");
  if (cw <= 0)
    return false;
  for (size_t i = 0; i < cnt; i++) {
    json_object *o = json_object_array_get_idx(outs, i);
    if (o == nullptr || o == cur)
      continue;
    const char *t = jstr(o, "type");
    if (t == nullptr || strcmp(t, "output") != 0)
      continue;
    json_object *r = jget(o, "rect");
    if (r == nullptr)
      continue;
    if (jint(r, "width") > 0 && jint(r, "x") >= cx + cw / 2)
      return true;
  }
  return false;
}
