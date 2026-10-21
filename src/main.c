/* main.c — DWM-style master/stack tiling daemon для Sway (Sway IPC).
 *
 * Инвариант воркспейса — ровно две зоны:
 *     splith { мастер _m<ws> (M%) | splitv-стопка _s<ws> }
 * Доля мастера выбирается ЗАРАНЕЕ, при запуске: `sms [N]`, N = 10..90 —
 * доля мастера, % (например `sms 60` или `sms 50`); env SMS_PCT — то же
 * самое, аргумент сильнее; дефолт 55/45.
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
 *      ПОЛУЧАЕТ ФОКУС (dwm unmanage → focus(NULL)); слепой пачкой
 *      ТОЛЬКО по свежему снимку (гвард workspace в каждой команде).
 *      Мастер был один — фокус остаётся на опустевшем столе: демон
 *      не даёт увести его на другой выход, новое окно родится здесь.
 *   3. СЫРОЙ move left НА ОКНЕ СТОПКИ (бинды юзера) — ЗУМ: ОДИН свап
 *      с мастером — окно наследует место и геометрию, а бывший
 *      мастер встаёт ТОЧНО на слот зумированного (по снимку порядка;
 *      зум вершины и так точен). Память _p на снятом мастере.
 *   4. СЫРОЙ move right НА МАСТЕРЕ: есть память _p — ТОЧНЫЙ ВОЗВРАТ
 *      (партнёр выходит в мастер, снятый мастер — на его слот);
 *      памяти нет — РОТАЦИЯ с вершиной (снятый — вершиной; ротация
 *      ставит память — повторение возвращает всё). Тумблер всегда.
 *   5. Стопка — ОДИН splitv-контейнер; блудные окна съезжают к _s.
 *      move up/down внутри стопки — нативная перестановка (0 команд).
 *      «Крайние» движения (верх-вверх / низ-вниз) ломают ws в
 *      splitv — демон чинит за ОДИН раунд реструктуризацией
 *      «move left» (окно — первый ребёнок ws, ws=splith).
 *
 * СНИМОК СТОПКИ: после каждого здорового ретайла (0 команд) демон
 * запоминает порядок стола (мастер + стопка сверху вниз + имя ws).
 * Любая команда снимок гасит (верным его вернёт контрольный
 * reconcile). Снимок — ТОЛЬКО ускоритель: нет его — честный путь с
 * get_tree, поведение то же, но на кадр медленнее.
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
 * кого двигал сам, и глотает эти события БЕЗ пересчёта. После пачки
 * с move-командами — контрольный reconcile.
 *
 * Плавающие: при рождении/уходе — в центр; их move-события не читаем
 * (плавающие двигает сам sway — нативно).
 *
 * build: cc -std=c23 -Wall -Wextra -Wpedantic -Wconversion -Wshadow \
 *   -Wformat=2 -Werror=format-security -O2 main.c -ljson-c -o sms
 */
#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <inttypes.h>
#include <signal.h>
#include <stdbool.h>
#include <stdckdint.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <json-c/json.h>

/* ══════════════ ТЮНИНГ ══════════════ */
static int MASTER_PCT = 55;        /* доля мастера, %; env SMS_PCT = 10..90 */
constexpr int PCT_TOLERANCE = 3;
constexpr int TILED_MAX = 64;

constexpr uint32_t IPC_MAX_LEN = 16u * 1024u * 1024u;
constexpr uint32_t EV_WORKSPACE = 0x80000000u | 0u; /* focus → трекинг стола */
constexpr uint32_t EV_WINDOW = 0x80000000u | 3u;

enum { RUN_COMMAND = 0, SUBSCRIBE = 2, GET_TREE = 4 };

/* ── POSIX I/O: полные циклы, EINTR ── */

[[nodiscard]] static bool write_all(int fd, const void *buf, size_t n) {
  const char *p = buf;
  while (n > 0) {
    ssize_t k = write(fd, p, n);
    if (k < 0) {
      if (errno == EINTR)
        continue;
      return false;
    }
    if (k == 0)
      return false;
    p += (size_t)k;
    n -= (size_t)k;
  }
  return true;
}

[[nodiscard]] static bool read_all(int fd, void *buf, size_t n)
{
    char *p = buf;
    while (n > 0) {
        ssize_t k = read(fd, p, n);
        if (k < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        if (k == 0) return false; /* EOF: sway ушёл */
        p += (size_t)k;
        n -= (size_t)k;
    }
    return true;
}

/* ── Sway IPC фрейм: "i3-ipc" + u32 len + u32 type, host endian ── */

[[nodiscard]] static bool ipc_send(int fd, uint32_t type,
                                   const char *payload, size_t len)
{
    uint32_t payload_len = (uint32_t)len;
    uint32_t t = type;
    uint8_t h[14] = { 'i', '3', '-', 'i', 'p', 'c' };
    memcpy(h + 6, &payload_len, 4);
    memcpy(h + 10, &t, 4);
    if (!write_all(fd, h, 14)) return false;
    return len == 0 || write_all(fd, payload, len);
}

/* NUL-терминированный malloc-буфер; при oversize payload ДРЕНИРУЕТСЯ,
 * чтобы сокет не рассинхронизировался. */
[[nodiscard]] static uint8_t *ipc_recv(int fd, uint32_t *type)
{
    uint8_t h[14];
    uint32_t len, t;
    if (!read_all(fd, h, 14)) return nullptr;
    memcpy(&len, h + 6, 4);
    memcpy(&t, h + 10, 4);
    *type = t;
    if (memcmp(h, "i3-ipc", 6) != 0) return nullptr;
    if (len > IPC_MAX_LEN) {
        uint8_t sink[4096];
        while (len > 0) {
            size_t chunk = len < sizeof sink ? len : sizeof sink;
            if (!read_all(fd, sink, chunk)) return nullptr;
            len -= (uint32_t)chunk;
        }
        return nullptr;
    }
    size_t total;
    if (ckd_add(&total, (size_t)len, (size_t)1)) return nullptr;
    uint8_t *buf = malloc(total);
    if (buf == nullptr) return nullptr;
    if (len > 0 && !read_all(fd, buf, len)) {
        free(buf);
        return nullptr;
    }
    buf[len] = '\0';
    return buf;
}

/* ── NULL-безопасные доступоры (заимствованные ссылки) ── */

static json_object *jget(json_object *o, const char *key)
{
    json_object *v = nullptr;
    if (o != nullptr && json_object_is_type(o, json_type_object)
        && json_object_object_get_ex(o, key, &v))
        return v;
    return nullptr;
}

static json_object *jarr(json_object *o, const char *key)
{
    json_object *v = jget(o, key);
    return (v != nullptr && json_object_is_type(v, json_type_array)) ? v : nullptr;
}

static const char *jstr(json_object *o, const char *key)
{
    json_object *v = jget(o, key);
    return v != nullptr ? json_object_get_string(v) : nullptr;
}

static int64_t jint(json_object *o, const char *key)
{
    json_object *v = jget(o, key);
    return v != nullptr ? (int64_t)json_object_get_int64(v) : -1;
}

static bool jbool(json_object *o, const char *key)
{
    json_object *v = jget(o, key);
    return v != nullptr && json_object_get_boolean(v);
}

static bool layout_is(json_object *n, const char *want)
{
    const char *l = jstr(n, "layout");
    return l != nullptr && strcmp(l, want) == 0;
}

static bool is_floating(json_object *n)
{
    const char *f = jstr(n, "floating");
    return f != nullptr
        && (strcmp(f, "auto_on") == 0 || strcmp(f, "user_on") == 0);
}

static bool is_tiled(json_object *n)
{
    json_object *nodes = jarr(n, "nodes");
    if (nodes != nullptr && json_object_array_length(nodes) > 0)
        return false;
    const char *type = jstr(n, "type");
    if (type == nullptr || strcmp(type, "con") != 0) return false;
    if (is_floating(n)) return false;
    const char *scratch = jstr(n, "scratchpad_state");
    return scratch == nullptr || strcmp(scratch, "none") == 0;
}

static bool has_mark(json_object *n, const char *mark)
{
    json_object *marks = jarr(n, "marks");
    if (marks == nullptr) return false;
    size_t cnt = json_object_array_length(marks);
    for (size_t i = 0; i < cnt; i++) {
        json_object *m = json_object_array_get_idx(marks, i);
        const char *s = m != nullptr ? json_object_get_string(m) : nullptr;
        if (s != nullptr && strcmp(s, mark) == 0) return true;
    }
    return false;
}

/* ws, на котором умер держатель _m — из marks закрытого окна; 0 = нет */
static int64_t dead_master_ws(json_object *con)
{
    json_object *marks = jarr(con, "marks");
    if (marks == nullptr) return 0;
    size_t cnt = json_object_array_length(marks);
    for (size_t i = 0; i < cnt; i++) {
        json_object *m = json_object_array_get_idx(marks, i);
        const char *s = m != nullptr ? json_object_get_string(m) : nullptr;
        if (s != nullptr && s[0] == '_' && s[1] == 'm') {
            int64_t v = strtoll(s + 2, nullptr, 10);
            if (v > 0) return v;
        }
    }
    return 0;
}

/* ── Сбор тайлов: один DFS, порядок дерева (= визуальный порядок).
   Массивы — СЛОТЫ: id/parent/rw[i] описывают слот i, а не окно. ── */

typedef struct {
    int n;                          /* тайловых окон */
    int64_t id[TILED_MAX];
    json_object *parent[TILED_MAX]; /* родитель слота (жив, пока живо дерево) */
    int64_t rw[TILED_MAX];          /* rect.width слота */
    int64_t ws_w;                   /* rect.width воркспейса */
    int64_t master_id;              /* держатель _m<ws>, -1 = нет */
    int64_t stack_id;               /* держатель _s<ws>, -1 = нет */
    int64_t partner_id;             /* держатель _p<ws> (место возврата), -1 = нет */
    bool focused_here;              /* внутри ws есть сфокусированный узел */
} Tiled;

static void collect(json_object *n, json_object *parent,
                    const char *mk_m, const char *mk_s,
                    const char *mk_p, Tiled *t)
{
    int64_t id = jint(n, "id");
    if (!t->focused_here && jbool(n, "focused"))
        t->focused_here = true;
    if (t->master_id == -1 && has_mark(n, mk_m))
        t->master_id = id;
    if (t->stack_id == -1 && has_mark(n, mk_s))
        t->stack_id = id;
    if (t->partner_id == -1 && has_mark(n, mk_p))
        t->partner_id = id;
    if (is_tiled(n) && t->n < TILED_MAX) {
        t->id[t->n] = id;
        t->parent[t->n] = parent;
        json_object *r = jget(n, "rect");
        t->rw[t->n] = (r != nullptr) ? jint(r, "width") : 0;
        t->n++;
    }
    json_object *nodes = jarr(n, "nodes"); /* floating_nodes не считаем */
    if (nodes == nullptr) return;
    size_t cnt = json_object_array_length(nodes);
    for (size_t i = 0; i < cnt; i++)
        collect(json_object_array_get_idx(nodes, i), n, mk_m, mk_s, mk_p, t);
}

/* индекс окна среди тайлов; -1 = его здесь нет */
static int idx_of(const Tiled *t, int64_t id)
{
    if (id <= 0) return -1;
    for (int i = 0; i < t->n; i++)
        if (t->id[i] == id) return i;
    return -1;
}

/* ── Кэш эха: окна, которые ДВИГАЛИ наши команды. Их window::move —
   наше собственное эхо (одна move-команда = один window::move,
   замерено; swap/mark/resize эха не дают) — глотаем без пересчёта.
   Провалившаяся команда не эхает: застрявший id может ошибочно
   проглотить ОДИН будущий ход юзера — стол долечит следующий эвент. ── */

static int64_t echo_ids[TILED_MAX];
static int echo_n = 0;

static void echo_track(int64_t id)
{
    if (id <= 0 || echo_n >= TILED_MAX) return;
    echo_ids[echo_n++] = id;
}

static bool echo_consume(int64_t id)
{
    for (int i = 0; i < echo_n; i++)
        if (echo_ids[i] == id) {
            echo_ids[i] = echo_ids[--echo_n];
            return true;
        }
    return false;
}

/* ── Команды: пачка через «;», критерии перед КАЖДОЙ командой ── */

static void cmd_append(char *buf, size_t cap, size_t *off, const char *cmd)
{
    size_t len = strlen(cmd);
    if (*off + len + 3 >= cap) return;
    if (*off > 0) { buf[(*off)++] = ';'; buf[(*off)++] = ' '; }
    memcpy(buf + *off, cmd, len);
    *off += len;
    buf[*off] = '\0';
}

/* move-команда: пачка + запоминание эха */
static void cmd_move(char *buf, size_t cap, size_t *off, const char *cmd,
                     int64_t id)
{
    cmd_append(buf, cap, off, cmd);
    echo_track(id);
}

static void send_cmd(int fd, const char *cmd)
{
    if (getenv("SMS_DEBUG") != nullptr) fprintf(stderr, "cmd: %s\n", cmd);
    if (!ipc_send(fd, RUN_COMMAND, cmd, strlen(cmd))) return;
    uint32_t type;
    free(ipc_recv(fd, &type)); /* ответ вычитываем молча, чтобы не рассинхрониться */
}

[[nodiscard]] static json_object *get_tree(int fd)
{
    for (int attempt = 0; attempt < 2; attempt++) {
        uint32_t type;
        if (!ipc_send(fd, GET_TREE, nullptr, 0)) return nullptr;
        uint8_t *buf = ipc_recv(fd, &type);
        if (buf == nullptr) return nullptr;
        if (getenv("SMS_DEBUG") != nullptr) fprintf(stderr, "tree: read\n");
        json_object *tree = json_tokener_parse((const char *)buf);
        free(buf);
        if (tree != nullptr) return tree;
        /* пустой/битый ответ — одна повторная попытка */
    }
    return nullptr;
}

/* ── Пропорции: мастер ≈ MASTER_PCT% ширины воркспейса? ── */

[[nodiscard]] static bool proportions_ok(const Tiled *t)
{
    if (t->n < 2 || t->rw[0] <= 0 || t->ws_w <= 0) return true;
    int pct = (int)((t->rw[0] * 100) / t->ws_w);
    return pct >= MASTER_PCT - PCT_TOLERANCE
        && pct <= MASTER_PCT + PCT_TOLERANCE;
}

/* ── Действие раунда (из события) ── */

typedef struct {
    int64_t promote;   /* новое окно → мастер; 0 = нет */
    int64_t moved;     /* окно от window::move; 0 = нет */
    int64_t died_ws;   /* ws, где умер держатель _m; 0 = нет */
} Act;

static const Act ACT_NONE = { .promote = 0, .moved = 0, .died_ws = 0 };

/* ── СНИМОК СТОЛА: последний ЗДОРОВЫЙ порядок (0 команд = снимок
   верен; любая команда — гасит, вернёт контрольный reconcile).
   Читают: слепой спавн (мастер/стопка), слепая смерть (верхушка),
   точный зум (порядок стопки). Это ускоритель, не истина: нет
   снимка — честный путь с get_tree. ── */

#define WS_SNAP 16
typedef struct {
    int64_t ws;                     /* id воркспейса; 0 = слот свободен */
    char name[64];                  /* имя ws для гварда workspace="…" */
    int64_t master;                 /* мастер (для слепого спавна) */
    int n;                          /* окон в стопке */
    int64_t stack[TILED_MAX];       /* порядок стопки сверху вниз */
    uint64_t tick;                  /* LRU по ЧТЕНИЯМ */
} Snap;

static Snap g_snap[WS_SNAP];
static uint64_t g_snap_tick = 1;
static int64_t g_focus_ws = 0;      /* ws сфокусированного окна */

static Snap *snap_get(int64_t ws)
{
    if (ws <= 0) return nullptr;
    for (int i = 0; i < WS_SNAP; i++)
        if (g_snap[i].ws == ws) {
            g_snap[i].tick = ++g_snap_tick;
            return &g_snap[i];
        }
    return nullptr;
}

static void snap_evict(int64_t ws)
{
    if (ws <= 0) return;
    for (int i = 0; i < WS_SNAP; i++)
        if (g_snap[i].ws == ws) {
            g_snap[i].ws = 0;
            return;
        }
}

static void snap_store(int64_t ws, const char *name, int64_t master,
                       const int64_t *stack, int n)
{
    if (ws <= 0 || n < 0 || n > TILED_MAX) return;
    Snap *s = nullptr;
    for (int i = 0; i < WS_SNAP; i++)
        if (g_snap[i].ws == ws) { s = &g_snap[i]; break; }
    if (s == nullptr) {
        s = &g_snap[0];              /* свободный слот или LRU-жертва */
        for (int i = 1; i < WS_SNAP; i++) {
            if (g_snap[i].ws == 0) { s = &g_snap[i]; break; }
            if (g_snap[i].tick < s->tick) s = &g_snap[i];
        }
        s->tick = ++g_snap_tick;
    }
    s->ws = ws;
    snprintf(s->name, sizeof s->name, "%s",
             (name != nullptr && name[0] != '\0') ? name : "");
    s->master = master;
    s->n = n;
    if (n > 0) memcpy(s->stack, stack, (size_t)n * sizeof stack[0]);
}

/* ── Retile: канонический ребилд одного воркспейса.
 * Возвращает: отправлялись ли команды вообще (стол был болен).
 * *moves_out |= в пачке были MOVE-команды (породят эхо + нужен
 * контрольный reconcile). ── */

static bool retile_ws(int fd, json_object *ws, const Act *act, bool *moves_out)
{
    int64_t wid = jint(ws, "id");
    if (wid <= 0) return false;
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
    if (t.n == 0) {                 /* пустой стол — не трогаем */
        snap_evict(wid);
        /* мастер умер последним — фокус ОСТАЁТСЯ на этом столе:
           некоторые сборки/конфиги sway уводят его на другой выход,
           а юзер ждёт следующее окно здесь */
        if (act->died_ws == wid && g_focus_ws == wid) {
            const char *nm = jstr(ws, "name");
            if (nm != nullptr && nm[0] != '\0') {
                char keep[128];
                snprintf(keep, sizeof keep,
                         "[workspace=\"^%s$\"] focus", nm);
                send_cmd(fd, keep);
                return true;
            }
        }
        return false;
    }
    if (getenv("SMS_DEBUG") != nullptr)
        fprintf(stderr, "retile: ws=%" PRId64 " n=%d p0ws=%d splith=%d "
                "m=%" PRId64 " mid=%" PRId64 " sid=%" PRId64 "\n",
                wid, t.n, t.parent[0] == ws, layout_is(ws, "splith"),
                t.id[0], t.master_id, t.stack_id);

    char buf[8192], one[192];
    size_t off = 0;
    buf[0] = '\0';

    int pi = idx_of(&t, act->moved);
    int64_t displaced = -1;          /* снятый мастер → вершина стопки */
    bool zoomed = false;             /* снятие мастера — зум (не спавн) */

    /* ═══ ЯД «КРАЙНИХ» ДВИЖЕНИЙ (move up на верхнем / move down на
       нижнем): окно вырвалось на уровень ws, а ws перевернулся в
       splitv. Лечение — реструктуризация «[окно] move left»: окно
       встаёт ПЕРВЫМ ребёнком, ws=splith, лишние обёртки sway
       растворяет сам (замерено). НИЖНЕЕ (последний тайл): старый
       мастер — на ДНО стопки (точный обмен) — вгоняем его туда ДО
       реструктуризации, переметив _s на текущий низ. Снимок после
       реструктуризации недостоверен — остаток доделает reconcile. ═══ */
    if (pi >= 0 && act->moved > 0 && act->moved != t.master_id
        && t.parent[pi] == ws && !layout_is(ws, "splith")) {
        if (getenv("SMS_DEBUG") != nullptr)
            fprintf(stderr, "яд: pi=%d n=%d mid=%" PRId64 "\n",
                    pi, t.n, t.master_id);
        if (pi == t.n - 1 && t.master_id > 0) {
            json_object *sc = nullptr;   /* контейнер стопки */
            for (int i = t.n - 1; i >= 1; i--)
                if (i != pi && t.parent[i] != ws) { sc = t.parent[i]; break; }
            int sn = -1;                 /* низ стопки */
            if (sc != nullptr)
                for (int i = t.n - 1; i >= 1; i--)
                    if (i != pi && t.parent[i] == sc) { sn = i; break; }
            int mi = idx_of(&t, t.master_id);
            if (sn > 0 && mi >= 0 && t.parent[mi] != sc) {
                if (getenv("SMS_DEBUG") != nullptr)
                    fprintf(stderr, "яд-T2: сн=%" PRId64 " мастер=%" PRId64 " на дно\n",
                            t.id[sn], t.id[mi]);
                snprintf(one, sizeof one,
                         "[con_id=%" PRId64 "] mark --replace %s",
                         t.id[sn], mk_s);
                cmd_append(buf, sizeof buf, &off, one);
                snprintf(one, sizeof one,
                         "[con_id=%" PRId64 "] move container to mark %s",
                         t.id[mi], mk_s);
                cmd_move(buf, sizeof buf, &off, one, t.id[mi]);
                *moves_out = true;
            }
        }
        snprintf(one, sizeof one,
                 "[con_id=%" PRId64 "] move left", t.id[pi]);
        cmd_move(buf, sizeof buf, &off, one, t.id[pi]);
        *moves_out = true;
        goto finish;
    }

    /* ═══ МАСТЕР УШЁЛ В СТОПКУ (сырой move right на мастере): Sway
       вбрасывает его ВНУТРЬ стопки — по геометрии, серединой, а не
       вершиной (замерено: 3-окна → слот 1). Есть память _p — ТОЧНЫЙ
       ВОЗВРАТ: свап с партнёром обменивает их слоты (партнёр сидит
       ровно на прежнем слоте мастера!), затем партнёр выходит в
       мастер — снятый остаётся на СВОЁМ месте, с геометрией слота.
       Памяти нет — РОТАЦИЯ с вершиной: свап с вершиной + её побег;
       снятый — вершиной; ротация ставит память — повторный move
       right возвращает всё. Побег сбрасывает ширину — resize в пачке. ═══ */
    if (pi >= 0 && act->moved > 0 && act->moved == t.master_id
        && t.n >= 2 && t.parent[pi] != ws) {
        int ki = idx_of(&t, t.partner_id);
        if (ki < 0 || ki == pi || t.partner_id == t.master_id
            || t.parent[ki] != t.parent[pi])
            ki = -1;                 /* памяти нет/чужая — ротация */
        int64_t who = 0;             /* кто выходит в мастер */
        bool rot = false;            /* ротация (не возврат) */
        bool top_mark = false;       /* снятому мастеру — якорь _s */
        if (ki >= 0) {
            who = t.partner_id;      /* точный возврат */
            /* партнёр был якорем (вершиной) до вставки мастера? */
            top_mark = (pi > 0) ? (ki == 0) : (ki == 1);
        } else {
            rot = true;
            if (pi == 0) {
                if (t.parent[1] == t.parent[0])
                    who = t.id[1];   /* окно под мастером (бывшая вершина) */
            } else {
                if (t.parent[0] == t.parent[pi])
                    who = t.id[0];   /* вершина */
                top_mark = true;     /* снятый встанет вершиной */
            }
        }
        if (who > 0) {
            if (getenv("SMS_DEBUG") != nullptr)
                fprintf(stderr, "демо/возврат: pi=%d ki=%d who=%" PRId64 "%s\n",
                        pi, ki, who, rot ? " ротация" : " возврат");
            if (rot && pi > 0) {
                /* свап с вершиной: снятый мастер — вершиной (место И
                   геометрия) ещё ДО побега вершины */
                snprintf(one, sizeof one,
                         "[con_id=%" PRId64 "] swap container with con_id %" PRId64,
                         t.id[pi], who);
                cmd_append(buf, sizeof buf, &off, one);
            } else if (!rot) {
                /* свап с партнёром: снятый мастер — на его слот */
                snprintf(one, sizeof one,
                         "[con_id=%" PRId64 "] swap container with con_id %" PRId64,
                         t.id[pi], who);
                cmd_append(buf, sizeof buf, &off, one);
            }
            snprintf(one, sizeof one,
                     "[con_id=%" PRId64 "] move left", who);
            cmd_move(buf, sizeof buf, &off, one, who);
            *moves_out = true;
            snprintf(one, sizeof one,
                     "[con_id=%" PRId64 "] mark --replace %s",
                     who, mk_m);     /* --replace срезает _p партнёра */
            cmd_append(buf, sizeof buf, &off, one);
            if (rot) {               /* ротация ставит память */
                snprintf(one, sizeof one,
                         "[con_id=%" PRId64 "] mark --add %s",
                         t.id[pi], mk_p);
                cmd_append(buf, sizeof buf, &off, one);
            }
            if (top_mark) {          /* снятый мастер — вершина: якорь */
                snprintf(one, sizeof one,
                         "[con_id=%" PRId64 "] mark --add %s",
                         t.id[pi], mk_s);
                cmd_append(buf, sizeof buf, &off, one);
            }
            snprintf(one, sizeof one,
                     "[con_id=%" PRId64 "] resize set width %d ppt",
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
    if (pi > 0 && act->moved > 0 && act->moved != t.master_id
        && t.parent[pi] == ws && t.master_id > 0
        && t.master_id == t.id[0]) {
        Snap *sn = snap_get(wid);
        int j = -1;
        if (sn != nullptr)
            for (int i = 0; i < sn->n; i++)
                if (sn->stack[i] == act->moved) { j = i; break; }
        if (j >= 1) {
            int wi = idx_of(&t, sn->stack[j - 1]);   /* окно над слотом */
            if (wi > 0 && t.parent[wi] != ws) {
                char mk_a[24];
                snprintf(mk_a, sizeof mk_a, "_a%" PRId64, wid);
                if (getenv("SMS_DEBUG") != nullptr)
                    fprintf(stderr, "точный зум: j=%d над=%" PRId64 "\n",
                            j, sn->stack[j - 1]);
                snprintf(one, sizeof one,
                         "[con_id=%" PRId64 "] swap container with mark %s",
                         t.id[pi], mk_m);
                cmd_append(buf, sizeof buf, &off, one);
                snprintf(one, sizeof one,
                         "[con_id=%" PRId64 "] mark --replace %s",
                         t.id[pi], mk_m);
                cmd_append(buf, sizeof buf, &off, one);
                snprintf(one, sizeof one,
                         "[con_id=%" PRId64 "] mark --add %s",
                         t.master_id, mk_p);   /* память возврата */
                cmd_append(buf, sizeof buf, &off, one);
                snprintf(one, sizeof one,
                         "[con_id=%" PRId64 "] mark --add %s",
                         sn->stack[j - 1], mk_a);
                cmd_append(buf, sizeof buf, &off, one);
                snprintf(one, sizeof one,
                         "[con_id=%" PRId64 "] move container to mark %s",
                         t.master_id, mk_a);
                cmd_move(buf, sizeof buf, &off, one, t.master_id);
                *moves_out = true;
                snprintf(one, sizeof one, "unmark %s", mk_a);
                cmd_append(buf, sizeof buf, &off, one);
                goto finish;
            }
        }
    }

    /* ═══ КАНДИДАТ: зум (вырвался на уровень ws) или новое окно.
       Живой держатель _m — ОДИН свап: обмен местами и геометрией,
       бывший мастер встаёт ТОЧНО на место кандидата. ═══ */
    int cand = -1;
    if (pi > 0 && act->moved != t.master_id && t.parent[pi] == ws)
        cand = pi;                    /* зум: move left вырвал на уровень ws */
    if (cand < 0 && act->promote > 0) {
        int ni = idx_of(&t, act->promote);
        if (ni > 0) cand = ni;        /* новое окно — мастер (dwm attach) */
    }
    if (cand > 0 && t.master_id > 0) {
        displaced = t.id[0];         /* прежний мастер уходит под свап */
        zoomed = (cand == pi);       /* снятие зумом, не спавном */
        snprintf(one, sizeof one,
                 "[con_id=%" PRId64 "] swap container with mark %s",
                 t.id[cand], mk_m);
        cmd_append(buf, sizeof buf, &off, one);
        int64_t tid = t.id[cand]; t.id[cand] = t.id[0]; t.id[0] = tid;
        /* parent[] не трогаем: слоты позиционны, свап меняет окна,
           но не родителей слотов (обмен был бы багом: порождал ложный
           «побег» и висячее эхо от no-op move) */
    }

    /* ═══ ПОБЕГ ИЗ ПЛЕНА: мастер не на уровне ws — «move left».
       Встаёт ПЕРВЫМ ребёнком ws, ws=splith, контейнер стопки жив.
       ТОЛЬКО при n>1 (обёртка мульти-ребёнок — побег НАВЕРХ, свёрено
       с sway: ветка promote). Синглтон (последнее окно в обёртке
       или на столе) — НИКОГДА: move left на нём в новых sway —
       ВАРП НА СОСЕДНИЙ ВЫХОД (i3-семантика «singleton children as if
       at workspace level»); выжившее после смерти мастера остаётся
       на месте — визуально тот же полный стол, структуру достроит
       следующее событие.
       Табbed/stacked-тюрьма: move left там — переключение ТАБОВ
       (no-op без эха!) — сначала превращаем её в splitv (layout по
       листу целит в родителя), потом побег. ═══ */
    bool escaped = false;
    if (t.parent[0] != ws && t.n > 1) {
        const char *pl = jstr(t.parent[0], "layout");
        if (pl != nullptr
            && (strcmp(pl, "tabbed") == 0 || strcmp(pl, "stacked") == 0)) {
            snprintf(one, sizeof one,
                     "[con_id=%" PRId64 "] layout splitv", t.id[0]);
            cmd_append(buf, sizeof buf, &off, one);
        }
        snprintf(one, sizeof one,
                 "[con_id=%" PRId64 "] move left", t.id[0]);
        cmd_move(buf, sizeof buf, &off, one, t.id[0]);
        t.parent[0] = ws;
        escaped = true;
        *moves_out = true;
    } else if (t.parent[0] == ws && !layout_is(ws, "splith")) {
        /* ═══ РЕСТРУКТУРИЗАЦИЯ: мастер УЖЕ на уровне ws, но ws не
           splith (пережиток чужого layout/яда без movers) — тот же
           «move left»: встанет первым, ws=splith. Снимок дальше
           недостоверен — финиш, доделает reconcile. ═══ */
        snprintf(one, sizeof one,
                 "[con_id=%" PRId64 "] move left", t.id[0]);
        cmd_move(buf, sizeof buf, &off, one, t.id[0]);
        *moves_out = true;
        goto finish;
    }

    int64_t m = t.id[0];
    if (t.master_id != m) {
        snprintf(one, sizeof one,
                 "[con_id=%" PRId64 "] mark --replace %s", m, mk_m);
        cmd_append(buf, sizeof buf, &off, one);
        /* сменился мастер (НЕ нашим зумом/демо) — память возврата _p
           недействительна: место держит окно, занявшее место ДРУГОГО */
        if (t.partner_id > 0) {
            snprintf(one, sizeof one,
                     "[con_id=%" PRId64 "] unmark %s", t.partner_id, mk_p);
            cmd_append(buf, sizeof buf, &off, one);
        }
    } else if (act->moved > 0 && act->moved == t.partner_id
               && act->moved != t.master_id) {
        /* партнёр памяти уехал своими биндами — точный возврат
           стал бы ложным, память гасим (документированная семантика) */
        snprintf(one, sizeof one, "unmark %s", mk_p);
        cmd_append(buf, sizeof buf, &off, one);
    }

    /* одно окно: структуру не трогаем; следующее событие достроит */
    if (t.n == 1)
        goto finish;

    /* ═══ СТОПКА: якорь — первый не-мастер, УЖЕ живущий в контейнере;
       если все плоско (рождение/частокол) — tiled[1] ═══ */
    int ai = 1;
    for (int i = 1; i < t.n; i++)
        if (t.parent[i] != ws) { ai = i; break; }
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
        snprintf(one, sizeof one,
                 "[con_id=%" PRId64 "] mark --replace %s", a, mk_s);
        cmd_append(buf, sizeof buf, &off, one);
    }
    if (flat) {
        /* обернуть якорь в свежий splitv — контейнер стопки */
        snprintf(one, sizeof one,
                 "[con_id=%" PRId64 "] split vertical", a);
        cmd_append(buf, sizeof buf, &off, one);
    } else if (!layout_is(ac, "splitv")) {
        /* лист ИЗНУТРИ стопки → цель = контейнер стопки */
        snprintf(one, sizeof one,
                 "[con_id=%" PRId64 "] layout splitv", a);
        cmd_append(buf, sizeof buf, &off, one);
    }

    /* сбор: все не-мастера вне контейнера якоря едут к метке _s —
       вставка следующей за якорем = верх стопки. Обратный порядок
       сохраняет визуальный порядок стека. */
    for (int i = t.n - 1; i >= 1; i--) {
        if (i == ai) continue;
        if (t.id[i] == displaced) continue;
        if (!flat && t.parent[i] == ac) continue;
        snprintf(one, sizeof one,
                 "[con_id=%" PRId64 "] move container to mark %s",
                 t.id[i], mk_s);
        cmd_move(buf, sizeof buf, &off, one, t.id[i]);
        *moves_out = true;
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
                         "[con_id=%" PRId64 "] move container to mark %s",
                         displaced, mk_s);
                cmd_move(buf, sizeof buf, &off, one, displaced);
                *moves_out = true;
            }
            snprintf(one, sizeof one,
                     "[con_id=%" PRId64 "] swap container with mark %s",
                     displaced, mk_s);
            cmd_append(buf, sizeof buf, &off, one);
            if (zoomed) {
                snprintf(one, sizeof one,
                         "[con_id=%" PRId64 "] mark --add %s",
                         displaced, mk_p);
                cmd_append(buf, sizeof buf, &off, one);
            }
        }
    }

    /* пропорция: свап несёт геометрию с собой, побег — сбрасывает
       в 50/50; в обоих случаях слот 0 должен стать M% */
    if (escaped || !proportions_ok(&t)) {
        snprintf(one, sizeof one,
                 "[con_id=%" PRId64 "] resize set width %d ppt",
                 m, MASTER_PCT);
        cmd_append(buf, sizeof buf, &off, one);
    }

finish:
    /* ═══ СМЕРТЬ МАСТЕРА: фокус — новому мастеру. Sway сам фокусирует
       ДНО стопки (замерено) — подсветка не попадает на мастера. ═══ */
    if (act->died_ws == wid && t.focused_here) {
        snprintf(one, sizeof one,
                 "[con_id=%" PRId64 "] focus", t.id[0]);
        cmd_append(buf, sizeof buf, &off, one);
    }
    if (off > 0) {
        send_cmd(fd, buf);           /* здоровый стол — off == 0 */
        snap_evict(wid);             /* стол менялся — снимок погашен */
    } else {
        /* 0 команд = дерево совпало с каноном: снимок верен */
        snap_store(wid, jstr(ws, "name"), t.id[0], &t.id[1], t.n - 1);
    }
    return off > 0;
}

static bool retile_all(int fd, json_object *n, const Act *act, bool *moves_out)
{
    bool any = false;
    const char *type = jstr(n, "type");
    if (type != nullptr && strcmp(type, "workspace") == 0)
        return retile_ws(fd, n, act, moves_out); /* воркспейсы не вложены */
    json_object *nodes = jarr(n, "nodes");
    if (nodes == nullptr) return false;
    size_t cnt = json_object_array_length(nodes);
    for (size_t i = 0; i < cnt; i++)
        any |= retile_all(fd, json_object_array_get_idx(nodes, i), act, moves_out);
    return any;
}

/* ── Контрольный reconcile: после пачки с move-командами перечитать
   истинное состояние и доделать/проверить (максимум 2 раунда).
   Слепые перехваты передают сюда свой act — промоушен/смерть
   доводятся до конца, даже если пачка промолчала (чужой стол). ── */

static void reconcile_act(int fd, const Act *act)
{
    for (int i = 0; i < 2; i++) {
        json_object *tree = get_tree(fd);
        if (tree == nullptr) return;
        bool mv = false;
        bool any = retile_all(fd, tree, act, &mv);
        json_object_put(tree);
        if (getenv("SMS_DEBUG") != nullptr)
            fprintf(stderr, "reconcile: раунд %d, команды=%d, moves=%d\n",
                    i, any ? 1 : 0, mv ? 1 : 0);
        if (!any) break;    /* чистый круг — снимки свежие, хватит */
    }
}

static void reconcile(int fd)
{
    reconcile_act(fd, &ACT_NONE);
}

/* ── СЛЕПОЙ ПЕРЕХВАТ СПАВНА: пачка уходит ДО любого чтения дерева —
   успеть до первого кадра. Снимок здорового стола + метки заменяют
   get_tree: свап несёт место И геометрию мастера (нативный тайлинг
   без мигания), снятый мастер — вершиной стопки (dwm). Гвард
   workspace="имя": родилось на чужом столе — пачка молчит, reconcile
   с act.promote расставит на СВОЁМ столе. Гвард tiling на командах
   окна-новичка — страховка от гонки «мс-задержки» вердикта: если
   окно успели всплавить for_window-правилом ДО ИСПОЛНЕНИЯ пачки,
   свап/переметка просто не сработают (критерии sway матчатся в
   момент исполнения команды, а не отправки). ── */

static bool spawn_fast(int fd, int64_t nid)
{
    if (nid <= 0) return false;
    Snap *e = snap_get(g_focus_ws);
    if (e == nullptr || e->master <= 0 || e->name[0] == '\0')
        return false;                /* нет свежего снимка — честный путь */
    char mk_m[24], mk_s[24], mk_p[24];
    snprintf(mk_m, sizeof mk_m, "_m%" PRId64, g_focus_ws);
    snprintf(mk_s, sizeof mk_s, "_s%" PRId64, g_focus_ws);
    snprintf(mk_p, sizeof mk_p, "_p%" PRId64, g_focus_ws);

    char buf[640], one[224];
    size_t off = 0;
    buf[0] = '\0';
    snprintf(one, sizeof one,
             "[con_id=%" PRId64 " workspace=\"^%s$\" tiling] swap container with mark %s",
             nid, e->name, mk_m);
    cmd_append(buf, sizeof buf, &off, one);
    snprintf(one, sizeof one,
             "[con_id=%" PRId64 " workspace=\"^%s$\" tiling] mark --replace %s",
             nid, e->name, mk_m);
    cmd_append(buf, sizeof buf, &off, one);
    /* спавн — смена мастера «со стороны»: память возврата гасим */
    snprintf(one, sizeof one, "[con_mark=%s] unmark %s", mk_p, mk_p);
    cmd_append(buf, sizeof buf, &off, one);
    if (e->n >= 1) {                 /* стопка жива: снятый — вершина */
        snprintf(one, sizeof one,
                 "[con_id=%" PRId64 " workspace=\"^%s$\"] move container to mark %s",
                 e->master, e->name, mk_s);
        cmd_move(buf, sizeof buf, &off, one, e->master);
        snprintf(one, sizeof one,
                 "[con_id=%" PRId64 " workspace=\"^%s$\"] swap container with mark %s",
                 e->master, e->name, mk_s);
        cmd_append(buf, sizeof buf, &off, one);
        /* якорная метка — на нового верховика стопки */
        snprintf(one, sizeof one,
                 "[con_id=%" PRId64 " workspace=\"^%s$\"] mark --replace %s",
                 e->master, e->name, mk_s);
        cmd_append(buf, sizeof buf, &off, one);
    }
    if (getenv("SMS_DEBUG") != nullptr)
        fprintf(stderr, "spawn-fast: ws=%" PRId64 " окно=%" PRId64 "\n",
                g_focus_ws, nid);
    send_cmd(fd, buf);
    snap_evict(g_focus_ws);
    Act act = { .promote = nid, .moved = 0, .died_ws = 0 };
    reconcile_act(fd, &act);
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
   мастера (+фокус, если юзер здесь); канон достроит следующий
   спавн/зом. Снимка нет / имя пусто / мастер был один — честный
   путь с get_tree. Слепые команды по ГОЛОЙ метке _s запрещены
   (метка могла уехать с окном на другой стол — драг). ── */

static bool death_fast(int fd, int64_t wsid)
{
    if (wsid <= 0) return false;
    Snap *e = snap_get(wsid);
    if (e == nullptr || e->n < 1 || e->name[0] == '\0')
        return false;     /* нет снимка / мастер был один — честный путь */
    char mk_m[24], mk_s[24], mk_p[24];
    snprintf(mk_m, sizeof mk_m, "_m%" PRId64, wsid);
    snprintf(mk_s, sizeof mk_s, "_s%" PRId64, wsid);
    snprintf(mk_p, sizeof mk_p, "_p%" PRId64, wsid);

    char buf[640], one[224];
    size_t off = 0;
    buf[0] = '\0';
    int64_t top = e->stack[0];
    if (e->n >= 2) {                 /* стопка жива: верхушка — в мастер */
        snprintf(one, sizeof one,
                 "[con_id=%" PRId64 " workspace=\"^%s$\"] move left",
                 top, e->name);
        cmd_move(buf, sizeof buf, &off, one, top);
        snprintf(one, sizeof one,
                 "[con_id=%" PRId64 " workspace=\"^%s$\"] mark --replace %s",
                 top, e->name, mk_m);
        cmd_append(buf, sizeof buf, &off, one);
        snprintf(one, sizeof one,
                 "[con_id=%" PRId64 " workspace=\"^%s$\"] resize set width %d ppt",
                 top, e->name, MASTER_PCT);
        cmd_append(buf, sizeof buf, &off, one);
        /* якорная метка — на нового верховика стопки */
        snprintf(one, sizeof one,
                 "[con_id=%" PRId64 " workspace=\"^%s$\"] mark --replace %s",
                 e->stack[1], e->name, mk_s);
        cmd_append(buf, sizeof buf, &off, one);
    } else {
        /* ПОСЛЕДНЕЕ окно стопки: НЕ ДВИГАЕМ — move left на синглтоне
           (i3-семантика новых sway) = ВАРП НА СОСЕДНИЙ ВЫХОД. Только
           метка мастера; выжившее и так на весь стол, канон достроит
           следующий спавн/зом */
        snprintf(one, sizeof one,
                 "[con_id=%" PRId64 " workspace=\"^%s$\"] mark --replace %s",
                 top, e->name, mk_m);
        cmd_append(buf, sizeof buf, &off, one);
    }
    snprintf(one, sizeof one, "[con_mark=%s] unmark %s", mk_p, mk_p);
    cmd_append(buf, sizeof buf, &off, one);
    if (g_focus_ws == wsid) {        /* юзер здесь — подсветить мастера */
        snprintf(one, sizeof one,
                 "[con_id=%" PRId64 " workspace=\"^%s$\"] focus",
                 top, e->name);
        cmd_append(buf, sizeof buf, &off, one);
    }
    if (getenv("SMS_DEBUG") != nullptr)
        fprintf(stderr, "death-fast: ws=%" PRId64 "\n", wsid);
    send_cmd(fd, buf);
    snap_evict(wsid);
    /* фокус уже в пачке (если юзер здесь) — reconcile без акта смерти:
       иначе каждый круг повторял бы focus и снимки не сохранились бы */
    reconcile(fd);
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

   Алгоритм: вычитать всё, что уже в буфере (не блокируя); если
   мало — до 3×1мс подождать. floating-для-новичка → ПЛАВАЮЩИЙ
   (пачку не шлём вовсе; само событие floating встаёт в очередь и
   его штатный обработчик делает «move position center» + reconcile).
   close-для-новичка → окно умерло (смерть разрулит штатный close из
   очереди). focus-для-новичка → критерии уже отработали и НЕ
   всплавили → тайловый, пачка немедленно. Тишина до конца грации →
   тайловый (страховка — гвард tiling в пачке + reconcile: в
   экстремальном случае свап промолчит, метка _m останется на
   прежнем мастере и долечит всё обратно). Все вычитанные события
   (title/focus/чужие) — в FIFO-очередь: главный цикл разберёт их
   ПОСЛЕ спавна, в исходном порядке, штатными обработчиками. ── */

enum { SV_TILED = 0, SV_FLOAT = 1, SV_DEAD = 2 };

#define PEND_MAX 128
static char    *g_pend[PEND_MAX];
static uint32_t g_pend_type[PEND_MAX];
static int      g_pend_n = 0;

static void pend_push(uint32_t type, const char *payload)
{
    if (payload == nullptr) return;
    if (g_pend_n >= PEND_MAX) {       /* переполнение теоретическое:
                                         жертвуем самым старым — цикл
                                         дочитает живое из сокета */
        free(g_pend[0]);
        memmove(g_pend, g_pend + 1,
                (size_t)(PEND_MAX - 1) * sizeof g_pend[0]);
        memmove(g_pend_type, g_pend_type + 1,
                (size_t)(PEND_MAX - 1) * sizeof g_pend_type[0]);
        g_pend_n = PEND_MAX - 1;
    }
    char *dup = strdup(payload);
    if (dup == nullptr) return;
    g_pend[g_pend_n] = dup;
    g_pend_type[g_pend_n] = type;
    g_pend_n++;
}

/* вычитать доступное без блокировки; -1 = вердикта в буфере нет */
static int spawn_drain(int sub_fd, int64_t nid)
{
    struct pollfd pf;
    memset(&pf, 0, sizeof pf);
    pf.fd = sub_fd;
    pf.events = POLLIN;
    for (;;) {
        pf.revents = 0;
        if (poll(&pf, 1, 0) <= 0 || !(pf.revents & POLLIN)) break;
        uint32_t type;
        uint8_t *payload = ipc_recv(sub_fd, &type);
        if (payload == nullptr) break;    /* sway умер — цикл увидит EOF */
        int v = -1;
        if (type == EV_WINDOW) {
            json_object *ev = json_tokener_parse((const char *)payload);
            if (ev != nullptr) {
                const char *ch = jstr(ev, "change");
                json_object *con = jget(ev, "container");
                int64_t id = (con != nullptr) ? jint(con, "id") : -1;
                if (ch != nullptr && id == nid) {
                    if (strcmp(ch, "floating") == 0 && is_floating(con))
                        v = SV_FLOAT;
                    else if (strcmp(ch, "close") == 0)
                        v = SV_DEAD;
                    else if (strcmp(ch, "focus") == 0)
                        v = SV_TILED;  /* критерии отработали — не всплыло */
                }
                json_object_put(ev);
            }
        }
        pend_push(type, (const char *)payload);  /* само событие — в
                                                    очередь, разберёт
                                                    штатный обработчик */
        free(payload);
        if (v >= 0) return v;
    }
    return -1;
}

static int spawn_verdict(int sub_fd, int64_t nid)
{
    int v = spawn_drain(sub_fd, nid);
    if (v >= 0) goto out;
    /* грация: sway дописывает floating/focus того же view_map —
       микросекунды; 3×1мс с запасом, кадр (16мс) не страдает */
    struct pollfd pf;
    memset(&pf, 0, sizeof pf);
    pf.fd = sub_fd;
    pf.events = POLLIN;
    for (int i = 0; i < 3; i++) {
        pf.revents = 0;
        if (poll(&pf, 1, 1) <= 0 || !(pf.revents & POLLIN)) break;
        v = spawn_drain(sub_fd, nid);
        if (v >= 0) goto out;
    }
    v = SV_TILED;
out:
    if (getenv("SMS_DEBUG") != nullptr)
        fprintf(stderr, "spawn-verdict: окно=%" PRId64 " → %s\n", nid,
                v == SV_FLOAT ? "плавающий" : v == SV_DEAD ? "умер" : "тайловый");
    return v;
}

/* ── События окон: эхо глотаем, спавн/смерть мастера — слепые
   перехваты, остальное — ребилд + reconcile ── */

static void handle_event(int sub_fd, int cmd_fd, const char *payload)
{
    json_object *ev = json_tokener_parse(payload);
    if (ev == nullptr) return;

    Act act = { .promote = 0, .moved = 0, .died_ws = 0 };
    bool need_retile = false;
    bool need_center  = false;
    int64_t cid = -1;
    const char *change = jstr(ev, "change");
    json_object *con = jget(ev, "container");
    if (con != nullptr) cid = jint(con, "id");

    if (getenv("SMS_DEBUG") != nullptr && change != nullptr)
        fprintf(stderr, "ev: %s id=%" PRId64 "\n", change, cid);

    if (change != nullptr) {
        if (strcmp(change, "new") == 0) {
            if (is_floating(con)) {
                need_center = true;
            } else {
                /* вердикт нужен ТОЛЬКО слепому пути: без снимка
                   spawn_fast молчит и так, честный путь floating
                   просто не заметит (collect читает только тайлы).
                   Подглядывание стоит 0..3мс — только когда пачка
                   реально уйдёт */
                Snap *pre = snap_get(g_focus_ws);
                if (pre == nullptr || pre->master <= 0 || pre->name[0] == '\0') {
                    need_retile = true; act.promote = cid;
                } else {
                    int v = spawn_verdict(sub_fd, cid);
                    if (v == SV_FLOAT || v == SV_DEAD) {
                        /* пачки нет; floating/close уже в очереди —
                           их штатные обработчики сделают всё сами */
                    } else if (!spawn_fast(cmd_fd, cid)) {
                        need_retile = true; act.promote = cid;
                    }
                }
            }
        } else if (strcmp(change, "close") == 0) {
            if (!is_floating(con)) {
                int64_t dws = dead_master_ws(con); /* 0 = умер не мастер */
                if (dws > 0 && death_fast(cmd_fd, dws)) {
                    json_object_put(ev);
                    return;
                }
                need_retile = true;
                act.died_ws = dws;
            }
        } else if (strcmp(change, "floating") == 0) {
            if (is_floating(con)) { need_center = true; need_retile = true; }
            else need_retile = true;
        } else if (strcmp(change, "move") == 0) {
            if (!is_floating(con)) {
                if (echo_consume(cid)) {  /* наше эхо — без пересчёта */
                    json_object_put(ev);
                    return;
                }
                need_retile = true; act.moved = cid;
            }
        }
    }

    if (need_center && cid > 0) {
        char cmd[64];
        snprintf(cmd, sizeof cmd,
                 "[con_id=%" PRId64 "] move position center", cid);
        send_cmd(cmd_fd, cmd);
    }
    if (need_retile) {
        json_object *tree = get_tree(cmd_fd);
        if (tree != nullptr) {
            bool mv = false;
            bool any = retile_all(cmd_fd, tree, &act, &mv);
            json_object_put(tree);
            /* пачка была (даже только метки) — контрольный reconcile:
               долечить и обновить снимки для слепых путей */
            if (any) reconcile(cmd_fd);
        }
    }
    json_object_put(ev);
}

/* ── workspace-события: только focus — трекинг стола для слепых
   путей (спавн летит на фокусный стол). reload/init/rename — тишина. ── */

static void handle_ws_event(int cmd_fd, const char *payload)
{
    (void)cmd_fd;
    json_object *ev = json_tokener_parse(payload);
    if (ev == nullptr) return;
    const char *change = jstr(ev, "change");
    if (change != nullptr && strcmp(change, "focus") == 0) {
        json_object *cur = jget(ev, "current");
        int64_t id = (cur != nullptr) ? jint(cur, "id") : 0;
        if (id > 0) g_focus_ws = id;
        if (getenv("SMS_DEBUG") != nullptr)
            fprintf(stderr, "ws: focus=%" PRId64 "\n", g_focus_ws);
    }
    json_object_put(ev);
}

/* ── Сфокусированный лист (тайл или плавающее) + его воркспейс ── */

static json_object *find_focused(json_object *n, json_object *ws,
                                 json_object **ws_out)
{
    const char *type = jstr(n, "type");
    if (type != nullptr && strcmp(type, "workspace") == 0)
        ws = n;
    else if (type != nullptr
             && (strcmp(type, "con") == 0
                 || strcmp(type, "floating_con") == 0)) {
        /* плавающие в get_tree имеют type "floating_con"! */
        json_object *nodes = jarr(n, "nodes");
        if ((nodes == nullptr || json_object_array_length(nodes) == 0)
            && jbool(n, "focused")) {
            *ws_out = ws;
            return n;
        }
    }
    json_object *ch[2] = { jarr(n, "nodes"), jarr(n, "floating_nodes") };
    for (int k = 0; k < 2; k++) {
        if (ch[k] == nullptr) continue;
        size_t cnt = json_object_array_length(ch[k]);
        for (size_t i = 0; i < cnt; i++) {
            json_object *r =
                find_focused(json_object_array_get_idx(ch[k], i), ws, ws_out);
            if (r != nullptr) return r;
        }
    }
    return nullptr;
}

/* ── Сокет и main ── */

[[nodiscard]] static int sway_connect(void)
{
    const char *path = getenv("SWAYSOCK");
    if (path == nullptr || path[0] == '\0') path = getenv("I3SOCK");
    if (path == nullptr || path[0] == '\0') return -1;
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    struct sockaddr_un sa;
    memset(&sa, 0, sizeof sa);
    sa.sun_family = AF_UNIX;
    if (strlen(path) >= sizeof sa.sun_path) { close(fd); return -1; }
    strcpy(sa.sun_path, path);
    if (connect(fd, (struct sockaddr *)&sa, sizeof sa) < 0) {
        close(fd); return -1;
    }
    return fd;
}

[[nodiscard]] static bool subscribe_events(int fd)
{
    const char payload[] = "[\"window\",\"workspace\"]";
    if (!ipc_send(fd, SUBSCRIBE, payload, sizeof payload - 1)) return false;
    uint32_t type;
    uint8_t *reply = ipc_recv(fd, &type);
    if (reply == nullptr) return false;
    json_object *obj = json_tokener_parse((const char *)reply);
    free(reply);
    if (obj == nullptr) return false;
    json_object *v = jget(obj, "success");
    bool ok = v != nullptr && json_object_get_boolean(v);
    json_object_put(obj);
    return ok;
}

/* ── Мягкая остановка: SIGTERM/SIGINT — просто выйти (нечего снимать,
   демон ничего не ставил в конфиге юзера) ── */

static volatile sig_atomic_t g_stop = 0;
static void on_stop(int sig) { (void)sig; g_stop = 1; }

int main(int argc, char **argv)
{
    signal(SIGPIPE, SIG_IGN);

    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_stop;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGTERM, &sa, nullptr);
    sigaction(SIGINT, &sa, nullptr);

    /* пропорция выбирается ЗАРАНЕЕ, до старта: env SMS_PCT (10..90) */
    const char *pct = getenv("SMS_PCT");
    if (pct != nullptr && pct[0] != '\0') {
        int v = atoi(pct);
        if (v >= 10 && v <= 90)
            MASTER_PCT = v;
    }

    /* CLI сильнее env: `sms N`, N = 10..90 — доля мастера, %.
       Неверный аргумент — usage и выход 1: молча гадать нельзя. */
    if (argc > 1) {
        char *end = nullptr;
        long v = strtol(argv[1], &end, 10);
        if (argc > 2 || end == argv[1] || *end != '\0'
            || v < 10 || v > 90) {
            fprintf(stderr,
                    "использование: %s [N]   # N = 10..90, доля мастера, %%\n",
                    argv[0]);
            return 1;
        }
        MASTER_PCT = (int)v;
    }

    int sub_fd = sway_connect();
    if (sub_fd < 0) { fprintf(stderr, "sms: нет $SWAYSOCK\n"); return 1; }
    int cmd_fd = sway_connect();
    if (cmd_fd < 0) { close(sub_fd); return 1; }
    if (!subscribe_events(sub_fd)) {
        fprintf(stderr, "sms: подписка не удалась\n");
        close(sub_fd); close(cmd_fd); return 1;
    }

    printf("sms: запущен (%d/%d)\n", MASTER_PCT, 100 - MASTER_PCT);
    fflush(stdout);

    {
        json_object *tree = get_tree(cmd_fd);
        if (tree != nullptr) {
            json_object *fws = nullptr;    /* семя фокусного стола */
            (void)find_focused(tree, nullptr, &fws);
            if (fws != nullptr) g_focus_ws = jint(fws, "id");
            bool mv = false;
            bool any = retile_all(cmd_fd, tree, &ACT_NONE, &mv);
            json_object_put(tree);
            if (any) reconcile(cmd_fd);  /* долечить + засеять снимки */
        }
    }

    struct pollfd pf;
    memset(&pf, 0, sizeof pf);
    pf.fd = sub_fd;
    pf.events = POLLIN;
    for (;;) {
        /* сначала — события, вычитанные подглядыванием вердикта:
           порядок FIFO = исходный порядок сокета */
        if (g_pend_n > 0) {
            if (g_stop) break;
            uint32_t type = g_pend_type[0];
            char *payload = g_pend[0];
            g_pend_n--;
            if (g_pend_n > 0) {
                memmove(g_pend, g_pend + 1,
                        (size_t)g_pend_n * sizeof g_pend[0]);
                memmove(g_pend_type, g_pend_type + 1,
                        (size_t)g_pend_n * sizeof g_pend_type[0]);
            }
            if (type == EV_WINDOW)
                handle_event(sub_fd, cmd_fd, payload);
            else if (type == EV_WORKSPACE)
                handle_ws_event(cmd_fd, payload);
            free(payload);
            continue;
        }
        int r = poll(&pf, 1, 500);
        if (g_stop) break;               /* юзер остановил — выйти */
        if (r < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (r == 0) continue;            /* тик таймаута — просто дальше */
        uint32_t type;
        uint8_t *payload = ipc_recv(sub_fd, &type);
        if (payload == nullptr) break;   /* sway умер */
        if (type == EV_WINDOW)
            handle_event(sub_fd, cmd_fd, (const char *)payload);
        else if (type == EV_WORKSPACE)
            handle_ws_event(cmd_fd, (const char *)payload);
        free(payload);
    }

    close(sub_fd);
    close(cmd_fd);
    return 0;
}
