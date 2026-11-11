/* test_main.c — unit-тесты для sway-mstack.
 *
 * Файл включает src/main.c напрямую: демон — один статический
 * translation unit, и тестировать его внутренние помощники иначе
 * нельзя, не вынося их в отдельную библиотеку. `main` демона
 * переименовывается препроцессором, тестовый `main` остаётся свой.
 *
 * Запуск: cmake --build build --target sms_unit && ctest --test-dir build
 * или напрямую: ./build/sms_unit
 *
 * Стиль: без фреймворков. CHECK печатает файл/строку и выражение,
 * в конце — сводка; ненулевой код возврата = провал.
 */

#define main sms_main
#include "../../src/main.c"
#undef main

#include <sys/socket.h>

/* ══════════════ Мини-фреймворк ══════════════ */

static int g_checks = 0;
static int g_failures = 0;

#define CHECK(cond)                                                            \
  do {                                                                         \
    g_checks++;                                                                \
    if (!(cond)) {                                                             \
      g_failures++;                                                            \
      fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);          \
    }                                                                          \
  } while (0)

#define CHECK_STR(actual, expected)                                            \
  do {                                                                         \
    const char *a_ = (actual);                                                 \
    const char *e_ = (expected);                                               \
    g_checks++;                                                                \
    if (a_ == nullptr || strcmp(a_, e_) != 0) {                                \
      g_failures++;                                                            \
      fprintf(stderr, "FAIL %s:%d: %s\n  ожидалось: \"%s\"\n  получено:  %s\n", \
              __FILE__, __LINE__, #actual, e_, a_ != nullptr ? a_ : "NULL");   \
    }                                                                          \
  } while (0)

static json_object *json(const char *text) {
  json_object *o = parse_json_exact(text, strlen(text));
  if (o == nullptr) {
    fprintf(stderr, "FATAL: тестовая JSON-строка не разбирается: %s\n", text);
    exit(2);
  }
  return o;
}

static void reset_runtime(void) {
  memset(g_snap, 0, sizeof g_snap);
  g_snap_tick = 1;
  memset(g_echo, 0, sizeof g_echo);
  echo_n = 0;
  g_echo_seq = 0;
  g_echo_batch_lo = 0;
  g_struct_sent = 0;
  g_focus_ws = 0;
  g_focus_win = 0;
  g_debug = false;
  MASTER_PCT = 55;
}

static void reset_snapshots(void) {
  memset(g_snap, 0, sizeof g_snap);
  g_snap_tick = 1;
}

static void reset_echo(void) {
  memset(g_echo, 0, sizeof g_echo);
  echo_n = 0;
  g_echo_seq = 0;
  g_echo_batch_lo = 0;
  g_focus_win = 0;
}

static Tiled empty_tiled(void) {
  Tiled t;
  memset(&t, 0, sizeof t);
  t.master_id = -1;
  t.stack_id = -1;
  t.partner_id = -1;
  return t;
}

/* ══════════════ ws_guard ══════════════ */

static void test_ws_guard(void) {
  char out[GUARD_MAX];

  CHECK(ws_guard(out, sizeof out, "1"));
  CHECK_STR(out, "workspace=\"^1$\"");

  CHECK(ws_guard(out, sizeof out, "my ws"));
  CHECK_STR(out, "workspace=\"^my\\ ws$\"");

  CHECK(ws_guard(out, sizeof out, "a:b/c.d"));
  CHECK_STR(out, "workspace=\"^a\\:b\\/c\\.d$\"");

  /* не-алнумерический ASCII — ровно один бэкслэш */
  CHECK(ws_guard(out, sizeof out, "a-b!c"));
  CHECK_STR(out, "workspace=\"^a\\-b\\!c$\"");

  /* UTF-8 идёт байт-в-байт: PCRE2_UTF не должен получить разорванный
     символ */
  CHECK(ws_guard(out, sizeof out, "рабочий"));
  CHECK_STR(out, "workspace=\"^рабочий$\"");

  CHECK(ws_guard(out, sizeof out, "a\"b"));
  CHECK_STR(out, "workspace=\"^a\\\"b$\"");

  CHECK(ws_guard(out, sizeof out, "a\\b"));
  CHECK_STR(out, "workspace=\"^a\\\\b$\"");

  /* хвостовой бэкслэш ломает поиск закрывающей кавычки */
  CHECK(!ws_guard(out, sizeof out, "bad\\"));
  /* бэкслэш перед кавычкой неоднозначен */
  CHECK(!ws_guard(out, sizeof out, "bad\\\"x"));
  /* контроль-байты */
  CHECK(!ws_guard(out, sizeof out, "bad\nname"));
  CHECK(!ws_guard(out, sizeof out, "bad\tname"));
  /* пустое/NULL имя */
  CHECK(!ws_guard(out, sizeof out, ""));
  CHECK(!ws_guard(out, sizeof out, nullptr));
  /* слишком маленький буфер */
  char tiny[16];
  CHECK(!ws_guard(tiny, 15, "1"));
  /* имя не влезает в буфер */
  CHECK(!ws_guard(tiny, 16, "0123456789"));
  /* ровно помещается: 12 байт префикса + 1 имя + $ " NUL = 16 */
  CHECK(ws_guard(tiny, 16, "1"));
  CHECK_STR(tiny, "workspace=\"^1$\"");
}

/* ══════════════ ws_focus_cmd ══════════════ */

static void test_ws_focus_cmd(void) {
  char out[WSNAME_MAX + 64];

  CHECK(ws_focus_cmd(out, sizeof out, "1"));
  CHECK_STR(out, "workspace --no-auto-back-and-forth \"1\"");

  CHECK(ws_focus_cmd(out, sizeof out, "мой стол"));
  CHECK_STR(out, "workspace --no-auto-back-and-forth \"мой стол\"");

  CHECK(ws_focus_cmd(out, sizeof out, "a b"));
  CHECK_STR(out, "workspace --no-auto-back-and-forth \"a b\"");

  static const char *const kw[] = {
      "next", "prev", "current", "back_and_forth", "next_on_output",
      "prev_on_output", "number", "output", "gaps",
      "--no-auto-back-and-forth", "NEXT", "Output", nullptr};
  for (int i = 0; kw[i] != nullptr; i++)
    CHECK(!ws_focus_cmd(out, sizeof out, kw[i]));

  CHECK(!ws_focus_cmd(out, sizeof out, "a\"b"));
  CHECK(!ws_focus_cmd(out, sizeof out, "a'b"));
  CHECK(!ws_focus_cmd(out, sizeof out, "a\\b"));
  CHECK(!ws_focus_cmd(out, sizeof out, "a\nb"));
  CHECK(!ws_focus_cmd(out, sizeof out, "a\tb"));
  CHECK(!ws_focus_cmd(out, sizeof out, ""));
  CHECK(!ws_focus_cmd(out, sizeof out, nullptr));

  /* буфер меньше команды — честный отказ, не обрезок */
  char small[16];
  CHECK(!ws_focus_cmd(small, sizeof small, "1"));
  CHECK(!ws_focus_cmd(out, 0, "1"));

  /* точный размер: длина + NUL */
  char exact[sizeof("workspace --no-auto-back-and-forth \"1\"")];
  CHECK(ws_focus_cmd(exact, sizeof exact, "1"));
  CHECK_STR(exact, "workspace --no-auto-back-and-forth \"1\"");
}

/* ══════════════ JSON-доступоры ══════════════ */

static void test_json_helpers(void) {
  json_object *o = json("{\"i\":42,\"big\":9007199254740993,"
                        "\"b\":true,\"f\":false,\"s\":\"x\","
                        "\"a\":[1,2],\"o\":{\"k\":1},\"n\":null}");

  CHECK(jint(o, "i") == 42);
  CHECK(jint(o, "big") == 9007199254740993LL);
  CHECK(jint(o, "missing") == -1);
  CHECK(jint(o, "s") == 0); /* json-c конвертирует, это не ошибка */

  CHECK(jstr(o, "s") != nullptr && strcmp(jstr(o, "s"), "x") == 0);
  CHECK(jstr(o, "missing") == nullptr);

  CHECK(jbool(o, "b"));
  CHECK(!jbool(o, "f"));
  CHECK(!jbool(o, "missing"));

  CHECK(jarr(o, "a") != nullptr);
  CHECK(json_object_array_length(jarr(o, "a")) == 2);
  CHECK(jarr(o, "o") == nullptr); /* объект — не массив */
  CHECK(jget(o, "o") != nullptr);
  /* json-c не хранит null-члены: ключ с JSON null исчезает */
  CHECK(jget(o, "n") == nullptr);
  CHECK(jget(o, "missing") == nullptr);

  CHECK(jget(nullptr, "a") == nullptr);
  CHECK(jarr(nullptr, "a") == nullptr);
  CHECK(jstr(nullptr, "a") == nullptr);
  CHECK(jint(nullptr, "a") == -1);
  CHECK(!jbool(nullptr, "a"));

  json_object_put(o);

  json_object *l = json("{\"layout\":\"splith\"}");
  CHECK(layout_is(l, "splith"));
  CHECK(!layout_is(l, "splitv"));
  CHECK(!layout_is(l, ""));
  json_object_put(l);
  CHECK(!layout_is(nullptr, "splith"));

  json_object *f = json("{\"floating\":\"auto_on\"}");
  CHECK(is_floating(f));
  json_object_put(f);
  f = json("{\"floating\":\"user_on\"}");
  CHECK(is_floating(f));
  json_object_put(f);
  f = json("{\"floating\":\"auto_off\"}");
  CHECK(!is_floating(f));
  json_object_put(f);
  f = json("{\"floating\":\"user_off\"}");
  CHECK(!is_floating(f));
  json_object_put(f);
  f = json("{}");
  CHECK(!is_floating(f));
  json_object_put(f);
  CHECK(!is_floating(nullptr));
}

/* ══════════════ is_tiled ══════════════ */

static void test_is_tiled(void) {
  struct {
    const char *text;
    bool want;
  } cases[] = {
      {"{\"type\":\"con\"}", true},
      {"{\"type\":\"con\",\"nodes\":[]}", true},
      {"{\"type\":\"con\",\"nodes\":[{}]}", false},
      {"{\"type\":\"con\",\"floating\":\"auto_on\"}", false},
      {"{\"type\":\"con\",\"floating\":\"user_on\"}", false},
      {"{\"type\":\"con\",\"floating\":\"auto_off\"}", true},
      {"{\"type\":\"con\",\"scratchpad_state\":\"fresh\"}", false},
      {"{\"type\":\"con\",\"scratchpad_state\":\"changed\"}", false},
      {"{\"type\":\"con\",\"scratchpad_state\":\"none\"}", true},
      {"{\"type\":\"workspace\"}", false},
      {"{\"type\":\"floating_con\"}", false},
      {"{}", false},
  };
  for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
    json_object *o = json(cases[i].text);
    bool got = is_tiled(o);
    g_checks++;
    if (got != cases[i].want) {
      g_failures++;
      fprintf(stderr, "FAIL is_tiled(%s): ожидалось %d, получено %d\n",
              cases[i].text, (int)cases[i].want, (int)got);
    }
    json_object_put(o);
  }
}

/* ══════════════ collect / idx_of ══════════════ */

static const char *TREE_FIXTURE =
    "{"
    "\"id\":4,\"type\":\"workspace\",\"name\":\"1\",\"layout\":\"splith\","
    "\"rect\":{\"x\":0,\"y\":0,\"width\":1280,\"height\":720},"
    "\"nodes\":["
    "{\"id\":10,\"type\":\"con\",\"app_id\":\"a\",\"layout\":\"none\","
    "\"rect\":{\"x\":0,\"y\":0,\"width\":704,\"height\":720},"
    "\"marks\":[\"_m4\"],\"nodes\":[]},"
    "{\"id\":11,\"type\":\"con\",\"layout\":\"splitv\","
    "\"rect\":{\"x\":704,\"y\":0,\"width\":576,\"height\":720},"
    "\"marks\":[],\"nodes\":["
    "{\"id\":20,\"type\":\"con\",\"app_id\":\"b\",\"layout\":\"none\","
    "\"rect\":{\"x\":704,\"y\":0,\"width\":576,\"height\":360},"
    "\"marks\":[\"_s4\"],\"nodes\":[]},"
    "{\"id\":30,\"type\":\"con\",\"app_id\":\"c\",\"layout\":\"none\","
    "\"rect\":{\"x\":704,\"y\":360,\"width\":576,\"height\":360},"
    "\"marks\":[\"_p4\"],\"nodes\":[]}"
    "]}"
    "]}";

static void test_collect(void) {
  json_object *ws = json(TREE_FIXTURE);
  Tiled t = empty_tiled();
  t.ws_w = jint(jget(ws, "rect"), "width");
  collect(ws, ws, "_m4", "_s4", "_p4", &t);

  CHECK(t.n == 3);
  CHECK(t.ws_w == 1280);
  CHECK(t.id[0] == 10 && t.id[1] == 20 && t.id[2] == 30);
  CHECK(t.rw[0] == 704 && t.rw[1] == 576 && t.rw[2] == 576);
  CHECK(t.master_id == 10);
  CHECK(t.stack_id == 20);
  CHECK(t.partner_id == 30);
  CHECK(t.parent[0] == ws);
  CHECK(t.parent[1] != ws && t.parent[1] == t.parent[2]);

  CHECK(idx_of(&t, 10) == 0);
  CHECK(idx_of(&t, 20) == 1);
  CHECK(idx_of(&t, 30) == 2);
  CHECK(idx_of(&t, 40) == -1);
  CHECK(idx_of(&t, 0) == -1);
  CHECK(idx_of(&t, -1) == -1);

  Tiled empty = empty_tiled();
  CHECK(idx_of(&empty, 10) == -1);

  json_object_put(ws);

  /* Метка на контейнере без окон тоже находится: collect смотрит
     marks до проверки is_tiled. */
  json_object *odd = json("{\"id\":5,\"type\":\"workspace\","
                          "\"nodes\":[{\"id\":50,\"type\":\"con\","
                          "\"nodes\":[{\"id\":51,\"type\":\"con\"}]}]}");
  Tiled o = empty_tiled();
  collect(odd, odd, "_m5", "_s5", "_p5", &o);
  CHECK(o.n == 1 && o.id[0] == 51);
  json_object_put(odd);

  /* Переполнение TILED_MAX: лишние окна не управляются, но память
     не разъезжается. Предупреждение печатается один раз на процесс. */
  json_object *big = json_object_new_object();
  json_object_object_add(big, "id", json_object_new_int64(9));
  json_object_object_add(big, "type", json_object_new_string("workspace"));
  json_object *nodes = json_object_new_array();
  for (int i = 0; i < TILED_MAX + 4; i++) {
    json_object *c = json_object_new_object();
    json_object_object_add(c, "id", json_object_new_int64(100 + i));
    json_object_object_add(c, "type", json_object_new_string("con"));
    json_object_object_add(c, "nodes", json_object_new_array());
    json_object_array_add(nodes, c);
  }
  json_object_object_add(big, "nodes", nodes);
  Tiled b = empty_tiled();
  collect(big, big, "_m9", "_s9", "_p9", &b);
  CHECK(b.n == TILED_MAX);
  CHECK(b.id[0] == 100);
  CHECK(b.id[TILED_MAX - 1] == 100 + TILED_MAX - 1);
  json_object_put(big);
}

/* ══════════════ proportions_ok ══════════════ */

static void test_proportions(void) {
  Tiled t = empty_tiled();

  /* меньше двух окон — понятие доли не применяется */
  t.n = 0;
  CHECK(proportions_ok(&t, 0));
  t.n = 1;
  CHECK(proportions_ok(&t, 0));

  t.n = 2;
  t.rw[0] = 0;
  CHECK(proportions_ok(&t, 100)); /* ширина ещё не измерена */

  t.rw[0] = 55;
  CHECK(proportions_ok(&t, 45)); /* 55% */
  t.rw[0] = 52;
  CHECK(proportions_ok(&t, 48)); /* нижняя граница допуска */
  t.rw[0] = 58;
  CHECK(proportions_ok(&t, 42)); /* верхняя граница */
  t.rw[0] = 51;
  CHECK(!proportions_ok(&t, 49));
  t.rw[0] = 59;
  CHECK(!proportions_ok(&t, 41));

  /* zone_w <= 0 — база берётся из ширины воркспейса */
  t.ws_w = 100;
  t.rw[0] = 55;
  CHECK(proportions_ok(&t, 0));
  t.rw[0] = 51;
  CHECK(!proportions_ok(&t, 0));
  t.ws_w = 0;
  CHECK(proportions_ok(&t, 0)); /* базы нет — не трогаем */

  /* округление вниз: 549/1000 = 54% — в допуске */
  t.ws_w = 0;
  t.rw[0] = 549;
  CHECK(proportions_ok(&t, 451));
  t.rw[0] = 519;
  CHECK(!proportions_ok(&t, 481));

  /* MASTER_PCT меняется аргументом запуска — проверяем с 60 */
  MASTER_PCT = 60;
  t.rw[0] = 57;
  CHECK(proportions_ok(&t, 43));
  t.rw[0] = 56;
  CHECK(!proportions_ok(&t, 44));
  t.rw[0] = 63;
  CHECK(proportions_ok(&t, 37));
  t.rw[0] = 64;
  CHECK(!proportions_ok(&t, 36));
  MASTER_PCT = 55;
}

/* ══════════════ master_out_of_slot (watchdog свапа) ══════════════ */

static void test_master_out_of_slot(void) {
  json_object *ws = json(TREE_FIXTURE);
  Tiled t = empty_tiled();
  collect(ws, ws, "_m4", "_s4", "_p4", &t);
  CHECK(!master_out_of_slot(&t)); /* канон: _m в слоте 0 */
  json_object_put(ws);

  /* свап центра: мастер уехал в стопку, на его месте — её вершина */
  json_object *sw = json("{\"id\":4,\"type\":\"workspace\","
                         "\"nodes\":["
                         "{\"id\":20,\"type\":\"con\",\"marks\":[\"_s4\"]},"
                         "{\"id\":11,\"type\":\"con\",\"layout\":\"splitv\","
                         "\"nodes\":["
                         "{\"id\":10,\"type\":\"con\",\"marks\":[\"_m4\"]},"
                         "{\"id\":30,\"type\":\"con\",\"marks\":[]}]}]}");
  Tiled s = empty_tiled();
  collect(sw, sw, "_m4", "_s4", "_p4", &s);
  CHECK(s.n == 3);
  CHECK(s.master_id == 10 && s.id[0] == 20);
  CHECK(master_out_of_slot(&s));
  json_object_put(sw);

  Tiled one = empty_tiled();
  one.n = 1;
  one.id[0] = 10;
  one.master_id = 10;
  CHECK(!master_out_of_slot(&one)); /* одно окно — свапа нет */
  one.master_id = -1;
  CHECK(!master_out_of_slot(&one)); /* метки нет — не спорим */
  one.n = 2;
  one.id[1] = 11;
  one.master_id = 11;
  CHECK(master_out_of_slot(&one)); /* мастер в стопке */
  one.master_id = 10;
  CHECK(!master_out_of_slot(&one)); /* мастер на месте */
  one.master_id = 99;
  CHECK(!master_out_of_slot(&one)); /* метка на не-слоте — не спорим */
}

/* ══════════════ dead_master_ws ══════════════ */

static void test_dead_master_ws(void) {
  reset_snapshots();

  json_object *c = json("{\"id\":10,\"marks\":[\"_m4\"]}");
  CHECK(dead_master_ws(c, 10) == 4);
  json_object_put(c);

  c = json("{\"id\":10,\"marks\":[\"_s4\",\"_p4\",\"foo\"]}");
  CHECK(dead_master_ws(c, 10) == 0);
  json_object_put(c);

  c = json("{\"id\":10}");
  CHECK(dead_master_ws(c, 10) == 0);
  json_object_put(c);

  CHECK(dead_master_ws(nullptr, 10) == 0);

  /* мусорные метки: 0, отрицательные и не-числа отбрасываются */
  c = json("{\"id\":10,\"marks\":[\"_m0\",\"_m-3\",\"_mx\",\"_m\"]}");
  CHECK(dead_master_ws(c, 10) == 0);
  json_object_put(c);

  /* несколько _m: выбирается стол, где это окно — мастер по снимку */
  snap_store(4, "1", 1, 10, nullptr, 0);
  c = json("{\"id\":10,\"marks\":[\"_m5\",\"_m4\"]}");
  CHECK(dead_master_ws(c, 10) == 4);
  json_object_put(c);

  /* без снимка — первая валидная метка */
  reset_snapshots();
  c = json("{\"id\":10,\"marks\":[\"_m5\",\"_m4\"]}");
  CHECK(dead_master_ws(c, 10) == 5);
  json_object_put(c);

  /* снимок есть, но мастер в нём — другое окно: первая метка */
  snap_store(4, "1", 1, 99, nullptr, 0);
  c = json("{\"id\":10,\"marks\":[\"_m5\",\"_m4\"]}");
  CHECK(dead_master_ws(c, 10) == 5);
  json_object_put(c);

  /* marks не массив — молча 0 */
  c = json("{\"id\":10,\"marks\":\"_m4\"}");
  CHECK(dead_master_ws(c, 10) == 0);
  json_object_put(c);
}

/* ══════════════ Снимки столов ══════════════ */

static void test_snapshots(void) {
  reset_snapshots();

  CHECK(snap_get(0) == nullptr);
  CHECK(snap_get(-1) == nullptr);
  CHECK(snap_get(4) == nullptr);

  int64_t stack[] = {20, 30};
  snap_store(4, "1", 1, 10, stack, 2);
  Snap *s = snap_get(4);
  CHECK(s != nullptr);
  if (s != nullptr) {
    CHECK(s->ws == 4);
    CHECK_STR(s->name, "1");
    CHECK(s->master == 10);
    CHECK(s->partner == -1);
    CHECK(s->n == 2);
    CHECK(s->stack[0] == 20 && s->stack[1] == 30);
  }

  snap_evict(4);
  CHECK(snap_get(4) == nullptr);
  snap_evict(4); /* повторно — no-op */
  snap_evict(0);
  snap_evict(-5);

  /* границы длины имени: 95 влезает, 96 — уже нет */
  char name[WSNAME_MAX + 1];
  memset(name, 'a', sizeof name);
  name[WSNAME_MAX - 1] = '\0';
  snap_store(5, name, WSNAME_MAX - 1, 10, nullptr, 0);
  s = snap_get(5);
  CHECK(s != nullptr && s->name[0] == 'a');
  CHECK(s != nullptr && strlen(s->name) == WSNAME_MAX - 1);

  name[WSNAME_MAX - 1] = 'a';
  name[WSNAME_MAX] = '\0';
  snap_store(6, name, WSNAME_MAX, 10, nullptr, 0);
  s = snap_get(6);
  CHECK(s != nullptr && s->name[0] == '\0');

  /* NULL/пустое имя — слепые пути для стола отключаются */
  snap_store(7, nullptr, 0, 10, nullptr, 0);
  s = snap_get(7);
  CHECK(s != nullptr && s->name[0] == '\0');
  snap_store(8, "", 0, 10, nullptr, 0);
  s = snap_get(8);
  CHECK(s != nullptr && s->name[0] == '\0');

  /* невалидные ws/n не создают снимок */
  reset_snapshots();
  snap_store(0, "1", 1, 10, nullptr, 0);
  snap_store(-1, "1", 1, 10, nullptr, 0);
  snap_store(4, "1", 1, 10, nullptr, -1);
  snap_store(4, "1", 1, 10, nullptr, TILED_MAX + 1);
  CHECK(snap_get(4) == nullptr);

  /* LRU: snap_get обновляет tick, поэтому вытесняется самый давний
     по ЧТЕНИЮ, а не по записи */
  reset_snapshots();
  for (int i = 1; i <= WS_SNAP; i++)
    snap_store(i, "x", 1, 100 + i, nullptr, 0);
  CHECK(snap_get(1) != nullptr); /* 1 — теперь самая свежая */
  snap_store(WS_SNAP + 1, "y", 1, 999, nullptr, 0);
  CHECK(snap_get(2) == nullptr); /* 2 — самая старая */
  CHECK(snap_get(1) != nullptr);
  CHECK(snap_get(WS_SNAP + 1) != nullptr);

  /* снапшот по Tiled: имя из JSON, порядок стопки, partner */
  reset_snapshots();
  json_object *ws = json(TREE_FIXTURE);
  Tiled t = empty_tiled();
  collect(ws, ws, "_m4", "_s4", "_p4", &t);
  snap_store_tiles(4, ws, &t);
  s = snap_get(4);
  CHECK(s != nullptr);
  if (s != nullptr) {
    CHECK_STR(s->name, "1");
    CHECK(s->master == 10);
    CHECK(s->partner == 30);
    CHECK(s->n == 2);
    CHECK(s->stack[0] == 20 && s->stack[1] == 30);
  }
  json_object_put(ws);

  /* t.n == 1: стопка пуста, partner всё равно проставляется */
  reset_snapshots();
  json_object *single =
      json("{\"id\":4,\"name\":\"dev\",\"nodes\":["
           "{\"id\":10,\"type\":\"con\",\"marks\":[\"_m4\"],\"nodes\":[]}]}");
  Tiled one = empty_tiled();
  collect(single, single, "_m4", "_s4", "_p4", &one);
  snap_store_tiles(4, single, &one);
  s = snap_get(4);
  CHECK(s != nullptr && s->n == 0 && s->master == 10);
  CHECK(s != nullptr && s->partner == -1);
  json_object_put(single);

  /* t.n == 0: снимок не создаётся (n = -1 отбрасывается) */
  reset_snapshots();
  json_object *none = json("{\"id\":4,\"name\":\"empty\",\"nodes\":[]}");
  Tiled zero = empty_tiled();
  collect(none, none, "_m4", "_s4", "_p4", &zero);
  snap_store_tiles(4, none, &zero);
  CHECK(snap_get(4) == nullptr);
  json_object_put(none);

  /* длинное имя в JSON — снимок без имени, честный путь */
  reset_snapshots();
  char longname[WSNAME_MAX + 8];
  memset(longname, 'n', sizeof longname - 1);
  longname[sizeof longname - 1] = '\0';
  json_object *lw = json_object_new_object();
  json_object_object_add(lw, "id", json_object_new_int64(4));
  json_object_object_add(lw, "name", json_object_new_string(longname));
  json_object_object_add(lw, "nodes", json_object_new_array());
  Tiled lt = empty_tiled();
  lt.n = 1;
  lt.id[0] = 10;
  snap_store_tiles(4, lw, &lt);
  s = snap_get(4);
  CHECK(s != nullptr && s->name[0] == '\0');
  json_object_put(lw);
}

/* ══════════════ Кэш эха ══════════════ */

static void test_echo(void) {
  reset_echo();

  echo_track(10);
  CHECK(echo_n == 1);
  CHECK(g_echo[0].id == 10);
  CHECK(g_echo[0].ttl == ECHO_TTL_EXPLICIT_MS);
  CHECK(echo_consume(10));
  CHECK(echo_n == 0);
  CHECK(!echo_consume(10));

  /* короткая запись под фокусом — вероятно, жест юзера: не глотаем,
     но запись снимаем */
  reset_echo();
  g_focus_win = 12;
  echo_track_ttl(12, ECHO_TTL_SHORT_MS);
  CHECK(!echo_consume(12));
  CHECK(echo_n == 0);

  /* явная запись под фокусом — наше эхо, глотаем */
  reset_echo();
  g_focus_win = 12;
  echo_track(12);
  CHECK(echo_consume(12));
  CHECK(echo_n == 0);

  /* обычный move отменяет короткую запись про то же окно: на одно
     действие — одно событие */
  reset_echo();
  echo_track_ttl(13, ECHO_TTL_SHORT_MS);
  echo_track(13);
  CHECK(echo_n == 1);
  CHECK(g_echo[0].ttl == ECHO_TTL_EXPLICIT_MS);
  CHECK(echo_consume(13));
  CHECK(!echo_consume(13));

  /* close гасит все записи окна */
  reset_echo();
  echo_track(14);
  echo_track(14);
  CHECK(echo_n == 2);
  echo_forget(14);
  CHECK(echo_n == 0);
  echo_forget(99); /* чужое окно — no-op */
  CHECK(echo_n == 0);

  /* откат неудачной пачки: остаются записи до lo */
  reset_echo();
  echo_track(15);
  uint64_t lo = g_echo_seq;
  echo_track(16);
  echo_track(17);
  echo_rollback(lo);
  CHECK(echo_n == 1);
  CHECK(g_echo[0].id == 15);
  echo_rollback(0);
  CHECK(echo_n == 0);

  /* протухание: consume стряхивает просроченное */
  reset_echo();
  echo_track_ttl(18, 100);
  g_echo[0].t = now_ms() - 200;
  CHECK(!echo_consume(18));
  CHECK(echo_n == 0);

  /* prune чистит всё просроченное, свежее оставляет */
  reset_echo();
  echo_track_ttl(19, 100);
  echo_track_ttl(20, 500);
  g_echo[0].t = now_ms() - 200;
  echo_prune();
  CHECK(echo_n == 1);
  CHECK(g_echo[0].id == 20);

  /* идемпотентность: невалидные id игнорируются */
  reset_echo();
  echo_track(0);
  echo_track(-5);
  echo_track_ttl(0, ECHO_TTL_SHORT_MS);
  CHECK(echo_n == 0);

  /* переполнение: лишняя запись отбрасывается, массив не разъезжается.
     Один warning в stderr — ожидаемо. */
  reset_echo();
  for (int i = 0; i < ECHO_MAX; i++)
    echo_track(1000 + i);
  CHECK(echo_n == ECHO_MAX);
  echo_track(9999);
  CHECK(echo_n == ECHO_MAX);
  CHECK(g_echo[ECHO_MAX - 1].id == 1000 + ECHO_MAX - 1);
}

/* ══════════════ cmd_append / cmd_struct / cmd_move ══════════════ */

static void test_cmd_builders(void) {
  reset_runtime();

  char buf[64];
  size_t off = 0;
  buf[0] = '\0';

  CHECK(cmd_append(buf, sizeof buf, &off, "one"));
  CHECK_STR(buf, "one");
  CHECK(off == 3);
  CHECK(g_echo_batch_lo == g_echo_seq);

  echo_track(5); /* имитируем команду между append'ами */
  uint64_t batch_lo = g_echo_batch_lo;
  CHECK(cmd_append(buf, sizeof buf, &off, "two"));
  CHECK_STR(buf, "one; two");
  CHECK(off == 8);
  CHECK(g_echo_batch_lo == batch_lo); /* граница пачки не сдвинулась */

  /* не влезает — false, буфер и off не тронуты */
  char small[8];
  size_t soff = 0;
  small[0] = '\0';
  CHECK(cmd_append(small, sizeof small, &soff, "abcd")); /* 4+3 < 8 */
  CHECK_STR(small, "abcd");
  CHECK(!cmd_append(small, sizeof small, &soff, "ef")); /* 4+2+3 >= 8 */
  CHECK(soff == 4);
  CHECK_STR(small, "abcd");

  /* ровно допустимая граница: off + len + 3 < cap, здесь 5+3 = 8 < 9 */
  char exact[9];
  size_t eoff = 0;
  exact[0] = '\0';
  CHECK(cmd_append(exact, sizeof exact, &eoff, "abcde"));
  CHECK_STR(exact, "abcde");

  /* структурная команда считается, провалившаяся — нет */
  reset_runtime();
  char sbuf[16];
  size_t soff2 = 0;
  sbuf[0] = '\0';
  CHECK(cmd_struct(sbuf, sizeof sbuf, &soff2, "abcd"));
  CHECK(g_struct_sent == 1);
  CHECK(!cmd_struct(sbuf, sizeof sbuf, &soff2, "0123456789"));
  CHECK(g_struct_sent == 1);

  char tiny[8];
  size_t toff = 0;
  tiny[0] = '\0';
  CHECK(cmd_struct(tiny, sizeof tiny, &toff, "abcd"));
  CHECK(g_struct_sent == 2);
  CHECK(!cmd_struct(tiny, sizeof tiny, &toff, "ef"));
  CHECK(g_struct_sent == 2);

  /* move: структура + эхо; move_short: короткий TTL */
  reset_runtime();
  char mbuf[64];
  size_t moff = 0;
  mbuf[0] = '\0';
  CHECK(cmd_move(mbuf, sizeof mbuf, &moff, "move left", 42));
  CHECK(g_struct_sent == 1);
  CHECK(echo_n == 1 && g_echo[0].id == 42);
  CHECK(g_echo[0].ttl == ECHO_TTL_EXPLICIT_MS);

  reset_runtime();
  size_t shoff = 0;
  mbuf[0] = '\0';
  CHECK(cmd_move_short(mbuf, sizeof mbuf, &shoff, "move right", 43));
  CHECK(g_struct_sent == 1);
  CHECK(echo_n == 1 && g_echo[0].id == 43);
  CHECK(g_echo[0].ttl == ECHO_TTL_SHORT_MS);

  /* провал move не оставляет эха */
  reset_runtime();
  char mtiny[8];
  size_t mtoff = 0;
  mtiny[0] = '\0';
  CHECK(!cmd_move(mtiny, sizeof mtiny, &mtoff, "abcdef", 44));
  CHECK(echo_n == 0);
  CHECK(g_struct_sent == 0);
}

/* ══════════════ parse_json_exact ══════════════ */

static void test_parse_json_exact(void) {
  const char *text = "{\"a\":1}";
  json_object *o = parse_json_exact(text, strlen(text));
  CHECK(o != nullptr);
  if (o != nullptr) {
    CHECK(jint(o, "a") == 1);
    json_object_put(o);
  }

  /* длина точная, NUL-терминатор не обязателен */
  char *raw = malloc(7);
  CHECK(raw != nullptr);
  if (raw != nullptr) {
    memcpy(raw, "{\"a\":1}", 7);
    o = parse_json_exact(raw, 7);
    CHECK(o != nullptr);
    if (o != nullptr)
      json_object_put(o);
    free(raw);
  }

  CHECK(parse_json_exact(nullptr, 10) == nullptr);
  CHECK(parse_json_exact("", 0) == nullptr);
  CHECK(parse_json_exact("{\"a\":", 5) == nullptr);
  CHECK(parse_json_exact("garbage", 7) == nullptr);
  /* API json-c принимает int; больше — отказ до чтения буфера */
  CHECK(parse_json_exact("x", (size_t)INT_MAX + 1) == nullptr);

  o = parse_json_exact("[1,2,3]", 7);
  CHECK(o != nullptr && json_object_is_type(o, json_type_array));
  if (o != nullptr)
    json_object_put(o);
}

/* ══════════════ output_right_of ══════════════ */

static void test_output_right_of(void) {
  json_object *root = json(
      "{\"nodes\":["
      "{\"type\":\"output\",\"name\":\"HEADLESS-1\","
      "\"rect\":{\"x\":0,\"y\":0,\"width\":1280,\"height\":720}},"
      "{\"type\":\"output\",\"name\":\"HEADLESS-2\","
      "\"rect\":{\"x\":1280,\"y\":0,\"width\":1920,\"height\":1080}},"
      "{\"type\":\"con\",\"name\":\"not-output\","
      "\"rect\":{\"x\":5000,\"y\":0,\"width\":100,\"height\":100}}"
      "]}");

  json_object *ws1 = json("{\"output\":\"HEADLESS-1\"}");
  json_object *ws2 = json("{\"output\":\"HEADLESS-2\"}");
  json_object *wsx = json("{\"output\":\"NOPE\"}");
  json_object *wsm = json("{}");

  CHECK(output_right_of(root, ws1));
  CHECK(!output_right_of(root, ws2)); /* правее ничего нет */
  CHECK(!output_right_of(root, wsx));
  CHECK(!output_right_of(root, wsm));
  CHECK(!output_right_of(root, nullptr));
  CHECK(!output_right_of(nullptr, ws1));

  json_object_put(ws1);
  json_object_put(ws2);
  json_object_put(wsx);
  json_object_put(wsm);
  json_object_put(root);

  /* порог: кандидат должен начинаться не левее cx + cw/2 */
  json_object *edge = json(
      "{\"nodes\":["
      "{\"type\":\"output\",\"name\":\"L\","
      "\"rect\":{\"x\":0,\"y\":0,\"width\":1000,\"height\":500}},"
      "{\"type\":\"output\",\"name\":\"R\","
      "\"rect\":{\"x\":500,\"y\":0,\"width\":500,\"height\":500}}"
      "]}");
  json_object *wsl = json("{\"output\":\"L\"}");
  CHECK(output_right_of(edge, wsl));
  json_object_put(edge);

  edge = json(
      "{\"nodes\":["
      "{\"type\":\"output\",\"name\":\"L\","
      "\"rect\":{\"x\":0,\"y\":0,\"width\":1000,\"height\":500}},"
      "{\"type\":\"output\",\"name\":\"R\","
      "\"rect\":{\"x\":499,\"y\":0,\"width\":500,\"height\":500}}"
      "]}");
  CHECK(!output_right_of(edge, wsl));
  json_object_put(edge);

  /* нулевая ширина кандидата/текущего — не считается */
  edge = json(
      "{\"nodes\":["
      "{\"type\":\"output\",\"name\":\"L\","
      "\"rect\":{\"x\":0,\"y\":0,\"width\":0,\"height\":500}},"
      "{\"type\":\"output\",\"name\":\"R\","
      "\"rect\":{\"x\":100,\"y\":0,\"width\":500,\"height\":500}}"
      "]}");
  json_object *wsl2 = json("{\"output\":\"L\"}");
  CHECK(!output_right_of(edge, wsl2));
  json_object_put(edge);

  edge = json(
      "{\"nodes\":["
      "{\"type\":\"output\",\"name\":\"L\","
      "\"rect\":{\"x\":0,\"y\":0,\"width\":1000,\"height\":500}},"
      "{\"type\":\"output\",\"name\":\"R\","
      "\"rect\":{\"x\":2000,\"y\":0,\"width\":0,\"height\":500}}"
      "]}");
  CHECK(!output_right_of(edge, wsl2));
  json_object_put(edge);

  /* выход вертикально над текущим — не «правее» */
  edge = json(
      "{\"nodes\":["
      "{\"type\":\"output\",\"name\":\"L\","
      "\"rect\":{\"x\":0,\"y\":0,\"width\":1000,\"height\":500}},"
      "{\"type\":\"output\",\"name\":\"TOP\","
      "\"rect\":{\"x\":0,\"y\":-600,\"width\":1000,\"height\":500}}"
      "]}");
  CHECK(!output_right_of(edge, wsl2));
  json_object_put(edge);

  json_object_put(wsl);
  json_object_put(wsl2);

  /* нет nodes / rect — false, не падение */
  edge = json("{}");
  json_object *wsl3 = json("{\"output\":\"L\"}");
  CHECK(!output_right_of(edge, wsl3));
  json_object_put(edge);
  edge = json("{\"nodes\":[{\"type\":\"output\",\"name\":\"L\"}]}");
  CHECK(!output_right_of(edge, wsl3));
  json_object_put(edge);
  json_object_put(wsl3);
}

/* ══════════════ is_marked_daemon ══════════════ */

static void test_is_marked_daemon(void) {
  struct {
    const char *text;
    bool want;
  } cases[] = {
      {"{\"marks\":[\"_m4\"]}", true},
      {"{\"marks\":[\"_s4\"]}", true},
      {"{\"marks\":[\"_p4\"]}", true},
      {"{\"marks\":[\"foo\",\"_p4\"]}", true},
      {"{\"marks\":[\"foo\",\"_x4\"]}", false},
      {"{\"marks\":[\"M4\"]}", false},
      {"{\"marks\":[\"_\"]}", false},
      {"{\"marks\":[\"_q4\"]}", false},
      {"{\"marks\":[]}", false},
      {"{\"marks\":\"_m4\"}", false},
      {"{}", false},
  };
  for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
    json_object *o = json(cases[i].text);
    bool got = is_marked_daemon(o);
    g_checks++;
    if (got != cases[i].want) {
      g_failures++;
      fprintf(stderr, "FAIL is_marked_daemon(%s): ожидалось %d, получено %d\n",
              cases[i].text, (int)cases[i].want, (int)got);
    }
    json_object_put(o);
  }
  CHECK(!is_marked_daemon(nullptr));
}

/* ══════════════ find_focused ══════════════ */

static void test_find_focused(void) {
  json_object *ws_out = nullptr;

  json_object *tree = json(
      "{\"type\":\"root\",\"nodes\":["
      "{\"id\":4,\"type\":\"workspace\",\"name\":\"1\",\"focused\":true,"
      "\"nodes\":[]}"
      "]}");
  CHECK(find_focused(tree, nullptr, &ws_out) == nullptr);
  CHECK(ws_out != nullptr && jint(ws_out, "id") == 4);
  json_object_put(tree);

  ws_out = nullptr;
  tree = json(
      "{\"type\":\"root\",\"nodes\":["
      "{\"id\":4,\"type\":\"workspace\",\"name\":\"1\",\"focused\":true,"
      "\"nodes\":["
      "{\"id\":5,\"type\":\"con\",\"focused\":false,\"nodes\":["
      "{\"id\":6,\"type\":\"con\",\"focused\":true,\"nodes\":[]}]}"
      "]}"
      "]}");
  json_object *leaf = find_focused(tree, nullptr, &ws_out);
  CHECK(leaf != nullptr && jint(leaf, "id") == 6);
  CHECK(ws_out != nullptr && jint(ws_out, "id") == 4);
  json_object_put(tree);

  /* плавающее окно — тоже лист */
  ws_out = nullptr;
  tree = json(
      "{\"type\":\"root\",\"nodes\":["
      "{\"id\":4,\"type\":\"workspace\",\"name\":\"1\",\"focused\":true,"
      "\"nodes\":[],\"floating_nodes\":["
      "{\"id\":7,\"type\":\"floating_con\",\"focused\":true,\"nodes\":[]}]}"
      "]}");
  leaf = find_focused(tree, nullptr, &ws_out);
  CHECK(leaf != nullptr && jint(leaf, "id") == 7);
  json_object_put(tree);

  /* несфокусированный стол и окна — ничего */
  ws_out = nullptr;
  tree = json(
      "{\"type\":\"root\",\"nodes\":["
      "{\"id\":4,\"type\":\"workspace\",\"name\":\"1\",\"focused\":false,"
      "\"nodes\":[{\"id\":5,\"type\":\"con\",\"focused\":false,\"nodes\":[]}]}"
      "]}");
  CHECK(find_focused(tree, nullptr, &ws_out) == nullptr);
  CHECK(ws_out == nullptr);
  json_object_put(tree);

  /* контейнер с детьми не может быть листом, даже если focused */
  ws_out = nullptr;
  tree = json(
      "{\"type\":\"root\",\"nodes\":["
      "{\"id\":4,\"type\":\"workspace\",\"name\":\"1\",\"focused\":true,"
      "\"nodes\":[{\"id\":5,\"type\":\"con\",\"focused\":true,"
      "\"nodes\":[{\"id\":6,\"type\":\"con\",\"focused\":false,\"nodes\":[]}]}]}"
      "]}");
  CHECK(find_focused(tree, nullptr, &ws_out) == nullptr);
  json_object_put(tree);
}

/* ══════════════ workspace-события ══════════════ */

static void test_handle_ws_event(void) {
  reset_runtime();

  json_object *ev = json("{\"change\":\"focus\",\"current\":{\"id\":4}}");
  handle_ws_event(-1, ev);
  CHECK(g_focus_ws == 4);
  json_object_put(ev);

  /* тот же стол — память окна сохраняется */
  g_focus_win = 99;
  ev = json("{\"change\":\"focus\",\"current\":{\"id\":4}}");
  handle_ws_event(-1, ev);
  CHECK(g_focus_ws == 4);
  CHECK(g_focus_win == 99);
  json_object_put(ev);

  /* смена стола гасит память окна */
  ev = json("{\"change\":\"focus\",\"current\":{\"id\":5}}");
  handle_ws_event(-1, ev);
  CHECK(g_focus_ws == 5);
  CHECK(g_focus_win == 0);
  json_object_put(ev);

  /* не-focus и пустой current игнорируются */
  g_focus_ws = 5;
  g_focus_win = 77;
  ev = json("{\"change\":\"rename\",\"current\":{\"id\":9}}");
  handle_ws_event(-1, ev);
  CHECK(g_focus_ws == 5 && g_focus_win == 77);
  json_object_put(ev);
  ev = json("{\"change\":\"focus\"}");
  handle_ws_event(-1, ev);
  CHECK(g_focus_ws == 5 && g_focus_win == 77);
  json_object_put(ev);
  handle_ws_event(-1, nullptr);
  CHECK(g_focus_ws == 5 && g_focus_win == 77);
}

/* ══════════════ IPC-фрейминг ══════════════ */

static void test_ipc(void) {
  int sv[2] = {-1, -1};
  CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
  if (sv[0] < 0)
    return;

  CHECK(ipc_send(sv[0], RUN_COMMAND, "hello", 5));
  uint32_t type = 0;
  size_t len = 0;
  uint8_t *buf = ipc_recv(sv[1], &type, &len);
  CHECK(buf != nullptr);
  CHECK(type == RUN_COMMAND);
  CHECK(len == 5);
  if (buf != nullptr) {
    CHECK(memcmp(buf, "hello", 5) == 0);
    CHECK(buf[5] == '\0');
    free(buf);
  }

  /* пустой payload — только заголовок; recv отдаёт пустую строку */
  CHECK(ipc_send(sv[0], GET_TREE, nullptr, 0));
  buf = ipc_recv(sv[1], &type, &len);
  CHECK(buf != nullptr);
  CHECK(type == GET_TREE);
  CHECK(len == 0);
  if (buf != nullptr) {
    CHECK(buf[0] == '\0');
    free(buf);
  }

  /* out_len == nullptr допустим */
  CHECK(ipc_send(sv[0], SUBSCRIBE, "[]", 2));
  buf = ipc_recv(sv[1], &type, nullptr);
  CHECK(buf != nullptr && type == SUBSCRIBE);
  free(buf);

  /* битая магия — nullptr */
  uint8_t bad[14] = {'B', 'A', 'D', 'I', 'P', 'C', 0, 0, 0, 0, 0, 0, 0, 0};
  CHECK(write_all(sv[0], bad, sizeof bad));
  CHECK(ipc_recv(sv[1], &type, &len) == nullptr);

  /* payload длиннее IPC_MAX_LEN дренируется; при обрыве — nullptr */
  uint32_t huge = IPC_MAX_LEN + 1;
  uint8_t head[14] = {'i', '3', '-', 'i', 'p', 'c'};
  memcpy(head + 6, &huge, 4);
  uint32_t zero = 0;
  memcpy(head + 10, &zero, 4);
  CHECK(write_all(sv[0], head, sizeof head));
  close(sv[0]);
  sv[0] = -1;
  CHECK(ipc_recv(sv[1], &type, &len) == nullptr);

  /* слишком длинный payload не отправляется */
  CHECK(!ipc_send(sv[1], RUN_COMMAND, "", (size_t)UINT32_MAX + 1));

  close(sv[1]);
}

static void test_posix_io(void) {
  int sv[2] = {-1, -1};
  CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
  if (sv[0] < 0)
    return;

  struct iovec iov[2] = {
      {.iov_base = (void *)"abc", .iov_len = 3},
      {.iov_base = (void *)"def", .iov_len = 3},
  };
  CHECK(writev_all(sv[0], iov, 2));
  char out[8] = {0};
  CHECK(read_all(sv[1], out, 6));
  CHECK(memcmp(out, "abcdef", 6) == 0);

  /* EOF после закрытия пишущей стороны */
  CHECK(write_all(sv[0], "z", 1));
  close(sv[0]);
  sv[0] = -1;
  CHECK(read_all(sv[1], out, 1));
  CHECK(out[0] == 'z');
  CHECK(!read_all(sv[1], out, 1));

  /* нулевая длина — успех без syscall */
  CHECK(write_all(sv[1], "", 0));
  CHECK(read_all(sv[1], out, 0));

  close(sv[1]);
}

/* ══════════════ now_ms ══════════════ */

static void test_now_ms(void) {
  int64_t a = now_ms();
  int64_t b = now_ms();
  CHECK(a > 0);
  CHECK(b >= a);
}

/* ══════════════ main ══════════════ */

int main(void) {
  test_ws_guard();
  test_ws_focus_cmd();
  test_json_helpers();
  test_is_tiled();
  test_collect();
  test_proportions();
  test_master_out_of_slot();
  test_dead_master_ws();
  test_snapshots();
  test_echo();
  test_cmd_builders();
  test_parse_json_exact();
  test_output_right_of();
  test_is_marked_daemon();
  test_find_focused();
  test_handle_ws_event();
  test_ipc();
  test_posix_io();
  test_now_ms();

  printf("unit: %d проверок, %d провалов\n", g_checks, g_failures);
  return g_failures == 0 ? 0 : 1;
}
