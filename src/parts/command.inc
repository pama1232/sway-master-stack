/* command.inc — Сборка и отправка команд.
 *
 * Фрагмент единого translation unit: включается из main.c после sms.h.
 * Порядок включения повторяет прежний порядок объявлений в main.c.
 * Пачки через «;», критерии перед каждой командой, get_tree. */

#include "../sms.h"

/* ── Команды: пачка через «;», критерии перед КАЖДОЙ командой ── */

static bool cmd_append(char *buf, size_t cap, size_t *off, const char *cmd) {
  if (*off == 0) /* первая команда новой пачки — низкая граница эха */
    g_echo_batch_lo = g_echo_seq;
  size_t len = strlen(cmd);
  if (*off + len + 3 >= cap) {
    fprintf(stderr, "sms: команда не влезла в пачку: %s\n", cmd);
    return false; /* обрезанная пачка — лучше кричать */
  }
  if (*off > 0) {
    buf[(*off)++] = ';';
    buf[(*off)++] = ' ';
  }
  memcpy(buf + *off, cmd, len);
  *off += len;
  buf[*off] = '\0';
  return true;
}

/* структурная команда: меняет порядок/иерархию (swap/split/layout).
   Отделена от чисто меточных, чтобы retile_ws знал, можно ли сохранить
   снимок после пачки. */
static bool cmd_struct(char *buf, size_t cap, size_t *off, const char *cmd) {
  if (!cmd_append(buf, cap, off, cmd))
    return false;
  g_struct_sent++;
  return true;
}

/* move-команда: пачка + запоминание эха. Swap/mark/resize
   window::move не дают, поэтому их пачки идут через cmd_append/
   cmd_struct и контрольный reconcile им не нужен. */
static bool cmd_move(char *buf, size_t cap, size_t *off, const char *cmd,
                     int64_t id) {
  if (!cmd_struct(buf, cap, off, cmd))
    return false;
  echo_track(id);
  return true;
}

/* move, который обязан отработать сразу (переброс на соседний выход).
   Обычное эхо живёт 500мс; если Sway команду проигнорировал (no-op),
   такая запись провисит полсекунды и может съесть следующий жест
   юзера. Здесь хватает 100мс: событие приходит за миллисекунды,
   а no-op быстро протухает. */
static bool cmd_move_short(char *buf, size_t cap, size_t *off, const char *cmd,
                           int64_t id) {
  if (!cmd_struct(buf, cap, off, cmd))
    return false;
  echo_track_ttl(id, ECHO_TTL_SHORT_MS);
  return true;
}

static void send_cmd(int fd, const char *cmd) {
  if (g_debug)
    fprintf(stderr, "cmd: %s\n", cmd);
  if (!ipc_send(fd, RUN_COMMAND, cmd, strlen(cmd))) {
    echo_rollback(g_echo_batch_lo);
    return;
  }
  uint32_t type;
  uint8_t *reply = ipc_recv(
      fd, &type, nullptr); /* вычитать обязаны — не рассинхронить сокет */
  if (reply == nullptr) {
    echo_rollback(g_echo_batch_lo);
    return;
  }
  if (g_debug) /* отказы sway (критерий не сматчился) иначе не видны */
    fprintf(stderr, "reply: %s\n", (const char *)reply);
  /* Дешёвая проверка без парсера: sway печатает success с пробелом,
     но принимаем и компактную форму. Ложное срабатывание лишь
     откатит эхо — лишний ретайл, не поломка. */
  if (strstr((const char *)reply, "\"success\": false") != nullptr ||
      strstr((const char *)reply, "\"success\":false") != nullptr)
    echo_rollback(g_echo_batch_lo);
  free(reply);
}

[[nodiscard]] static json_object *get_tree(int fd) {
  for (int attempt = 0; attempt < 2; attempt++) {
    uint32_t type;
    size_t len;
    if (!ipc_send(fd, GET_TREE, nullptr, 0))
      return nullptr;
    uint8_t *buf = ipc_recv(fd, &type, &len);
    if (buf == nullptr)
      return nullptr;
    if (g_debug)
      fprintf(stderr, "tree: read\n");
    json_object *tree = parse_json_exact((const char *)buf, len);
    free(buf);
    if (tree != nullptr)
      return tree;
    /* пустой/битый ответ — одна повторная попытка */
  }
  return nullptr;
}


