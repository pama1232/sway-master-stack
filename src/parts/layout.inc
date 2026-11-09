/* layout.inc — Гварды воркспейса и сбор тайлов.
 *
 * Фрагмент единого translation unit: включается из main.c после sms.h.
 * Порядок включения повторяет прежний порядок объявлений в main.c.
 * Экранирование имён для критериев Sway, DFS-сбор Tiled и idx_of. */

#include "../sms.h"

/* ── Гвард стола для слепых пачек: workspace="^имя$" — критерии sway
   матчатся как PCRE (i3 userguide: «…are actually regular
   expressions»). Эскейп — РОВНО ОДИН бэкслэш перед не-алнумерическим
   ASCII-байтом: критерии-парсер sway (criteria.c, unescape) съедает
   бэкслэш ТОЛЬКО перед кавычкой, остальное PCRE получает как есть
   (проверено на живом sway 1.10: «\:\ » матчит «: », «\\:» — уже
   нет). Байты ≥0x80 (UTF-8) — не метасимволы, идут как есть:
   поштучное эскейпирование разрывало последовательность и валило
   весь регэксп под PCRE2_UTF — sway молча отвергал пачку. Кавычка
   имени — «\"», бэкслэш — «\\»; имя, кончающееся на «\», с «\»
   перед кавычкой или с контроль-байтами — отказ (false = слепой
   путь молчит, честный путь с get_tree расставит сам): такие хвосты
   ломают поиск закрывающей кавычки критерия. ── */
static bool ws_guard(char *out, size_t cap, const char *name) {
  if (name == nullptr || name[0] == '\0' || cap < 16)
    return false;
  size_t len = strlen(name);
  if (len > 0 && name[len - 1] == '\\')
    return false;
  static const char pre[] = "workspace=\"^";
  size_t o = sizeof pre - 1;
  memcpy(out, pre, o);
  for (const unsigned char *p = (const unsigned char *)name; *p != '\0'; p++) {
    unsigned char c = *p;
    if (c < 0x20)
      return false; /* контроль-байты */
    if (c == '\\' && p[1] == '"')
      return false; /* неоднозначно */
    bool alnum = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                 (c >= '0' && c <= '9') || c >= 0x80;
    size_t need = alnum ? 1u : 2u;
    if (o + need + 3u > cap)
      return false; /* «$", NUL */
    if (alnum) {
      out[o++] = (char)c;
    } else {
      out[o++] = '\\'; /* один уровень: PCRE видит «\c» */
      out[o++] = (char)c;
    }
  }
  out[o++] = '$';
  out[o++] = '"';
  out[o] = '\0';
  return true;
}

/* ── Удержание фокуса на ПУСТОМ столе: критерии sway матчат только
   контейнеры (tiling+floating; root_for_each_container в исходниках
   sway воркспейс-узел кандидатом не делает) — на опустевшем столе
   `[workspace="…"] focus` всегда даёт «No matching node». Поэтому
   адресуем стол напрямую: `workspace --no-auto-back-and-forth "имя"`
   — переключает и на пустой стол, флаг гасит auto_back_and_forth из
   конфига юзера. Имя — одним токеном в кавычках: резчик пачек «;»
   кавычки уважает, внешние кавычки парсер снимает, байты ≥0x80
   проходят как есть. Отказ (false = ничего не посылать): имена,
   перехватываемые парсером как ключевые слова, и имена с кавычками/
   бэкслэшем/контроль-байтами — они не переживают парсер. Отказ
   безвреден: фокус просто не удерживаем. ── */
static bool ws_focus_cmd(char *out, size_t cap, const char *name) {
  static const char *const kw[] = {"next",           "prev",
                                   "current",        "back_and_forth",
                                   "next_on_output", "prev_on_output",
                                   "number",         "output",
                                   "gaps",           "--no-auto-back-and-forth",
                                   nullptr};
  if (name == nullptr || name[0] == '\0')
    return false;
  for (int k = 0; kw[k] != nullptr; k++)
    if (strcasecmp(name, kw[k]) == 0)
      return false;
  for (const unsigned char *p = (const unsigned char *)name; *p != '\0'; p++)
    if (*p == '"' || *p == '\'' || *p == '\\' || *p < 0x20)
      return false;
  int n = snprintf(out, cap, "workspace --no-auto-back-and-forth \"%s\"", name);
  return n > 0 && (size_t)n < cap;
}

/* ── Сбор тайлов: один DFS, порядок дерева (= визуальный порядок).
   Массивы — СЛОТЫ: id/parent/rw[i] описывают слот i, а не окно. ── */

typedef struct {
  int n; /* тайловых окон */
  int64_t id[TILED_MAX];
  json_object *parent[TILED_MAX]; /* родитель слота (жив, пока живо дерево) */
  int64_t rw[TILED_MAX];          /* rect.width слота */
  int64_t ws_w;                   /* rect.width воркспейса */
  int64_t master_id;              /* держатель _m<ws>, -1 = нет */
  int64_t stack_id;               /* держатель _s<ws>, -1 = нет */
  int64_t partner_id; /* держатель _p<ws> (место возврата), -1 = нет */
} Tiled;

static void collect(json_object *n, json_object *parent, const char *mk_m,
                    const char *mk_s, const char *mk_p, Tiled *t) {
  int64_t id = jint(n, "id");
  json_object *nodes = jarr(n, "nodes"); /* один лукап на узел */
  /* три метки — ОДНИМ проходом по marks: три отдельных has_mark дали бы
     три хеш-лукапа json_object_object_get_ex на узел */
  if (t->master_id == -1 || t->stack_id == -1 || t->partner_id == -1) {
    json_object *marks = jarr(n, "marks");
    if (marks != nullptr) {
      size_t mc = json_object_array_length(marks);
      for (size_t mi = 0; mi < mc; mi++) {
        json_object *m = json_object_array_get_idx(marks, mi);
        const char *s = m != nullptr ? json_object_get_string(m) : nullptr;
        if (s == nullptr)
          continue;
        if (t->master_id == -1 && strcmp(s, mk_m) == 0)
          t->master_id = id;
        else if (t->stack_id == -1 && strcmp(s, mk_s) == 0)
          t->stack_id = id;
        else if (t->partner_id == -1 && strcmp(s, mk_p) == 0)
          t->partner_id = id;
      }
    }
  }
  if (is_tiled_with_nodes(n, nodes)) {
    if (t->n < TILED_MAX) {
      t->id[t->n] = id;
      t->parent[t->n] = parent;
      json_object *r = jget(n, "rect");
      t->rw[t->n] = (r != nullptr) ? jint(r, "width") : 0;
      t->n++;
    } else {
      static bool warned = false; /* одного раза за процесс хватит */
      if (!warned) {
        warned = true;
        fprintf(stderr,
                "sms: на столе больше %d тайлов — "
                "лишние не управляются\n",
                TILED_MAX);
      }
    }
  }
  if (nodes == nullptr) /* листья и контейнеры без children */
    return;
  size_t cnt = json_object_array_length(nodes);
  for (size_t i = 0; i < cnt; i++)
    collect(json_object_array_get_idx(nodes, i), n, mk_m, mk_s, mk_p, t);
}

/* индекс окна среди тайлов; -1 = его здесь нет */
static int idx_of(const Tiled *t, int64_t id) {
  if (id <= 0)
    return -1;
  for (int i = 0; i < t->n; i++)
    if (t->id[i] == id)
      return i;
  return -1;
}
