# Changelog

All notable changes to `sway-master-stack` are documented here.

---

## [2.0.0] — 2026-10-21

### English

#### Added
- **Zero-lag window spawn**: New windows are placed instantly before the first frame renders using table snapshots, eliminating intermediate window flicker.
- **Natural DWM Zoom & Return**:
  - `move left` on any stack window swaps it with Master and remembers where the old Master came from.
  - `move right` on Master returns it back to its original slot in the stack (or rotates with the top window).
- **Return memory (`_p<ws>`)**: The daemon uses lightweight container marks to remember swap origins across restarts without leaking memory.
- **Echo suppression cache**: The daemon tracks its own move commands and ignores self-induced events, breaking event loops.
- **Custom master ratio on launch**: Set master width on startup via CLI argument (`sway-master-stack 60`) or `SMS_PCT` environment variable (10–90%). Default: 55%.
- **Zero hijacked shortcuts**: The daemon requires no custom bindings or `nop` tricks in Sway config. Native arrows and move bindings work as-is.

#### Fixed
- **Tray & `for_window` race condition**: Tray apps (e.g. network/audio applets from Waybar) no longer kick tiled windows into floating mode. The daemon peeks into the socket buffer to let floating rules apply first.
- **Multi-monitor window warping**: Fixed a bug where closing the master window caused the last remaining window to fly over to the adjacent monitor.
- **PCRE2 regex bug**: Anchored workspace matching (`workspace="^name$"`) so workspace `1` never accidentally targets `10`–`18`.
- **Vertical stack collapse**: Eliminated nested container matryoshkas when toggling master between tiled and floating.

---

### Русский

#### Добавлено
- **Мгновенный спавн окон**: Новое окно встаёт на своё место до первого кадра отрисовки по сохранённому снимку стола. Окна больше не скачут и не мигают при открытии.
- **Точный DWM Зум и Возврат**:
  - `move left` на любом окне стопки делает его Мастером за один чистый свап.
  - `move right` на Мастере возвращает его ровно на то место в стопке, откуда он пришёл (или циклично меняет с верхним окном).
- **Память возврата (`_p<ws>`)**: Демон помнит, откуда пришёл Мастер, используя метки в дереве. Память не сбрасывается даже при рестарте демона.
- **Кэш эха**: Демон запоминает свои собственные перемещения и не тратит процессор на обработку собственного «эха».
- **Выбор пропорций при запуске**: Долю Мастера можно задать аргументом команды (`sway-master-stack 60` для 60%) или через переменную `SMS_PCT`. По умолчанию: 55%.
- **Полная свобода хоткеев**: Демону не нужны сторонние команды или перехваты клавиш в конфиге Sway. Ваши привычные стрелки перемещения работают напрямую.

#### Исправлено
- **Баг с апплетами из трея Waybar**: Открытие окна из трея больше не выбивает окна стопки в плавающий режим. Демон даёт правилам `for_window` отработать до тайлинга.
- **Улёт окон на соседний монитор**: Исправлен баг, когда при закрытии Мастера последнее оставшееся окно стопки уносило на соседний экран.
- **Ложное срабатывание регексов**: Имена столов теперь строго заякорены (`workspace="^1$"`), поэтому стол `1` больше не путается со столами `10`–`18`.
- **Схлопывание сетки в вертикальный столб**: Устранена проблема вложенных контейнеров при частом использовании `$mod+Space` на Мастере.
