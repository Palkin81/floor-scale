#pragma once
// Веб-страница экрана на порту 80: статус, поиск сетей, проверка пароля,
// окно настройки, приём зашифрованных кредов, сброс сети.
// Заменяет встроенный web_server ESPHome. Черновик, не проверен на железе.

#include <string>
#include <functional>
#include <cmath>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/time.h>
#include <time.h>
#include "esp_http_server.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "esp_event.h"
#include "esphome/components/wifi/wifi_component.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "wifi_probe.h"
#include "prov_crypto.h"
#include "prov_link.h"
#include "net_role.h"

namespace ui {

static const char *TAG_UI = "ui";

// ---- состояние ----
inline uint32_t window_until = 0;           // до какого момента (мс) открыто окно
// Фиксированный буфер: статус пишется из цикла ESPHome и читается из задачи httpd
inline char status_buf[160] = "Ожидание";
inline std::string prov_key;                // base64 ключ, задаётся из YAML
inline std::string setup_ssid;
inline std::string peer_ap_ip;
inline std::string peer_host;
inline std::function<void(const std::string &, const std::string &)> on_creds;
inline std::function<void()> on_reset;
// Перезагрузка по кнопке на странице. Задаётся из YAML: App.reboot()
inline std::function<void()> on_reboot;
// Время: нет часов с батареей и сети, поэтому страница передаёт его (Unix-секунды, UTC)
inline volatile long long clock_set_at = 0;  // 0 = время не задано
inline esphome::wifi::WiFiComponent *wifi_comp_ptr = nullptr;  // задаётся из YAML: id(wifi_comp)
inline SemaphoreHandle_t peer_status_lock = nullptr;
inline bool peer_check_done = false;
inline bool peer_check_applicable = false;
inline bool peer_link_state = false;
inline char peer_role_buf[64] = "";
struct PeerCandidateCache {
  uint8_t mac[6] = {0};
  uint32_t checked_at = 0;
  bool valid = false;
};
inline PeerCandidateCache peer_candidate_cache[ESP_WIFI_MAX_CONN_NUM] = {};

// ---- устройство ----
inline std::string device_kind = "screen";        // "screen" или "scale"
inline std::string device_title = "Экран";        // заголовок страницы
// Фрагмент JSON без ведущей запятой, например "\"live\":1.2". Задаётся из YAML.
inline std::function<std::string()> extra_status;
// Действия калибровки, задаются из YAML, возвращают текст для пользователя
inline std::function<std::string()> on_zero;
inline std::function<std::string(float)> on_calib;
inline std::function<std::string()> on_curve_reset;

inline uint32_t now_ms() { return (uint32_t) (esp_timer_get_time() / 1000); }

inline void open_window(uint32_t ms) { window_until = now_ms() + ms; }
inline void close_window() { window_until = 0; }
inline bool window_open() {
  return window_until != 0 && (int32_t) (now_ms() - window_until) < 0;
}
inline void set_status(const std::string &s) { snprintf(status_buf, sizeof(status_buf), "%s", s.c_str()); }

// Команды калибровки: httpd только ставит в очередь, выполняет цикл ESPHome (run_pending)
inline volatile bool cmd_pending = false;
inline std::string cmd_name;
inline float cmd_known = 0.0f;

inline void run_pending() {
  if (!cmd_pending) return;
  cmd_pending = false;
  std::string r;
  if (cmd_name == "zero" && on_zero) r = on_zero();
  else if (cmd_name == "calib" && on_calib) r = on_calib(cmd_known);
  else if (cmd_name == "curve_reset" && on_curve_reset) r = on_curve_reset();
  if (!r.empty()) set_status(r);
}

static const char PAGE[] = R"HTML(
<!doctype html>
<html lang="ru"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1,viewport-fit=cover">
<meta name="color-scheme" content="light dark">
<link rel="icon" href="data:,">
<title>Floor Scale</title>
<style>
:root{
--text:#16181d;--text2:rgba(22,24,29,.66);--text3:rgba(22,24,29,.4);
--glass:rgba(255,255,255,.46);--glass2:rgba(255,255,255,.8);--field:rgba(255,255,255,.6);
--stroke:rgba(255,255,255,.8);--stroke-in:rgba(255,255,255,.4);--sheen:.7;
--tabs:rgba(255,255,255,.66);
--sep:rgba(60,60,67,.13);--shadow:0 10px 32px rgba(40,60,130,.14);
--blue:#0a7aff;--green:#1fa84a;--red:#e5382d;--orange:#e08400;
--base:#eaf0fb;--b1:#b9d4ff;--b2:#e4c8ff;--b3:#ffd8bb;
}
@media (prefers-color-scheme:dark){:root{
--text:#f4f6fa;--text2:rgba(244,246,250,.68);--text3:rgba(244,246,250,.42);
--glass:rgba(36,40,58,.5);--glass2:rgba(30,34,50,.86);--field:rgba(255,255,255,.09);
--stroke:rgba(255,255,255,.17);--stroke-in:rgba(255,255,255,.06);--sheen:.22;
--tabs:rgba(26,30,46,.7);
--sep:rgba(255,255,255,.1);--shadow:0 12px 36px rgba(0,0,0,.42);
--blue:#4da3ff;--green:#3ddc6a;--red:#ff6a60;--orange:#ffb340;
--base:#090c17;--b1:#1c3d8c;--b2:#552d8e;--b3:#0e5a64;
}}
*{box-sizing:border-box;-webkit-tap-highlight-color:transparent}
html{-webkit-text-size-adjust:100%}
body{margin:0;min-height:100vh;color:var(--text);-webkit-font-smoothing:antialiased;
font:16px/1.4 -apple-system,BlinkMacSystemFont,"SF Pro Text","SF Pro Display","Helvetica Neue",system-ui,"Segoe UI",Roboto,sans-serif;
background:radial-gradient(70vmax 70vmax at 0% 0%,var(--b1),transparent 62%),radial-gradient(60vmax 60vmax at 100% 28%,var(--b2),transparent 62%),radial-gradient(70vmax 70vmax at 20% 100%,var(--b3),transparent 62%),var(--base);
background-attachment:fixed}
main{max-width:640px;margin:0 auto;padding:calc(20px + env(safe-area-inset-top)) 16px calc(120px + env(safe-area-inset-bottom))}
.glass{position:relative;background:var(--glass);border:1px solid var(--stroke);border-radius:26px;
box-shadow:var(--shadow),inset 0 1px 0 var(--stroke),inset 0 0 0 1px var(--stroke-in);
-webkit-backdrop-filter:blur(26px) saturate(170%);backdrop-filter:blur(26px) saturate(170%)}
.glass::before{content:"";position:absolute;inset:0;border-radius:inherit;pointer-events:none;opacity:var(--sheen);
background:linear-gradient(145deg,rgba(255,255,255,.4),rgba(255,255,255,0) 40%)}
.glass>*{position:relative}
@supports not ((backdrop-filter:blur(1px)) or (-webkit-backdrop-filter:blur(1px))){.glass{background:var(--glass2)}}
.top{display:flex;align-items:flex-end;justify-content:space-between;gap:12px;margin:4px 4px 20px}
h1{margin:0;font-size:34px;line-height:1.1;font-weight:700;letter-spacing:-.02em}
.sub{margin:5px 0 0;color:var(--text2);font-size:15px}
h2{margin:24px 8px 10px;font-size:13px;font-weight:600;letter-spacing:.06em;text-transform:uppercase;color:var(--text2)}
.chip{display:inline-flex;align-items:center;gap:8px;padding:8px 14px;border-radius:99px;font-size:13px;font-weight:600;white-space:nowrap;flex:none}
.dot{width:8px;height:8px;border-radius:50%;background:var(--text3)}
.chip.ok .dot{background:var(--green);box-shadow:0 0 10px var(--green)}
.chip.bad .dot{background:var(--red);box-shadow:0 0 10px var(--red)}
.chip.wait .dot{background:var(--orange);animation:pulse 1.2s ease-in-out infinite}
@keyframes pulse{50%{opacity:.3}}
.page{display:none}
.page.on{display:block;animation:in .32s ease}
@keyframes in{from{opacity:0;transform:translateY(10px)}to{opacity:1;transform:none}}
.card{padding:18px;margin-bottom:14px}
.hero{padding:28px 22px;text-align:center;margin-bottom:14px}
.lab{font-size:12px;font-weight:600;letter-spacing:.07em;text-transform:uppercase;color:var(--text2)}
.big{font-size:80px;font-weight:700;letter-spacing:-.045em;line-height:1.05;margin:8px 0 4px;font-variant-numeric:tabular-nums}
.big small{font-size:26px;font-weight:600;color:var(--text2);margin-left:8px;letter-spacing:0}
.mid{font-size:30px;font-weight:700;letter-spacing:-.02em;margin:10px 0 6px}
.hint{color:var(--text2);font-size:15px}
.grid{display:grid;grid-template-columns:1fr 1fr;gap:12px;margin-bottom:14px}
.tile{padding:16px 18px;min-width:0}
.tile .v{margin-top:7px;font-size:18px;font-weight:600;overflow-wrap:anywhere;display:flex;align-items:center;gap:10px}
.wide{grid-column:1/-1}
.list{padding:2px 18px;margin-bottom:14px}
.row{display:flex;align-items:center;justify-content:space-between;gap:16px;padding:14px 0;border-bottom:1px solid var(--sep)}
.row:last-child{border-bottom:0}
.row .k{color:var(--text2);flex:none}
.row .v{font-weight:600;text-align:right;overflow-wrap:anywhere;display:flex;align-items:center;gap:10px;justify-content:flex-end}
.btn{appearance:none;font:inherit;font-weight:600;font-size:17px;min-height:52px;padding:0 20px;width:100%;cursor:pointer;
display:flex;align-items:center;justify-content:center;gap:9px;color:var(--text);
background:var(--field);border:1px solid var(--stroke);border-radius:17px;
-webkit-backdrop-filter:blur(14px);backdrop-filter:blur(14px);transition:transform .12s,opacity .2s}
.btn:active{transform:scale(.975)}
.btn:disabled{opacity:.45;pointer-events:none}
.btn.pri{color:#fff;background:linear-gradient(180deg,#4aa3ff,#0a7aff);border-color:rgba(255,255,255,.38);box-shadow:0 8px 20px rgba(10,122,255,.34),inset 0 1px 0 rgba(255,255,255,.4)}
.btn.dng{color:var(--red)}
.btn svg{width:20px;height:20px}
.stack{display:grid;gap:10px}
.inp{width:100%;min-height:52px;padding:0 16px;font:inherit;font-size:17px;color:var(--text);background:var(--field);border:1px solid var(--stroke);border-radius:17px;outline:0}
.inp:focus{border-color:var(--blue);box-shadow:0 0 0 4px rgba(10,122,255,.22)}
.inp::placeholder{color:var(--text3)}
.steps{margin:0 0 4px;padding:0;list-style:none;counter-reset:s}
.steps li{counter-increment:s;position:relative;padding:0 0 14px 40px;color:var(--text2);font-size:15px}
.steps li::before{content:counter(s);position:absolute;left:0;top:-1px;width:28px;height:28px;border-radius:50%;background:var(--field);border:1px solid var(--stroke);color:var(--text);font-weight:700;font-size:14px;display:flex;align-items:center;justify-content:center}
.pill{display:inline-block;padding:7px 14px;margin:0 8px 8px 0;border-radius:99px;background:var(--field);border:1px solid var(--stroke);font-size:14px;font-weight:600}
.net{display:flex;align-items:center;gap:12px;width:100%;padding:15px 18px;border:0;border-bottom:1px solid var(--sep);background:none;color:inherit;font:inherit;text-align:left;cursor:pointer}
.net:last-child{border-bottom:0}
.net b{flex:1;font-weight:600;overflow:hidden;text-overflow:ellipsis;white-space:nowrap}
.net svg{width:18px;height:18px;flex:none;color:var(--text2);fill:none;stroke:currentColor;stroke-width:2;stroke-linecap:round;stroke-linejoin:round}
.net svg.cur{color:var(--green)}
.net:disabled{opacity:.5}
.bars{display:inline-flex;align-items:flex-end;gap:2px;height:16px;flex:none}
.bars i{width:4px;border-radius:2px;background:var(--text);opacity:.2}
.bars i:nth-child(1){height:5px}.bars i:nth-child(2){height:8px}.bars i:nth-child(3){height:12px}.bars i:nth-child(4){height:16px}
.bars i.on{opacity:.85}
.empty{padding:22px 18px;text-align:center;color:var(--text2)}
.ev{display:flex;gap:14px;padding:11px 0;border-bottom:1px solid var(--sep);font-size:14px}
.ev:last-child{border-bottom:0}
.ev time{color:var(--text3);font-variant-numeric:tabular-nums;flex:none}
.foot{text-align:center;color:var(--text3);font-size:13px;margin:22px 0 0}
.tabs{position:fixed;left:50%;transform:translateX(-50%);bottom:calc(14px + env(safe-area-inset-bottom));z-index:20;
display:flex;gap:4px;padding:6px;border-radius:32px;width:min(calc(100% - 28px),470px);background:var(--tabs)}
.tab{flex:1;display:flex;flex-direction:column;align-items:center;gap:3px;padding:9px 2px 8px;border:0;border-radius:26px;background:none;color:var(--text2);font:inherit;font-size:11px;font-weight:600;cursor:pointer;transition:background .2s,color .2s}
.tab svg{width:25px;height:25px;fill:none;stroke:currentColor;stroke-width:1.8;stroke-linecap:round;stroke-linejoin:round}
.tab[aria-selected=true]{color:var(--blue);background:rgba(10,122,255,.15)}
.toast{position:fixed;left:50%;top:calc(12px + env(safe-area-inset-top));z-index:60;width:min(calc(100% - 28px),520px);padding:14px 18px;border-radius:22px;
background:var(--glass2);font-weight:500;line-height:1.35;transform:translate(-50%,-160%);opacity:0;pointer-events:none;transition:transform .38s cubic-bezier(.2,.9,.3,1.1),opacity .25s}
.toast.on{transform:translate(-50%,0);opacity:1;pointer-events:auto}
.scrim{position:fixed;inset:0;z-index:50;display:none;align-items:flex-end;justify-content:center;background:rgba(10,14,28,.36);-webkit-backdrop-filter:blur(6px);backdrop-filter:blur(6px)}
.scrim.on{display:flex;animation:fade .2s}
@keyframes fade{from{opacity:0}}
.panel{width:min(100% - 16px,520px);margin:0 8px calc(8px + env(safe-area-inset-bottom));padding:24px 20px 20px;border-radius:32px;background:var(--glass2);animation:up .34s cubic-bezier(.2,.9,.3,1)}
@keyframes up{from{transform:translateY(46px);opacity:0}}
.panel h3{margin:0 0 6px;font-size:21px;letter-spacing:-.01em}
.panel p{margin:0 0 18px;color:var(--text2);font-size:15px}
.spin{width:36px;height:36px;border-radius:50%;border:3.5px solid var(--sep);border-top-color:var(--blue);animation:rot .8s linear infinite;margin:4px 0 16px}
@keyframes rot{to{transform:rotate(360deg)}}
.two{display:grid;grid-template-columns:1fr 1fr;gap:10px}
.eye{position:absolute;right:6px;top:6px;width:auto;min-height:40px;padding:0 12px;font-size:14px;border-radius:13px;color:var(--blue)}
.fld{position:relative;margin-bottom:12px}
.fld .inp{padding-right:100px}
@media (prefers-reduced-motion:reduce){*{animation:none!important;transition:none!important}}
</style></head><body>
<svg width="0" height="0" style="position:absolute" aria-hidden="true"><defs>
<symbol id="i-home" viewBox="0 0 24 24"><path d="M3.5 11.2 12 4l8.5 7.2M5.8 9.6V19a1 1 0 0 0 1 1h10.4a1 1 0 0 0 1-1V9.6"/><path d="M10 20v-5.2h4V20"/></symbol>
<symbol id="i-wifi" viewBox="0 0 24 24"><path d="M2.6 9.2a14 14 0 0 1 18.8 0M5.7 12.6a9.6 9.6 0 0 1 12.6 0M8.8 16a5.2 5.2 0 0 1 6.4 0"/><circle cx="12" cy="19.3" r="1" fill="currentColor"/></symbol>
<symbol id="i-cal" viewBox="0 0 24 24"><rect x="3.5" y="4" width="17" height="16" rx="4.5"/><path d="M7.8 11.2a4.5 4.5 0 0 1 8.4 0M12 11.4l2-2.6"/></symbol>
<symbol id="i-sys" viewBox="0 0 24 24"><path d="M4 6.5h8M17 6.5h3M4 12h3M12 12h8M4 17.5h9M18 17.5h2"/><circle cx="14.5" cy="6.5" r="2.3"/><circle cx="9.5" cy="12" r="2.3"/><circle cx="15.5" cy="17.5" r="2.3"/></symbol>
<symbol id="i-lock" viewBox="0 0 24 24"><rect x="5" y="10.5" width="14" height="9.5" rx="2.6"/><path d="M8.5 10.5V8a3.5 3.5 0 0 1 7 0v2.5"/></symbol>
<symbol id="i-check" viewBox="0 0 24 24"><path d="m5 12.5 4.5 4.5L19 7.5"/></symbol>
<symbol id="i-search" viewBox="0 0 24 24"><path d="M20 12a8 8 0 1 1-2.4-5.7M20 4v4.6h-4.6"/></symbol>
</defs></svg>
<main>
<header class="top">
<div><h1 id="title">Floor Scale</h1><p class="sub" id="sub">Подключение…</p></div>
<span class="chip glass" id="chip"><i class="dot"></i><span id="chiptxt">Загрузка</span></span>
</header>
<section class="page on" id="p-home">
<div class="glass hero" id="hero-scale" hidden>
<div class="lab">Вес сейчас</div>
<div class="big"><span id="live">—</span><small>кг</small></div>
<div class="hint">Последний результат: <b id="res">нет данных</b></div>
</div>
<div class="glass hero" id="hero-screen" hidden>
<div class="lab">Связь</div>
<div class="mid" id="hs-main">Проверяем…</div>
<div class="hint" id="hs-sub"></div>
</div>
<div class="grid">
<div class="glass tile"><div class="lab">Сеть</div><div class="v" id="h-net">—</div></div>
<div class="glass tile"><div class="lab">Сигнал</div><div class="v"><span id="h-bars"></span><span id="h-sig">—</span></div></div>
<div class="glass tile"><div class="lab">IP-адрес</div><div class="v" id="h-ip">—</div></div>
<div class="glass tile"><div class="lab">Режим пары</div><div class="v" id="h-role">—</div></div>
<div class="glass tile wide"><div class="lab">Статус</div><div class="v" id="h-st">—</div></div>
</div>
</section>
<section class="page" id="p-net">
<div class="glass list">
<div class="row"><span class="k">Сеть</span><span class="v" id="n-net">—</span></div>
<div class="row"><span class="k">IP-адрес</span><span class="v" id="n-ip">—</span></div>
<div class="row"><span class="k">Сигнал</span><span class="v"><span id="n-bars"></span><span id="n-sig">—</span></span></div>
</div>
<h2>Окно настройки</h2>
<div class="glass card">
<p class="hint" style="margin:0 0 14px" id="n-win">Окно закрыто. Пока оно закрыто, устройство не принимает новые настройки сети от соседа.</p>
<button class="btn" id="b-open"><span>Открыть на 5 минут</span></button>
</div>
<h2>Wi-Fi сети</h2>
<button class="btn pri" id="b-scan"><svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><use href="#i-search"/></svg><span>Искать сети</span></button>
<div class="glass list" id="list" style="margin-top:14px;padding:0" hidden></div>
</section>
<section class="page" id="p-cal">
<div class="glass hero">
<div class="lab">Вес сейчас</div>
<div class="big"><span id="c-live">—</span><small>кг</small></div>
</div>
<div class="glass card">
<ol class="steps">
<li>Уберите всё с платформы и нажмите «Обнулить».</li>
<li>Введите массу известного груза и нажмите «Калибровать», затем положите груз на платформу.</li>
<li>Повторите с другими грузами: можно сохранить до пяти точек, промежуточные значения считаются линейно.</li>
</ol>
<div class="stack">
<button class="btn" id="b-zero">Обнулить</button>
<input class="inp" id="known" type="number" inputmode="decimal" step="0.1" min="0.1" placeholder="Известный вес, кг" aria-label="Известный вес, кг">
<button class="btn pri" id="b-calib">Калибровать по грузу</button>
</div>
</div>
<h2>Точки калибровки</h2>
<div class="glass card">
<div id="curve"></div>
<div class="hint" id="curve-note" style="margin-bottom:14px"></div>
<button class="btn dng" id="b-curve">Сбросить точки</button>
</div>
</section>
<section class="page" id="p-sys">
<div class="glass list">
<div class="row"><span class="k">Устройство</span><span class="v" id="s-dev">—</span></div>
<div class="row"><span class="k">Режим пары</span><span class="v" id="s-role">—</span></div>
<div class="row"><span class="k">Второе устройство</span><span class="v" id="s-peer">—</span></div>
<div class="row"><span class="k">Окно настройки</span><span class="v" id="s-win">—</span></div>
<div class="row"><span class="k">Время устройства</span><span class="v" id="s-clock">—</span></div>
</div>
<h2>Сброс сети</h2>
<div class="glass card">
<p class="hint" style="margin:0 0 14px">Сбрасывает Wi-Fi на обоих устройствах, если второе отвечает. Оба перезагрузятся и заново найдут друг друга.</p>
<button class="btn dng" id="b-reset">Сбросить сеть</button>
</div>
<h2>Перезагрузка</h2>
<div class="glass card">
<p class="hint" style="margin:0 0 14px">Перезагружает только это устройство. Сосед не затрагивается, сеть сохраняется.</p>
<button class="btn" id="b-reboot">Перезагрузить</button>
</div>
<h2>События</h2>
<div class="glass list" id="events"></div>
<p class="foot">Floor Scale · веб-интерфейс устройства</p>
</section>
</main>
<nav class="tabs glass" role="tablist" aria-label="Разделы">
<button class="tab" role="tab" id="t-home" data-p="home" aria-selected="true"><svg viewBox="0 0 24 24"><use href="#i-home"/></svg>Главная</button>
<button class="tab" role="tab" id="t-net" data-p="net" aria-selected="false"><svg viewBox="0 0 24 24"><use href="#i-wifi"/></svg>Сеть</button>
<button class="tab" role="tab" id="t-cal" data-p="cal" aria-selected="false" style="display:none"><svg viewBox="0 0 24 24"><use href="#i-cal"/></svg>Калибровка</button>
<button class="tab" role="tab" id="t-sys" data-p="sys" aria-selected="false"><svg viewBox="0 0 24 24"><use href="#i-sys"/></svg>Система</button>
</nav>
<div class="toast glass" id="toast" role="status" aria-live="polite"></div>
<div class="scrim" id="scrim">
<div class="panel glass" id="s-pass" hidden>
<h3 id="ps-ssid">Сеть</h3>
<p>Введите пароль Wi-Fi. Мы проверим подключение и передадим сеть второму устройству.</p>
<div class="fld">
<input class="inp" id="ps-pass" type="password" placeholder="Пароль" maxlength="64" autocomplete="off" autocapitalize="off" spellcheck="false" aria-label="Пароль Wi-Fi">
<button class="btn eye" id="ps-eye" type="button">Показать</button>
</div>
<div class="two"><button class="btn" id="ps-cancel">Отмена</button><button class="btn pri" id="ps-go">Подключить</button></div>
</div>
<div class="panel glass" id="s-prog" hidden>
<div class="spin"></div>
<h3 id="pg-t">Подождите</h3>
<p id="pg-p" style="margin:0"></p>
</div>
<div class="panel glass" id="s-ask" hidden>
<h3 id="ask-t"></h3>
<p id="ask-p"></p>
<div class="two"><button class="btn" id="ask-no">Отмена</button><button class="btn" id="ask-ok">ОК</button></div>
</div>
</div>
<script>
const $=id=>document.getElementById(id);
const el=(t,c,x)=>{const e=document.createElement(t);if(c)e.className=c;if(x!=null)e.textContent=x;return e};
const clear=e=>{e.textContent=''};
const num=v=>v==null?'—':String(v).replace('.',',');
let S={},offline=0,isScale=false,busy=false,pend='',askRes=null,cur=null,tt,ready=false,lastSt='';
const events=[];
/* ---------- сообщения ---------- */
function logEv(t){
if(!t||(events[0]&&events[0].t===t))return;
events.unshift({t:t,at:new Date()});if(events.length>30)events.pop();renderEv();
}
function renderEv(){
const b=$('events');clear(b);
if(!events.length){b.append(el('div','empty','Пока нет событий'));return}
events.forEach(e=>{const r=el('div','ev');r.append(el('time',null,e.at.toLocaleTimeString('ru-RU')),el('span',null,e.t));b.append(r)});
}
function say(t,keep){
const s=$('toast');clearTimeout(tt);
if(!t){s.classList.remove('on');return}
s.textContent=t;s.classList.add('on');logEv(t);
if(!keep)tt=setTimeout(()=>s.classList.remove('on'),7000);
}
$('toast').onclick=()=>say('');
/* ---------- запросы ---------- */
async function api(u,o,ms){
const c=new AbortController(),id=setTimeout(()=>c.abort(),ms||8000);
try{const r=await fetch(u,Object.assign({signal:c.signal,cache:'no-store'},o));return await r.json()}
finally{clearTimeout(id)}
}
const post=(u,b,ms)=>api(u,b?{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:new URLSearchParams(b)}:{method:'POST'},ms);
function setBusy(b){busy=b;document.querySelectorAll('.btn,.net').forEach(x=>x.disabled=b)}
/* ---------- разделы ---------- */
const pages=['home','net','cal','sys'];
function go(p){
if(!pages.includes(p)||(p==='cal'&&!isScale))p='home';
pages.forEach(x=>{$('p-'+x).classList.toggle('on',x===p);$('t-'+x).setAttribute('aria-selected',x===p)});
scrollTo(0,0);
}
document.querySelectorAll('.tab').forEach(t=>t.onclick=()=>{location.hash=t.dataset.p});
addEventListener('hashchange',()=>go(location.hash.slice(1)));
/* ---------- отрисовка статуса ---------- */
function bars(r){
const n=isNaN(r)?0:r>=-55?4:r>=-67?3:r>=-75?2:r>=-85?1:0,b=el('span','bars');
b.setAttribute('aria-hidden','true');
for(let i=1;i<=4;i++){const x=document.createElement('i');if(i<=n)x.className='on';b.append(x)}
return b;
}
function put(id,node){const e=$(id);clear(e);if(node)e.append(node)}
function chip(c,t){$('chip').className='chip glass '+c;$('chiptxt').textContent=t}
function renderCurve(s){
const box=$('curve'),note=$('curve-note');clear(box);note.textContent='';
if(!s||s.indexOf('нет точек')===0){
note.textContent=s?'Точек пока нет ('+s.replace('нет точек, ','')+').':'Точек пока нет.';return;
}
s.split(' | ').forEach((p,i)=>box.append(el('span','pill',i===0?p.replace('ноль','Ноль:'):p.replace('.',','))));
}
function render(j){
isScale=j.kind==='scale';
$('t-cal').style.display=isScale?'':'none';
if(!ready){ready=true;go(location.hash.slice(1))}
const title=j.title||'Floor Scale';$('title').textContent=title;document.title=title;
$('sub').textContent=j.role||'Роль не определена';
const peerName=isScale?'Экран':'Весы';
let c='',t='Ожидание сети',main='Ожидание сети',hint=j.status||'',peer='Ожидание сети';
if(j.peer_applicable){
if(!j.peer_checked){c='wait';t='Проверка связи';main='Проверяем связь…';peer='Проверка…'}
else if(j.peer_link){c='ok';t='Связь есть';main=peerName+' на связи';peer='Подключено — '+(j.peer_role||'сосед')}
else{c='bad';t='Нет связи';main=peerName+' не найден';peer='Нет связи'}
}
chip(c,t);
$('hero-scale').hidden=!isScale;$('hero-screen').hidden=isScale;
$('live').textContent=num(j.live);$('c-live').textContent=num(j.live);
$('res').textContent=j.result==null?'нет данных':num(j.result)+' кг';
$('hs-main').textContent=main;$('hs-sub').textContent=hint;
const net=j.network&&j.network!=='нет'?j.network:'Не подключено';
const ip=j.ip&&j.ip!=='нет'?j.ip:'—';
const rssi=parseInt(j.signal,10),sig=isNaN(rssi)?'—':rssi+' dBm';
$('h-net').textContent=net;$('n-net').textContent=net;
$('h-ip').textContent=ip;$('n-ip').textContent=ip;
put('h-bars',bars(rssi));put('n-bars',bars(rssi));
$('h-sig').textContent=sig;$('n-sig').textContent=sig;
$('h-role').textContent=j.role||'не определён';
$('h-st').textContent=j.status||'—';
$('n-win').textContent=j.window?'Окно открыто: устройство принимает настройки сети от соседа.':'Окно закрыто. Пока оно закрыто, устройство не принимает новые настройки сети от соседа.';
$('s-dev').textContent=title;$('s-role').textContent=j.role||'не определён';
$('s-peer').textContent=peer;$('s-win').textContent=j.window?'Открыто':'Закрыто';
$('s-clock').textContent=j.clock_ts?new Date(j.clock_ts*1000).toLocaleString('ru-RU'):'не задано';
if(isScale)renderCurve(j.curve);
if(j.status&&j.status!==lastSt){lastSt=j.status;if(j.status!=='Ожидание')logEv(j.status)}
}
async function refresh(){
if(document.hidden)return;
try{S=await api('/status');offline=0;render(S);syncClock(S)}
catch(e){if(++offline>=2){chip('bad','Нет связи со страницей')}}
}
/* ---------- время ---------- */
let lastClockTry=0;
// У устройства нет часов с батареей и сети: время берём у телефона/ПК, на котором открыта страница
function syncClock(j){
const now=Math.floor(Date.now()/1000);
if(Math.abs((j.clock_ts||0)-now)<=5||Date.now()-lastClockTry<10000)return;
lastClockTry=Date.now();
post('/time',{ts:now}).catch(()=>{});
}
/* ---------- панели ---------- */
function show(id){
cur=id;document.querySelectorAll('.panel').forEach(p=>{p.hidden=p.id!==id});$('scrim').classList.add('on');
}
function closeSheet(v){
$('scrim').classList.remove('on');cur=null;
if(askRes){const r=askRes;askRes=null;r(v===true)}
}
$('scrim').onclick=e=>{if(e.target===e.currentTarget&&cur!=='s-prog')closeSheet(false)};
addEventListener('keydown',e=>{if(e.key==='Escape'&&cur&&cur!=='s-prog')closeSheet(false)});
function prog(t,p){$('pg-t').textContent=t;$('pg-p').textContent=p;show('s-prog')}
function ask(t,p,ok,danger){
return new Promise(res=>{
$('ask-t').textContent=t;$('ask-p').textContent=p;
const b=$('ask-ok');b.textContent=ok;b.className='btn '+(danger?'dng':'pri');
askRes=res;show('s-ask');
});
}
$('ask-ok').onclick=()=>closeSheet(true);
$('ask-no').onclick=()=>closeSheet(false);
/* ---------- действия ---------- */
async function openWin(){
if(busy)return;
try{await post('/open');say('Окно настройки открыто на 5 минут')}catch(e){say('Нет связи с устройством')}
refresh();
}
async function resetNet(){
if(busy)return;
if(!await ask('Сбросить сеть?','Wi-Fi будет сброшен на обоих устройствах. Они перезагрузятся и заново найдут друг друга.','Сбросить',true))return;
setBusy(true);prog('Сброс сети','Передаём команду второму устройству…');
try{const j=await post('/reset',null,20000);say(j.msg||'Сброс сети, перезагрузка…',true)}
catch(e){say('Устройство перезагружается…',true)}
setBusy(false);closeSheet();
}
async function scan(){
if(busy)return;
const box=$('list');box.hidden=false;clear(box);box.append(el('div','empty','Ищем сети…'));
setBusy(true);
try{
const n=await api('/scan',null,20000);
clear(box);
if(n.error){box.hidden=true;say('Ошибка поиска: '+n.error,true)}
else{
const best={};
n.forEach(x=>{if(x.ssid&&(!best[x.ssid]||x.rssi>best[x.ssid].rssi))best[x.ssid]=x});
const arr=Object.keys(best).map(k=>best[k]).sort((a,b)=>b.rssi-a.rssi);
if(!arr.length)box.append(el('div','empty','Сети не найдены'));
arr.forEach(addNet);
}
}catch(e){box.hidden=true;say('Нет ответа от устройства при поиске',true)}
setBusy(false);
}
function addNet(n){
const b=el('button','net');b.type='button';
b.append(el('b',null,n.ssid),bars(n.rssi));
const ic=document.createElementNS('http://www.w3.org/2000/svg','svg');ic.setAttribute('viewBox','0 0 24 24');
const u=document.createElementNS('http://www.w3.org/2000/svg','use');
if(S.network===n.ssid){u.setAttribute('href','#i-check');ic.setAttribute('class','cur')}
else if(!n.open)u.setAttribute('href','#i-lock');
if(u.getAttribute('href')){ic.append(u);b.append(ic)}
b.onclick=()=>{if(n.open)connect(n.ssid,'');else openPass(n.ssid)};
$('list').append(b);
}
function openPass(ssid){
pend=ssid;$('ps-ssid').textContent=ssid;
const i=$('ps-pass');i.value='';i.type='password';$('ps-eye').textContent='Показать';
show('s-pass');setTimeout(()=>i.focus(),260);
}
$('ps-eye').onclick=()=>{const i=$('ps-pass'),h=i.type==='password';i.type=h?'text':'password';$('ps-eye').textContent=h?'Скрыть':'Показать'};
$('ps-cancel').onclick=()=>closeSheet(false);
$('ps-go').onclick=()=>{const p=$('ps-pass').value;if(!p){$('ps-pass').focus();return}closeSheet();connect(pend,p)};
$('ps-pass').onkeydown=e=>{if(e.key==='Enter')$('ps-go').click()};
async function connect(ssid,pass){
setBusy(true);
prog('Проверяем «'+ssid+'»','Подключаемся к сети и проверяем пароль. Страница может ненадолго потерять связь — это нормально.');
try{const j=await post('/connect',{ssid:ssid,pass:pass},60000);say(j.msg,!j.ok)}
catch(e){say('Нет связи с устройством. Возможно, точка доступа перезапустилась: подождите минуту и обновите страницу.',true)}
setBusy(false);closeSheet();refresh();
}
async function zeroIt(){
if(busy)return;
try{const j=await post('/zero');say(j.msg)}catch(e){say('Нет связи с устройством')}
setTimeout(refresh,1500);
}
async function calibIt(){
if(busy)return;
const v=$('known').value;
if(!v){say('Укажите известный вес');$('known').focus();return}
try{const j=await post('/calib',{known:v});say(j.msg)}catch(e){say('Нет связи с устройством')}
setTimeout(refresh,1500);
}
async function rebootIt(){
if(busy)return;
if(!await ask('Перезагрузить?','Это устройство перезагрузится. Сосед не затрагивается.','Перезагрузить',true))return;
setBusy(true);prog('Перезагрузка','Устройство перезагружается…');
try{const j=await post('/reboot',null,5000);say(j.msg||'Перезагрузка',true)}
catch(e){say('Устройство перезагружается…',true)}
setBusy(false);closeSheet();
}
async function curveReset(){
if(busy)return;
if(!await ask('Сбросить точки?','Все точки калибровки будут удалены.','Сбросить',true))return;
try{const j=await post('/curve_reset');say(j.msg)}catch(e){say('Нет связи с устройством')}
setTimeout(refresh,1500);
}
$('b-open').onclick=openWin;$('b-reset').onclick=resetNet;$('b-scan').onclick=scan;$('b-reboot').onclick=rebootIt;
$('b-zero').onclick=zeroIt;$('b-calib').onclick=calibIt;$('b-curve').onclick=curveReset;
go(location.hash.slice(1));renderEv();refresh();
setInterval(refresh,5000);
document.addEventListener('visibilitychange',()=>{if(!document.hidden)refresh()});
</script></body></html>
)HTML";

inline std::string json_escape(const char *s) {
  std::string o;
  for (const char *p = s; *p; p++) {
    unsigned char c = (unsigned char) *p;
    if (c == '"' || c == '\\') { o += '\\'; o += (char) c; }
    else if (c < 0x20) { o += ' '; }
    else { o += (char) c; }
  }
  return o;
}

inline std::string url_decode(const std::string &s) {
  std::string o;
  for (size_t i = 0; i < s.size(); i++) {
    if (s[i] == '+') {
      o += ' ';
    } else if (s[i] == '%' && i + 2 < s.size()) {
      std::string hex = s.substr(i + 1, 2);
      o += (char) strtol(hex.c_str(), nullptr, 16);
      i += 2;
    } else {
      o += s[i];
    }
  }
  return o;
}

inline std::string form_value(const std::string &body, const char *key) {
  std::string k = std::string(key) + "=";
  size_t p;
  if (body.compare(0, k.size(), k) == 0) {
    p = 0;
  } else {
    p = body.find("&" + k);
    if (p == std::string::npos) return "";
    p += 1;
  }
  p += k.size();
  size_t e = body.find('&', p);
  return url_decode(body.substr(p, e == std::string::npos ? std::string::npos : e - p));
}

// Читает тело запроса целиком. Возвращает -1, если тело больше max_len или ошибка.
inline int recv_body(httpd_req_t *req, std::string &out, size_t max_len) {
  size_t total = req->content_len;
  if (total == 0 || total > max_len) return -1;
  out.assign(total, '\0');
  size_t got = 0;
  while (got < total) {
    int r = httpd_req_recv(req, &out[got], total - got);
    if (r <= 0) return -1;
    got += (size_t) r;
  }
  return (int) total;
}

// Число для JSON: null, если значения нет
inline std::string num_json(float v) {
  if (std::isnan(v)) return "null";
  char b[24];
  snprintf(b, sizeof(b), "%.1f", v);
  return b;
}

inline std::string local_role(std::string *connected_ssid = nullptr) {
  wifi_ap_record_t ap = {};
  if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
    char name[33] = {0};
    memcpy(name, ap.ssid, 32);
    if (connected_ssid != nullptr) *connected_ssid = name;
    return setup_ssid == name ? "Клиент (слейв)" : "Домашняя сеть";
  }
  wifi_mode_t mode = WIFI_MODE_NULL;
  if (esp_wifi_get_mode(&mode) == ESP_OK && (mode == WIFI_MODE_AP || mode == WIFI_MODE_APSTA))
    return "Точка доступа (мастер)";
  return "Ожидание сети";
}

inline bool read_identity(const std::string &ip, const std::string &expected_kind,
                          std::string &role) {
  std::string response;
  if (!prov_link::get_request("http://" + ip + "/peer_identity", response)) return false;
  const std::string kind_field = "\"kind\":\"" + expected_kind + "\"";
  if (response.find(kind_field) == std::string::npos) return false;
  const std::string key = "\"role\":\"";
  size_t pos = response.find(key);
  if (pos != std::string::npos) {
    pos += key.size();
    size_t end = response.find('"', pos);
    if (end != std::string::npos) role = response.substr(pos, end - pos);
  }
  return true;
}

inline bool resolve_peer_host(std::string &ip) {
  if (peer_host.empty()) return false;
  std::string name = peer_host;
  const std::string suffix = ".local";
  if (name.size() > suffix.size() &&
      name.compare(name.size() - suffix.size(), suffix.size(), suffix) == 0)
    name.resize(name.size() - suffix.size());
  esp_ip4_addr_t addr = {};
  if (mdns_query_a(name.c_str(), 500, &addr) != ESP_OK) return false;
  char value[16];
  snprintf(value, sizeof(value), IPSTR, IP2STR(&addr));
  ip = value;
  return true;
}

inline bool probe_peer(std::string &peer_role) {
  const std::string expected = device_kind == "scale" ? "screen" : "scale";
  const std::string role = local_role();
  if (role == "Клиент (слейв)")
    return !peer_ap_ip.empty() && read_identity(peer_ap_ip, expected, peer_role);
  if (role == "Домашняя сеть") {
    std::string ip;
    return resolve_peer_host(ip) && read_identity(ip, expected, peer_role);
  }
  if (role != "Точка доступа (мастер)") return false;

  // В режиме мастера ищем именно веб-морду второго устройства среди клиентов
  // DHCP, чтобы телефон, подключённый к той же AP, не считался соседом.
  wifi_sta_list_t stations = {};
  if (esp_wifi_ap_get_sta_list(&stations) != ESP_OK || stations.num == 0) return false;
  esp_netif_t *ap_netif = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
  if (ap_netif == nullptr) return false;
  esp_netif_pair_mac_ip_t pairs[ESP_WIFI_MAX_CONN_NUM] = {};
  const int count = stations.num < ESP_WIFI_MAX_CONN_NUM ? stations.num : ESP_WIFI_MAX_CONN_NUM;
  for (int i = 0; i < count; i++) memcpy(pairs[i].mac, stations.sta[i].mac, 6);
  if (esp_netif_dhcps_get_clients_by_mac(ap_netif, count, pairs) != ESP_OK) return false;
  const uint32_t now = now_ms();
  for (int i = 0; i < count; i++) {
    if (pairs[i].ip.addr == 0) continue;
    int cache_slot = -1;
    for (int j = 0; j < ESP_WIFI_MAX_CONN_NUM; j++) {
      if (peer_candidate_cache[j].valid &&
          memcmp(peer_candidate_cache[j].mac, pairs[i].mac, 6) == 0) {
        cache_slot = j;
        if ((uint32_t) (now - peer_candidate_cache[j].checked_at) < 15000) break;
        cache_slot = j;
        break;
      }
      if (!peer_candidate_cache[j].valid && cache_slot < 0) cache_slot = j;
    }
    if (cache_slot >= 0 && peer_candidate_cache[cache_slot].valid &&
        memcmp(peer_candidate_cache[cache_slot].mac, pairs[i].mac, 6) == 0 &&
        (uint32_t) (now - peer_candidate_cache[cache_slot].checked_at) < 15000)
      continue;
    char ip[16];
    snprintf(ip, sizeof(ip), IPSTR, IP2STR(&pairs[i].ip));
    if (read_identity(ip, expected, peer_role)) {
      if (cache_slot >= 0) peer_candidate_cache[cache_slot].valid = false;
      return true;
    }
    if (cache_slot < 0) cache_slot = i % ESP_WIFI_MAX_CONN_NUM;
    memcpy(peer_candidate_cache[cache_slot].mac, pairs[i].mac, 6);
    peer_candidate_cache[cache_slot].checked_at = now;
    peer_candidate_cache[cache_slot].valid = true;
  }
  return false;
}

// Проверяем соседа вне HTTP-сервера: иначе встречные запросы /status с двух
// страниц могут занять оба HTTP server task и взаимно ждать ответа.
inline void peer_monitor_task(void *) {
  while (true) {
    std::string role;
    const std::string own_role = local_role();
    const bool applicable = own_role == "Клиент (слейв)" ||
                            own_role == "Точка доступа (мастер)" ||
                            own_role == "Домашняя сеть";
    const bool linked = applicable && probe_peer(role);
    if (peer_status_lock != nullptr && xSemaphoreTake(peer_status_lock, pdMS_TO_TICKS(100)) == pdTRUE) {
      peer_link_state = linked;
      peer_check_applicable = applicable;
      peer_check_done = true;
      snprintf(peer_role_buf, sizeof(peer_role_buf), "%s", linked ? role.c_str() : "");
      xSemaphoreGive(peer_status_lock);
    }
    vTaskDelay(pdMS_TO_TICKS(own_role == "Домашняя сеть" ? 30000 : 5000));
  }
}

inline std::string identity_json() {
  return "{\"kind\":\"" + device_kind + "\",\"role\":\"" +
         json_escape(local_role().c_str()) + "\"}";
}

// Состояние сети и отдельная проверка доступности именно второго устройства.
inline std::string status_json() {
  std::string network = "нет", signal = "нет", ip = "нет", connected_ssid;
  wifi_ap_record_t ap = {};
  if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
    char name[33] = {0};
    memcpy(name, ap.ssid, 32);
    network = name;
    signal = std::to_string((int) ap.rssi) + " dBm";
    connected_ssid = name;
  }
  const char *ifkey = connected_ssid.empty() ? "WIFI_AP_DEF" : "WIFI_STA_DEF";
  esp_netif_t *netif = esp_netif_get_handle_from_ifkey(ifkey);
  esp_netif_ip_info_t ipi = {};
  if (netif != nullptr && esp_netif_get_ip_info(netif, &ipi) == ESP_OK && ipi.ip.addr != 0) {
    char b[20];
    snprintf(b, sizeof(b), IPSTR, IP2STR(&ipi.ip));
    ip = b;
  }
  std::string out = "{";
  out += "\"status\":\"" + json_escape(status_buf) + "\",";
  out += "\"role\":\"" + json_escape(local_role().c_str()) + "\",";
  out += "\"network\":\"" + json_escape(network.c_str()) + "\",";
  out += "\"ip\":\"" + ip + "\",";
  out += "\"signal\":\"" + signal + "\",";
  out += std::string("\"window\":") + (window_open() ? "true" : "false");
  // Текущее время устройства (Unix, UTC) или 0, если время ещё не задано страницей
  const long long clock_now = clock_set_at ? (long long) time(nullptr) : 0;
  out += ",\"clock_ts\":" + std::to_string(clock_now);
  out += ",\"kind\":\"" + device_kind + "\"";
  out += ",\"title\":\"" + json_escape(device_title.c_str()) + "\"";
  bool checked = false, applicable = false, linked = false;
  char peer_role[sizeof(peer_role_buf)] = "";
  if (peer_status_lock != nullptr && xSemaphoreTake(peer_status_lock, pdMS_TO_TICKS(20)) == pdTRUE) {
    checked = peer_check_done;
    applicable = peer_check_applicable;
    linked = peer_link_state;
    memcpy(peer_role, peer_role_buf, sizeof(peer_role));
    peer_role[sizeof(peer_role) - 1] = '\0';
    xSemaphoreGive(peer_status_lock);
  }
  out += std::string(",\"peer_checked\":") + (checked ? "true" : "false");
  out += std::string(",\"peer_applicable\":") + (applicable ? "true" : "false");
  out += std::string(",\"peer_link\":") + (linked ? "true" : "false");
  if (linked) out += ",\"peer_role\":\"" + json_escape(peer_role) + "\"";
  if (extra_status) out += "," + extra_status();
  out += "}";
  return out;
}

static esp_err_t h_root(httpd_req_t *req) {
  httpd_resp_set_type(req, "text/html; charset=utf-8");
  return httpd_resp_send(req, PAGE, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t h_status(httpd_req_t *req) {
  std::string s = status_json();
  httpd_resp_set_type(req, "application/json; charset=utf-8");
  return httpd_resp_send(req, s.c_str(), s.size());
}

static esp_err_t h_peer_identity(httpd_req_t *req) {
  const std::string s = identity_json();
  httpd_resp_set_type(req, "application/json");
  return httpd_resp_send(req, s.c_str(), s.size());
}

static esp_err_t send_scan_error(httpd_req_t *req, const char *where, esp_err_t e) {
  ESP_LOGE(TAG_UI, "%s failed: %s", where, esp_err_to_name(e));
  char err[160];
  snprintf(err, sizeof(err), "{\"error\":\"%s: %s\"}", where, esp_err_to_name(e));
  httpd_resp_set_type(req, "application/json; charset=utf-8");
  return httpd_resp_sendstr(req, err);
}

// ---- поиск сетей ----
// Результаты забираем в своём обработчике события SCAN_DONE: он срабатывает
// сразу после обработчика ESPHome, раньше его основного цикла, который
// тоже вычитывает список из драйвера (после чтения список пустеет).
inline SemaphoreHandle_t scan_sem = nullptr;
inline wifi_ap_record_t scan_recs[20];
inline uint16_t scan_cnt = 0;
inline uint32_t scan_evt_status = 0;
inline uint16_t scan_evt_num = 0;
inline volatile bool scan_wanted = false;
inline bool scan_hooked = false;

static void on_scan_done(void *, esp_event_base_t, int32_t, void *data) {
  if (!scan_wanted) return;  // чужие сканы (ESPHome) не трогаем
  auto *ev = (wifi_event_sta_scan_done_t *) data;
  scan_evt_status = ev->status;
  scan_evt_num = ev->number;
  scan_cnt = 20;
  if (esp_wifi_scan_get_ap_records(&scan_cnt, scan_recs) != ESP_OK) scan_cnt = 0;
  scan_wanted = false;
  if (scan_sem) xSemaphoreGive(scan_sem);
}

static esp_err_t h_scan(httpd_req_t *req) {
  if (!scan_hooked) {
    scan_sem = xSemaphoreCreateBinary();
    esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_SCAN_DONE, &on_scan_done, nullptr);
    scan_hooked = true;
  }

  // Сканирование не работает, когда клиентский интерфейс выключен (только точка доступа).
  // Включаем клиент без подключения: сохранённой сети нет, переподключений не будет.
  wifi_mode_t mode = WIFI_MODE_NULL;
  if (esp_wifi_get_mode(&mode) == ESP_OK && mode == WIFI_MODE_AP) {
    esp_err_t me = esp_wifi_set_mode(WIFI_MODE_APSTA);
    ESP_LOGI(TAG_UI, "mode AP -> APSTA for scan: %s", esp_err_to_name(me));
  }

  xSemaphoreTake(scan_sem, 0);  // сбросить старый сигнал
  scan_cnt = 0;
  scan_wanted = true;

  wifi_scan_config_t cfg = {};
  cfg.show_hidden = false;
  cfg.scan_type = WIFI_SCAN_TYPE_ACTIVE;
  cfg.scan_time.active.min = 100;
  cfg.scan_time.active.max = 300;
  esp_err_t e = esp_wifi_scan_start(&cfg, false);  // не блокирующий
  if (e != ESP_OK) {
    scan_wanted = false;
    return send_scan_error(req, "scan_start", e);
  }
  if (xSemaphoreTake(scan_sem, pdMS_TO_TICKS(10000)) != pdTRUE) {
    scan_wanted = false;
    return send_scan_error(req, "scan_timeout", ESP_ERR_TIMEOUT);
  }
  ESP_LOGI(TAG_UI, "scan event: status=%u number=%u fetched=%u",
           (unsigned) scan_evt_status, (unsigned) scan_evt_num, (unsigned) scan_cnt);

  std::string out = "[";
  bool first = true;
  for (int i = 0; i < scan_cnt; i++) {
    char name[33] = {0};
    memcpy(name, scan_recs[i].ssid, 32);
    if (name[0] == 0) continue;
    char item[256];
    snprintf(item, sizeof(item), "%s{\"ssid\":\"%s\",\"rssi\":%d,\"open\":%s}",
             first ? "" : ",", json_escape(name).c_str(), (int) scan_recs[i].rssi,
             scan_recs[i].authmode == WIFI_AUTH_OPEN ? "true" : "false");
    out += item;
    first = false;
  }
  out += "]";
  httpd_resp_set_type(req, "application/json; charset=utf-8");
  return httpd_resp_send(req, out.c_str(), out.size());
}

// Ответ для действий калибровки
static esp_err_t reply_msg(httpd_req_t *req, const std::string &m) {
  httpd_resp_set_type(req, "application/json; charset=utf-8");
  std::string r = "{\"ok\":true,\"msg\":\"" + json_escape(m.c_str()) + "\"}";
  return httpd_resp_send(req, r.c_str(), r.size());
}

// Ставит команду в очередь; выполнит цикл ESPHome, результат придёт в статус
static esp_err_t queue_cmd(httpd_req_t *req, const char *name, float known, bool supported) {
  if (!supported) return reply_msg(req, "Не поддерживается");
  if (cmd_pending) return reply_msg(req, "Предыдущая команда ещё выполняется");
  cmd_name = name;
  cmd_known = known;
  cmd_pending = true;
  return reply_msg(req, "Команда принята");
}

static esp_err_t h_zero(httpd_req_t *req) {
  return queue_cmd(req, "zero", 0.0f, (bool) on_zero);
}

static esp_err_t h_calib(httpd_req_t *req) {
  std::string b;
  if (recv_body(req, b, 200) < 0) return reply_msg(req, "Ошибка запроса");
  std::string v = form_value(b, "known");
  float known = (float) strtod(v.c_str(), nullptr);
  return queue_cmd(req, "calib", known, (bool) on_calib);
}

static esp_err_t h_curve_reset(httpd_req_t *req) {
  return queue_cmd(req, "curve_reset", 0.0f, (bool) on_curve_reset);
}

// Проверка сети и передача зашифрованных учётных данных соседу.
static esp_err_t h_connect(httpd_req_t *req) {
  std::string b;
  httpd_resp_set_type(req, "application/json; charset=utf-8");
  if (recv_body(req, b, 300) < 0) {
    return httpd_resp_sendstr(req, "{\"ok\":false,\"msg\":\"Ошибка запроса\"}");
  }
  std::string ssid = form_value(b, "ssid");
  std::string pass = form_value(b, "pass");
  if (ssid.empty()) {
    return httpd_resp_sendstr(req, "{\"ok\":false,\"msg\":\"Выберите сеть\"}");
  }
  // Клиент ли мы setup-сети, определяем по списку сетей ESPHome, а не по
  // конфигурации драйвера: во флеше драйвера может лежать устаревшая запись.
  std::string sta_ssid;
  const bool was_setup_client = netrole::saved_sta(sta_ssid) && setup_ssid.size() > 0 &&
      sta_ssid == setup_ssid;

  bool previous_network_restored = false;
  if (probe::try_connect(ssid, pass, 15000, &previous_network_restored)) {
    if (!previous_network_restored) {
      set_status("Сеть проверена, но связь с соседом не восстановилась");
      return httpd_resp_sendstr(req,
          "{\"ok\":false,\"msg\":\"Сеть подходит, но устройство не вернулось в setup-сеть. Проверьте связь с соседом и повторите\"}");
    }
    std::string blob;
    if (!prov::encrypt(prov_key.c_str(), ssid, pass, blob)) {
      set_status("Ошибка шифрования учётных данных");
      return httpd_resp_sendstr(req,
          "{\"ok\":false,\"msg\":\"Не удалось зашифровать учётные данные\"}");
    }

    const bool sent = was_setup_client
        ? prov_link::send_to_peer_ap(peer_ap_ip, blob)
        : prov_link::send_to_ap_clients(blob);
    if (!sent) {
      set_status("Сеть проверена, сосед не принял настройки");
      return httpd_resp_sendstr(req,
          "{\"ok\":false,\"msg\":\"Сеть работает, но сосед не найден или его окно настройки закрыто. Откройте окно на втором устройстве и повторите\"}");
    }

    set_status("Сосед принял настройки, подключение к сети");
    httpd_resp_sendstr(req,
        "{\"ok\":true,\"msg\":\"Сосед принял настройки. Устройство подключается к сети\"}");
    if (on_creds) on_creds(ssid, pass);
    return ESP_OK;
  }
  set_status("Не удалось подключиться, проверьте пароль");
  return httpd_resp_sendstr(req,
      "{\"ok\":false,\"msg\":\"Не удалось подключиться, проверьте пароль\"}");
}

static esp_err_t h_open(httpd_req_t *req) {
  open_window(300000);
  set_status("Окно настройки открыто на 5 минут");
  httpd_resp_set_type(req, "application/json");
  return httpd_resp_sendstr(req, "{\"ok\":true}");
}

static esp_err_t h_reset(httpd_req_t *req) {
  bool peer_reset = false;
  if (prov::is_home_network(setup_ssid.c_str())) {
    peer_reset = prov_link::reset_peer_at(peer_host);
  } else {
    std::string sta_ssid;
    const bool setup_client = netrole::saved_sta(sta_ssid) && setup_ssid.size() > 0 &&
        sta_ssid == setup_ssid;
    peer_reset = setup_client
        ? prov_link::reset_peer_at(peer_ap_ip)
        : prov_link::reset_ap_clients();
  }

  const char *msg = peer_reset
      ? "Сброс отправлен обоим устройствам, они перезагружаются"
      : "Сосед не ответил; сбрасывается только это устройство";
  set_status(msg);
  httpd_resp_set_type(req, "application/json; charset=utf-8");
  std::string body = std::string("{\"ok\":true,\"peer_reset\":") +
                     (peer_reset ? "true" : "false") +
                     ",\"msg\":\"" + msg + "\"}";
  httpd_resp_send(req, body.c_str(), body.size());
  vTaskDelay(pdMS_TO_TICKS(250));
  if (on_reset) on_reset();
  return ESP_OK;
}

// Перезагрузка по кнопке. Ответ уходит до перезагрузки; запись в журнал устройства.
static esp_err_t h_reboot(httpd_req_t *req) {
  httpd_resp_set_type(req, "application/json; charset=utf-8");
  if (!on_reboot) return reply_msg(req, "Перезагрузка не поддерживается");
  ESP_LOGW(TAG_UI, "reboot requested from web page");
  set_status("Перезагрузка по команде со страницы");
  httpd_resp_sendstr(req, "{\"ok\":true,\"msg\":\"Перезагрузка\"}");
  vTaskDelay(pdMS_TO_TICKS(250));
  on_reboot();
  return ESP_OK;
}

// Установка времени со страницы (поле ts: Unix-секунды, UTC). Принимаем только разумные значения.
static esp_err_t h_time(httpd_req_t *req) {
  std::string b;
  if (recv_body(req, b, 100) < 0) return reply_msg(req, "Ошибка запроса");
  const long long ts = strtoll(form_value(b, "ts").c_str(), nullptr, 10);
  if (ts < 1704067200LL || ts > 4102444800LL) return reply_msg(req, "Неверное время");
  struct timeval tv = {};
  tv.tv_sec = (time_t) ts;
  tv.tv_usec = 0;
  if (settimeofday(&tv, nullptr) != 0) return reply_msg(req, "Не удалось установить время");
  clock_set_at = ts;
  ESP_LOGI(TAG_UI, "clock set from web page: unix=%lld", ts);
  return reply_msg(req, "Время установлено");
}

// Команда от соседа. Отвечает до сброса и не пересылает команду обратно.
static esp_err_t h_peer_reset(httpd_req_t *req) {
  set_status("Сосед запросил сброс сети");
  httpd_resp_set_type(req, "application/json; charset=utf-8");
  httpd_resp_sendstr(req, "{\"ok\":true,\"msg\":\"Сброс принят\"}");
  vTaskDelay(pdMS_TO_TICKS(500));
  if (on_reset) on_reset();
  return ESP_OK;
}

// Приём зашифрованного блока кредов (hex в теле запроса)
static esp_err_t h_prov(httpd_req_t *req) {
  httpd_resp_set_type(req, "application/json; charset=utf-8");
  if (!window_open()) {
    set_status("Окно настройки закрыто. Откройте его кнопкой на странице");
    return httpd_resp_sendstr(req, "{\"ok\":false,\"msg\":\"Окно настройки закрыто\"}");
  }
  std::string blob;
  if (recv_body(req, blob, 300) < 0) {
    return httpd_resp_sendstr(req, "{\"ok\":false,\"msg\":\"Ошибка запроса\"}");
  }
  std::string ssid, pass;
  if (!prov::decrypt(prov_key.c_str(), blob, ssid, pass)) {
    set_status("Ошибка: неверный ключ или данные");
    return httpd_resp_sendstr(req, "{\"ok\":false,\"msg\":\"Неверный ключ или данные\"}");
  }
  set_status("Креды приняты, перезагрузка");
  close_window();
  httpd_resp_sendstr(req, "{\"ok\":true,\"msg\":\"Креды приняты\"}");
  if (on_creds) on_creds(ssid, pass);
  return ESP_OK;
}

// Регистрация маршрута с проверкой результата: при переполнении таблицы
// обработчиков (ESP_ERR_HTTPD_HANDLERS_FULL) маршрут иначе пропадает молча,
// и клиент получает HTTP 404.
inline void reg_uri(httpd_handle_t srv, const httpd_uri_t &u) {
  const esp_err_t err = httpd_register_uri_handler(srv, &u);
  if (err != ESP_OK) {
    ESP_LOGE(TAG_UI, "register %s failed: %s", u.uri, esp_err_to_name(err));
  }
}

inline void start(int port) {
  httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
  cfg.server_port = port;
  // По умолчанию 8 обработчиков; у нас 12 маршрутов, лишние тихо отбрасывались (404)
  cfg.max_uri_handlers = 16;
  // web_server ESPHome больше не используется, но порт управления оставляем отдельным
  cfg.ctrl_port = 32769;
  // Стек по умолчанию (4 КБ) переполняется в обработчике сканирования
  cfg.stack_size = 12288;
  httpd_handle_t srv = nullptr;
  if (httpd_start(&srv, &cfg) != ESP_OK) {
    ESP_LOGE(TAG_UI, "httpd start failed on port %d", port);
    return;
  }

  httpd_uri_t u = {};
  u.uri = "/"; u.method = HTTP_GET; u.handler = h_root;
  reg_uri(srv, u);

  u = {};
  u.uri = "/status"; u.method = HTTP_GET; u.handler = h_status;
  reg_uri(srv, u);

  u = {};
  u.uri = "/peer_identity"; u.method = HTTP_GET; u.handler = h_peer_identity;
  reg_uri(srv, u);

  if (peer_status_lock == nullptr) peer_status_lock = xSemaphoreCreateMutex();
  if (peer_status_lock != nullptr) {
    xTaskCreate(peer_monitor_task, "peer_monitor", 8192, nullptr, 1, nullptr);
  } else {
    ESP_LOGE(TAG_UI, "cannot create peer status mutex");
  }

  u = {};
  u.uri = "/scan"; u.method = HTTP_GET; u.handler = h_scan;
  reg_uri(srv, u);

  u = {};
  u.uri = "/connect"; u.method = HTTP_POST; u.handler = h_connect;
  reg_uri(srv, u);

  u = {};
  u.uri = "/open"; u.method = HTTP_POST; u.handler = h_open;
  reg_uri(srv, u);

  u = {};
  u.uri = "/reset"; u.method = HTTP_POST; u.handler = h_reset;
  reg_uri(srv, u);

  u = {};
  u.uri = "/peer_reset"; u.method = HTTP_POST; u.handler = h_peer_reset;
  reg_uri(srv, u);

  u = {};
  u.uri = "/prov"; u.method = HTTP_POST; u.handler = h_prov;
  reg_uri(srv, u);

  u = {};
  u.uri = "/zero"; u.method = HTTP_POST; u.handler = h_zero;
  reg_uri(srv, u);

  u = {};
  u.uri = "/calib"; u.method = HTTP_POST; u.handler = h_calib;
  reg_uri(srv, u);

  u = {};
  u.uri = "/curve_reset"; u.method = HTTP_POST; u.handler = h_curve_reset;
  reg_uri(srv, u);
  u = {};
  u.uri = "/reboot"; u.method = HTTP_POST; u.handler = h_reboot;
  reg_uri(srv, u);
  u = {};
  u.uri = "/time"; u.method = HTTP_POST; u.handler = h_time;
  reg_uri(srv, u);

  ESP_LOGI(TAG_UI, "UI started on port %d", port);
}

}  // namespace ui
