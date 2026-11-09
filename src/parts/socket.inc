/* socket.inc — Сокет, подписка и блокировка.
 *
 * Фрагмент единого translation unit: включается из main.c после sms.h.
 * Порядок включения повторяет прежний порядок объявлений в main.c.
 * Подключение к Sway, subscribe_events и flock sms.lock. */

#include "../sms.h"

/* ── Сокет, подписка и блокировка ── */

[[nodiscard]] static int sway_connect(void) {
  const char *path = getenv("SWAYSOCK");
  if (path == nullptr || path[0] == '\0')
    path = getenv("I3SOCK");
  if (path == nullptr || path[0] == '\0')
    return -1;
  int fd = socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0)
    return -1;
  struct sockaddr_un sa;
  memset(&sa, 0, sizeof sa);
  sa.sun_family = AF_UNIX;
  if (strlen(path) >= sizeof sa.sun_path) {
    close(fd);
    return -1;
  }
  strcpy(sa.sun_path, path);
  if (connect(fd, (struct sockaddr *)&sa, sizeof sa) < 0) {
    close(fd);
    return -1;
  }
  return fd;
}

[[nodiscard]] static bool subscribe_events(int fd) {
  const char payload[] = "[\"window\",\"workspace\"]";
  if (!ipc_send(fd, SUBSCRIBE, payload, sizeof payload - 1))
    return false;
  uint32_t type;
  size_t len;
  uint8_t *reply = ipc_recv(fd, &type, &len);
  if (reply == nullptr)
    return false;
  if (type != SUBSCRIBE) {
    free(reply);
    return false;
  }
  json_object *obj = parse_json_exact((const char *)reply, len);
  free(reply);
  if (obj == nullptr)
    return false;
  json_object *v = jget(obj, "success");
  bool ok = v != nullptr && json_object_get_boolean(v);
  json_object_put(obj);
  return ok;
}

static int g_lock_fd = -1;

[[nodiscard]] static bool grab_lock(void) {
  char path[384];
  const char *dir = getenv("XDG_RUNTIME_DIR");
  if (dir != nullptr && dir[0] != '\0' && strlen(dir) < sizeof path - 32)
    snprintf(path, sizeof path, "%s/sms.lock", dir);
  else
    snprintf(path, sizeof path, "/tmp/sms-%d.lock", (int)getuid());
  int fd = open(path, O_CREAT | O_RDWR | O_CLOEXEC | O_NOFOLLOW, 0600);
  if (fd < 0)
    return true;
  if (flock(fd, LOCK_EX | LOCK_NB) != 0) {
    close(fd);
    return false;
  }
  g_lock_fd = fd;
  return true;
}

static void close_lock(void) {
  if (g_lock_fd >= 0) {
    close(g_lock_fd);
    g_lock_fd = -1;
  }
}
