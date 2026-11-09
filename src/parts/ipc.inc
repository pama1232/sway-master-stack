/* ipc.inc — POSIX I/O и кадры Sway IPC.
 *
 * Фрагмент единого translation unit: включается из main.c после sms.h.
 * Порядок включения повторяет прежний порядок объявлений в main.c.
 * Полные циклы read/write, EINTR и упаковка/распаковка i3-ipc. */

#include "../sms.h"

/* ── POSIX I/O: полные циклы, EINTR ── */

[[nodiscard]] static bool write_all(int fd, const void *buf, size_t n) {
  const char *p = buf;
  while (n > 0) {
    ssize_t k = write(fd, p, n);
    if (k < 0) {
      if (errno == EINTR) {
        if (g_stop)
          return false; /* юзер остановил — бросаем кадр */
        continue;
      }
      return false;
    }
    if (k == 0)
      return false;
    p += (size_t)k;
    n -= (size_t)k;
  }
  return true;
}

/* writev_all: тот же EINTR/partial-контракт для iovec[] */
[[nodiscard]] static bool writev_all(int fd, struct iovec *iov, int iovcnt) {
  while (iovcnt > 0) {
    ssize_t k = writev(fd, iov, iovcnt);
    if (k < 0) {
      if (errno == EINTR) {
        if (g_stop)
          return false;
        continue;
      }
      return false;
    }
    if (k == 0)
      return false;
    while (iovcnt > 0 && (size_t)k >= iov[0].iov_len) {
      k -= (ssize_t)iov[0].iov_len;
      iov++;
      iovcnt--;
    }
    if (iovcnt > 0) {
      iov[0].iov_base = (char *)iov[0].iov_base + k;
      iov[0].iov_len -= (size_t)k;
    }
  }
  return true;
}

[[nodiscard]] static bool read_all(int fd, void *buf, size_t n) {
  char *p = buf;
  while (n > 0) {
    ssize_t k = read(fd, p, n);
    if (k < 0) {
      if (errno == EINTR) {
        if (g_stop)
          return false;
        continue;
      }
      return false;
    }
    if (k == 0)
      return false; /* EOF: sway ушёл */
    p += (size_t)k;
    n -= (size_t)k;
  }
  return true;
}

/* ── Sway IPC фрейм: "i3-ipc" + u32 len + u32 type, host endian ── */

/* Один writev на команду вместо двух write (заголовок + payload):
   на пачке спавна 5–6 команд — минус 5–6 syscall на событие. */
[[nodiscard]] static bool ipc_send(int fd, uint32_t type, const char *payload,
                                   size_t len) {
  if (len > UINT32_MAX)
    return false; /* протокол: длина payload — u32 */
  uint32_t payload_len = (uint32_t)len;
  uint32_t t = type;
  uint8_t h[14] = {'i', '3', '-', 'i', 'p', 'c'};
  memcpy(h + 6, &payload_len, 4);
  memcpy(h + 10, &t, 4);
  if (len == 0)
    return write_all(fd, h, 14);
  struct iovec iov[2];
  iov[0].iov_base = h;
  iov[0].iov_len = sizeof h;
  iov[1].iov_base = (void *)payload;
  iov[1].iov_len = len;
  return writev_all(fd, iov, 2);
}

/* NUL-терминированный malloc-буфер; при oversize payload ДРЕНИРУЕТСЯ,
 * чтобы сокет не рассинхронизировался. out_len — длина payload без NUL. */
[[nodiscard]] static uint8_t *ipc_recv(int fd, uint32_t *type,
                                       size_t *out_len) {
  uint8_t h[14];
  uint32_t len, t;
  if (!read_all(fd, h, 14))
    return nullptr;
  memcpy(&len, h + 6, 4);
  memcpy(&t, h + 10, 4);
  if (memcmp(h, "i3-ipc", 6) != 0)
    return nullptr;
  *type = t; /* тип — только валидному кадру */
  if (len > IPC_MAX_LEN) {
    uint8_t sink[65536]; /* 16× меньше read() на дренаже */
    while (len > 0) {
      size_t chunk = len < sizeof sink ? len : sizeof sink;
      if (!read_all(fd, sink, chunk))
        return nullptr;
      len -= (uint32_t)chunk;
    }
    return nullptr;
  }
  size_t total;
  if (ckd_add(&total, (size_t)len, (size_t)1))
    return nullptr;
  uint8_t *buf = malloc(total);
  if (buf == nullptr)
    return nullptr;
  if (len > 0 && !read_all(fd, buf, len)) {
    free(buf);
    return nullptr;
  }
  buf[len] = '\0';
  if (out_len != nullptr)
    *out_len = (size_t)len;
  return buf;
}

/* Точный парсер по известной длине: json_tokener_parse(s) внутри делает
   strlen(s) и зовёт parse_ex — у нас длина уже есть. */
[[nodiscard]] static json_object *parse_json_exact(const char *buf,
                                                   size_t len) {
  if (buf == nullptr)
    return nullptr;
  if (len > (size_t)INT_MAX)
    return nullptr; /* API json-c берёт int */
  json_tokener *tok = json_tokener_new();
  if (tok == nullptr)
    return nullptr;
  json_object *obj = json_tokener_parse_ex(tok, buf, (int)len);
  enum json_tokener_error err = json_tokener_get_error(tok);
  json_tokener_free(tok);
  if (err != json_tokener_success) {
    if (obj != nullptr)
      json_object_put(obj);
    return nullptr;
  }
  return obj;
}
