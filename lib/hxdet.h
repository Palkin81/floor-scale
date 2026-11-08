#pragma once
// Подсказка «подключён ли датчик HX711» (этап 12.3c). Это только подсказка для пользователя:
// роль автоматически не переключается. Отсчёты приходят из sensor.hx711 (on_value) через note().
// Датчика нет или он не отвечает: отсчётов нет совсем (компонент hx711 их не публикует).
// Провод замкнут или датчик завис: значения не меняются или упираются в предел 24 бит.
#include <cmath>
#include <cstdint>

#include "esp_timer.h"

namespace hxdet {

enum Code { kNoCap = 0, kNoData = 1, kOk = 2, kStuck = 3, kSaturated = 4 };

inline constexpr uint32_t kStaleMs = 5000;    // дольше нет отсчётов: данных нет
inline constexpr uint32_t kStuckRun = 30;     // столько одинаковых подряд (~3 с при 10 Гц): значение замерло
inline constexpr float kSatLimit = 8388000.0f;  // предел 24-битного отсчёта 8388607

inline bool capable = false;  // в этой прошивке есть HX711 (ставится в device_setup весов)
inline uint32_t last_ms = 0;
inline uint32_t total = 0;
inline uint32_t same_run = 0;
inline float last_val = NAN;
inline float ema_dt = 0.0f;  // средний интервал между отсчётами, мс
inline bool sat = false;

inline uint32_t now() { return (uint32_t) (esp_timer_get_time() / 1000); }

// Вызывается из on_value датчика (цикл ESPHome).
inline void note(float x) {
  if (std::isnan(x)) return;
  const uint32_t t = now();
  if (last_ms != 0) {
    const float dt = (float) (t - last_ms);
    ema_dt = ema_dt == 0.0f ? dt : ema_dt * 0.9f + dt * 0.1f;
  }
  same_run = (x == last_val) ? same_run + 1 : 0;
  last_val = x;
  last_ms = t;
  total = total + 1;
  sat = std::fabs(x) >= kSatLimit;
}

inline int code() {
  if (!capable) return kNoCap;
  if (last_ms == 0 || now() - last_ms > kStaleMs) return kNoData;
  if (sat) return kSaturated;
  if (same_run >= kStuckRun) return kStuck;
  return kOk;
}

// Частота отсчётов, десятые доли Гц (0, если отсчётов нет).
inline int rate_dhz() { return ema_dt > 1.0f ? (int) (10000.0f / ema_dt + 0.5f) : 0; }

inline const char *name(int c) {  // ASCII, для чёрного ящика
  switch (c) {
    case kNoCap: return "nocap";
    case kNoData: return "nodata";
    case kOk: return "ok";
    case kStuck: return "stuck";
    case kSaturated: return "sat";
  }
  return "?";
}

}  // namespace hxdet
