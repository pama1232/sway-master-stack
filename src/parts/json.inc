/* json.inc — NULL-безопасные доступоры json-c.
 *
 * Фрагмент единого translation unit: включается из main.c после sms.h.
 * Порядок включения повторяет прежний порядок объявлений в main.c.
 * Лукапы полей, проверки типа/плавающего состояния и разбор дерева. */

#include "../sms.h"

/* ── NULL-безопасные доступоры (заимствованные ссылки) ── */

static json_object *jget(json_object *o, const char *key) {
  json_object *v = nullptr;
  /* json_object_object_get_ex сам проверяет тип: для не-объекта и NULL
     возвращает 0. Отдельный json_object_is_type — лишний вызов на
     каждый доступ к полю в горячем обходе дерева. */
  if (o != nullptr && json_object_object_get_ex(o, key, &v))
    return v;
  return nullptr;
}

static json_object *jarr(json_object *o, const char *key) {
  json_object *v = jget(o, key);
  return (v != nullptr && json_object_is_type(v, json_type_array)) ? v
                                                                   : nullptr;
}

static const char *jstr(json_object *o, const char *key) {
  json_object *v = jget(o, key);
  return v != nullptr ? json_object_get_string(v) : nullptr;
}

static int64_t jint(json_object *o, const char *key) {
  json_object *v = jget(o, key);
  return v != nullptr ? (int64_t)json_object_get_int64(v) : -1;
}

static bool jbool(json_object *o, const char *key) {
  json_object *v = jget(o, key);
  return v != nullptr && json_object_get_boolean(v);
}

static bool layout_is(json_object *n, const char *want) {
  const char *l = jstr(n, "layout");
  return l != nullptr && strcmp(l, want) == 0;
}

static bool is_floating(json_object *n) {
  const char *f = jstr(n, "floating");
  return f != nullptr &&
         (strcmp(f, "auto_on") == 0 || strcmp(f, "user_on") == 0);
}

/* Лист-«con» без детей, не плавающий и не в скретчпаде. nodes передаётся
   вызывающим: collect уже взял jarr(n,"nodes") — не ищем то же поле
   второй раз на каждом узле дерева. */
static bool is_tiled_with_nodes(json_object *n, json_object *nodes) {
  if (nodes != nullptr && json_object_array_length(nodes) > 0)
    return false;
  const char *type = jstr(n, "type");
  if (type == nullptr || strcmp(type, "con") != 0)
    return false;
  if (is_floating(n))
    return false;
  const char *scratch = jstr(n, "scratchpad_state");
  return scratch == nullptr || strcmp(scratch, "none") == 0;
}

/* Обёртка для unit-тестов: collect использует версию с готовым nodes. */
[[maybe_unused]] static bool is_tiled(json_object *n) {
  return is_tiled_with_nodes(n, jarr(n, "nodes"));
}


