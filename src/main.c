/* main.c — DWM-style master/stack tiling daemon для Sway (Sway IPC).
 *
 * Инвариант воркспейса — ровно две зоны, 55/45:
 *     splith { мастер _m<ws> (55%) | splitv-стопка _s<ws> (45%) }
 *
 * Смена мастера — ОБА направления, за ОДНО перемещение:
 *   • зум (move left на окне стопки): окно вырывается на уровень мастера
 *     → swap container with mark _m (цель-МЕТКА: con_id-цель в sway
 *     молча падает — источник «двух ходов»). Свап обменивает позицию
 *     И геометрию: окно наследует 55% сразу.
 *   • демо (move right на мастере): Sway вбрасывает его в стопку →
 *     верхушка становится мастером, старый — в стопку.
 *
 * Ребилд — канонический и идемпотентный: якорь стопки оборачивается
 * в свежий splitv, уровень мастера разворачивается в splith (по листу
 * + дублем по con_id контейнера), блудные окна съезжают к метке _s.
 * Сколько бы раз ни демоить — сетка не разваливается.
 *
 * Пропорции — намертво 55/45 (проверка на каждом ретайле, ±3%).
 * Плавающие окна центрируются при рождении/уходе, дальше не трогаются.
 *
 * Память: на итерацию ровно 5 выделений, все освобождаются; ipc_recv
 * дренажирует битые фреймы.
 *
 * build: clang -std=c23 -Wall -Wextra -Wpedantic -Wconversion -Wshadow \
 *   -Wformat=2 -Werror=format-security -O3 -fsanitize=address,undefined \
 *   main.c -ljson-c -o msd
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
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <json-c/json.h>

/* ══════════════ ТЮНИНГ ══════════════ */
constexpr int MASTER_PCT = 55;
constexpr int PCT_TOLERANCE = 3;
constexpr int TILED_MAX = 64;

constexpr uint32_t IPC_MAX_LEN = 16u * 1024u * 1024u;
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

/* ── Сбор тайлов: один DFS, порядок дерева (= визуальный порядок) ── */

typedef struct {
    int n;                          /* тайловых окон */
    int64_t id[TILED_MAX];
    json_object *parent[TILED_MAX]; /* родитель каждого (жив, пока живо дерево) */
    int64_t rw[TILED_MAX];          /* rect.width — СЛОТ, не окно */
    int64_t ws_w;                   /* rect.width воркспейса */
    int64_t master_id;              /* держатель _m<ws>, -1 = нет */
    int64_t stack_id;               /* держатель _s<ws>, -1 = нет */
} Tiled;

static void collect(json_object *n, json_object *parent,
                    const char *mk_m, const char *mk_s, Tiled *t)
{
    int64_t id = jint(n, "id");
    if (t->master_id == -1 && has_mark(n, mk_m))
        t->master_id = id;
    if (t->stack_id == -1 && has_mark(n, mk_s))
        t->stack_id = id;
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
        collect(json_object_array_get_idx(nodes, i), n, mk_m, mk_s, t);
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

static void send_cmd(int fd, const char *cmd)
{
    if (!ipc_send(fd, RUN_COMMAND, cmd, strlen(cmd))) return;
    uint32_t type;
    free(ipc_recv(fd, &type)); /* ответ вычитываем, чтобы не рассинхрониться */
}

[[nodiscard]] static json_object *get_tree(int fd)
{
    uint32_t type;
    if (!ipc_send(fd, GET_TREE, nullptr, 0)) return nullptr;
    uint8_t *buf = ipc_recv(fd, &type);
    if (buf == nullptr) return nullptr;
    json_object *tree = json_tokener_parse((const char *)buf);
    free(buf);
    return tree;
}

/* ── Пропорции: мастер ≈ MASTER_PCT% ширины воркспейса? ── */

[[nodiscard]] static bool proportions_ok(const Tiled *t)
{
    if (t->n < 2 || t->rw[0] <= 0 || t->ws_w <= 0) return true;
    int pct = (int)((t->rw[0] * 100) / t->ws_w);
    return pct >= MASTER_PCT - PCT_TOLERANCE
        && pct <= MASTER_PCT + PCT_TOLERANCE;
}

/* ── Retile: канонический ребилд одного воркспейса ── */

static void retile_ws(int fd, json_object *ws, int64_t preferred)
{
    int64_t wid = jint(ws, "id");
    if (wid <= 0) return;
    char mk_m[24], mk_s[24];
    snprintf(mk_m, sizeof mk_m, "_m%" PRId64, wid);
    snprintf(mk_s, sizeof mk_s, "_s%" PRId64, wid);

    Tiled t;
    memset(&t, 0, sizeof t);
    t.master_id = -1;
    t.stack_id = -1;
    json_object *wr = jget(ws, "rect");
    t.ws_w = (wr != nullptr) ? jint(wr, "width") : 0;
    collect(ws, ws, mk_m, mk_s, &t);
    if (t.n == 0) return;

    char buf[8192], one[128];
    size_t off = 0;
    buf[0] = '\0';

    json_object *p0 = t.parent[0];   /* уровень мастера */
    int pi = -1;
    if (preferred > 0) {
        for (int i = 0; i < t.n; i++)
            if (t.id[i] == preferred) { pi = i; break; }
    }

    /* ═══ ZOOM: окно-событие вырвалось на уровень мастера (move left) —
       свап с мастером ЧЕРЕЗ МЕТКУ: цель-con_id в sway молча падает,
       что и давало «два хода». Свап обменивает позицию И геометрию —
       окно наследует 55% сразу, resize после него не нужен. ═══ */
    if (pi > 0 && preferred != t.master_id && t.parent[pi] == p0) {
        snprintf(one, sizeof one,
                 "[con_id=%" PRId64 "] swap container with mark %s",
                 preferred, mk_m);
        cmd_append(buf, sizeof buf, &off, one);
        t.id[pi] = t.id[0];
        t.id[0] = preferred;
        /* parent[pi] == parent[0] == p0 — массив остаётся консистентен */
    }
    /* ═══ DEMO: двигали САМОГО мастера (move right) — Sway вбросил его
       в стопку: он уже не единственный лист своего уровня, либо вообще
       не DFS-первый. Верхушка становится мастером, старый — в стопку. ═══ */
    else if (preferred == t.master_id && t.n >= 2
             && (pi != 0 || t.parent[1] == p0)) {
        if (pi == 0) {
            snprintf(one, sizeof one,
                     "[con_id=%" PRId64 "] swap container with mark %s",
                     t.id[1], mk_m);
            cmd_append(buf, sizeof buf, &off, one);
            int64_t tmp = t.id[0];
            t.id[0] = t.id[1];
            t.id[1] = tmp;
        }
        /* pi != 0: DFS-первый уже не старый мастер — свап не нужен */
    }

    int64_t m = t.id[0];
    if (t.master_id != m) {
        snprintf(one, sizeof one,
                 "[con_id=%" PRId64 "] mark --replace %s", m, mk_m);
        cmd_append(buf, sizeof buf, &off, one);
    }

    if (t.n == 1) {
        /* подготовка к следующему окну: родитель — в горизонталь */
        if (!layout_is(p0, "splith")) {
            snprintf(one, sizeof one,
                     "[con_id=%" PRId64 "] layout splith", m);
            cmd_append(buf, sizeof buf, &off, one);
        }
        if (off > 0) send_cmd(fd, buf);
        return;
    }

    /* якорь стопки: первый не-мастер, УЖЕ живущий в контейнере;
       если все плоско (рождение/демо/частокол) — tiled[1] */
    int ai = 1;
    for (int i = 1; i < t.n; i++) {
        if (t.parent[i] != p0) { ai = i; break; }
    }
    int64_t a = t.id[ai];
    json_object *ac = t.parent[ai];
    bool flat = (ac == p0);

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
        /* контейнер якоря — в вертикаль: по листу (применяет к родителю)
           и дублем по con_id контейнера — страховка от расхождений */
        snprintf(one, sizeof one,
                 "[con_id=%" PRId64 "] layout splitv", a);
        cmd_append(buf, sizeof buf, &off, one);
        int64_t acid = jint(ac, "id");
        if (acid > 0) {
            snprintf(one, sizeof one,
                     "[con_id=%" PRId64 "] layout splitv", acid);
            cmd_append(buf, sizeof buf, &off, one);
        }
    }
    if (!layout_is(p0, "splith")) {
        /* уровень мастера — в горизонталь (демо-контейнер, вертикальный
           ws): та же двойная страховка */
        snprintf(one, sizeof one,
                 "[con_id=%" PRId64 "] layout splith", m);
        cmd_append(buf, sizeof buf, &off, one);
        int64_t p0id = jint(p0, "id");
        if (p0id > 0) {
            snprintf(one, sizeof one,
                     "[con_id=%" PRId64 "] layout splith", p0id);
            cmd_append(buf, sizeof buf, &off, one);
        }
    }

    /* сбор: все не-мастер окна вне контейнера якоря едут к метке _s —
       вставка СЛЕДУЮЩЕЙ ЗА якорем = внутрь стопки. Обратный порядок
       сохраняет визуальный порядок стека. */
    for (int i = t.n - 1; i >= 1; i--) {
        if (i == ai) continue;
        if (!flat && t.parent[i] == ac) continue;
        snprintf(one, sizeof one,
                 "[con_id=%" PRId64 "] move container to mark %s",
                 t.id[i], mk_s);
        cmd_append(buf, sizeof buf, &off, one);
    }

    if (!proportions_ok(&t)) {
        snprintf(one, sizeof one,
                 "[con_id=%" PRId64 "] resize set width %d ppt",
                 m, MASTER_PCT);
        cmd_append(buf, sizeof buf, &off, one);
    }
    if (off > 0) send_cmd(fd, buf); /* здоровый стол — off == 0 */
}

static void retile_all(int fd, json_object *n, int64_t preferred)
{
    const char *type = jstr(n, "type");
    if (type != nullptr && strcmp(type, "workspace") == 0) {
        retile_ws(fd, n, preferred);
        return; /* воркспейсы не вложены */
    }
    json_object *nodes = jarr(n, "nodes");
    if (nodes == nullptr) return;
    size_t cnt = json_object_array_length(nodes);
    for (size_t i = 0; i < cnt; i++)
        retile_all(fd, json_object_array_get_idx(nodes, i), preferred);
}

/* ── События: единый exit-путь, владение только здесь ── */

static void handle_event(int cmd_fd, const char *payload)
{
    json_object *ev = json_tokener_parse(payload);
    if (ev == nullptr) return;

    bool need_retile = false;
    bool need_center  = false;
    int64_t preferred = -1;
    int64_t cid = -1;
    const char *change = jstr(ev, "change");
    json_object *con = jget(ev, "container");
    if (con != nullptr) cid = jint(con, "id");

    if (change != nullptr) {
        if (strcmp(change, "new") == 0) {
            if (is_floating(con)) need_center = true;
            else need_retile = true;
        } else if (strcmp(change, "close") == 0) {
            if (!is_floating(con)) need_retile = true;
        } else if (strcmp(change, "floating") == 0) {
            if (is_floating(con)) { need_center = true; need_retile = true; }
            else need_retile = true;
        } else if (strcmp(change, "move") == 0) {
            if (!is_floating(con)) { need_retile = true; preferred = cid; }
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
            retile_all(cmd_fd, tree, preferred);
            json_object_put(tree);
        }
    }
    json_object_put(ev);
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

[[nodiscard]] static bool subscribe_window(int fd)
{
    const char payload[] = "[\"window\"]";
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

int main(void)
{
    signal(SIGPIPE, SIG_IGN);

    int sub_fd = sway_connect();
    if (sub_fd < 0) { fprintf(stderr, "msd: нет $SWAYSOCK\n"); return 1; }
    int cmd_fd = sway_connect();
    if (cmd_fd < 0) { close(sub_fd); return 1; }
    if (!subscribe_window(sub_fd)) {
        fprintf(stderr, "msd: подписка не удалась\n");
        close(sub_fd); close(cmd_fd); return 1;
    }

    json_object *tree = get_tree(cmd_fd);
    if (tree != nullptr) {
        retile_all(cmd_fd, tree, -1);
        json_object_put(tree);
    }

    for (;;) {
        uint32_t type;
        uint8_t *payload = ipc_recv(sub_fd, &type);
        if (payload == nullptr) break;
        if (type == EV_WINDOW)
            handle_event(cmd_fd, (const char *)payload);
        free(payload);
    }

    close(sub_fd);
    close(cmd_fd);
    return 0;
}
