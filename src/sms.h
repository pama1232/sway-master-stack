/* sms.h — общие настройки и состояние демона.
 *
 * Демон собирается как один translation unit: main.c включает этот
 * заголовок и фрагменты из parts/ в порядке объявлений. Здесь только
 * feature-макросы, системные include, тюнинг и общее состояние. */

#ifndef SMS_H
#define SMS_H

#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE 1 /* flock(2) — BSD, вне POSIX */

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdckdint.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#include <json-c/json.h>

/* ══════════════ ТЮНИНГ ══════════════ */
static int MASTER_PCT = 55; /* доля мастера, %; `sms N`, N = 10..90 */
constexpr int PCT_TOLERANCE = 3;
constexpr int TILED_MAX = 64;
constexpr int ECHO_MAX = 256; /* слотов кэша эха наших move-команд */
constexpr int WSNAME_MAX =
    96; /* имя ws в снимке; длиннее — слепой путь откажется */
constexpr int GUARD_MAX = 416; /* критерий: 2 байта на байт имени + обвязка */
constexpr int ONE_MAX =
    GUARD_MAX + 192; /* команда пачки: гвард + con_id + литерал */
constexpr int FAST_BATCH =
    ONE_MAX * 6 + 64;              /* слепая пачка — не больше 6 команд */
constexpr int RETILE_BATCH = 8192; /* буфер пачки retile_ws */

constexpr uint32_t IPC_MAX_LEN = 16u * 1024u * 1024u;
constexpr uint32_t IPC_EVENT = 0x80000000u;       /* бит события в типе;
                                                     ответ несёт тип запроса */
constexpr uint32_t EV_WORKSPACE = IPC_EVENT | 0u; /* focus → трекинг стола */
constexpr uint32_t EV_WINDOW = IPC_EVENT | 3u;

enum { RUN_COMMAND = 0, SUBSCRIBE = 2, GET_TREE = 4 };

static bool g_debug = false; /* SMS_DEBUG — фиксируется один раз в main */

/* Счётчик команд, меняющих порядок/иерархию дерева (move/swap/split/
   layout). Пачка только из меток/resize/focus порядок не трогает —
   снимок после неё остаётся верным (см. finish в retile_ws). */
static uint64_t g_struct_sent = 0;
#endif /* SMS_H */
