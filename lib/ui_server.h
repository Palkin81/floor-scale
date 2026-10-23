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
#include "esp_http_server.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "esp_event.h"
#include "esphome/components/wifi/wifi_component.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "wifi_probe.h"
#include "prov_crypto.h"

namespace ui {

static const char *TAG_UI = "ui";

// ---- состояние ----
inline uint32_t window_until = 0;           // до какого момента (мс) открыто окно
// Фиксированный буфер: статус пишется из цикла ESPHome и читается из задачи httpd
inline char status_buf[160] = "Ожидание";
inline std::string prov_key;                // base64 ключ, задаётся из YAML
inline std::function<void(const std::string &, const std::string &)> on_creds;
inline std::function<void()> on_reset;
inline esphome::wifi::WiFiComponent *wifi_comp_ptr = nullptr;  // задаётся из YAML: id(wifi_comp)

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
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Настройка сети</title>
<style>
body{font-family:system-ui,sans-serif;margin:0;padding:16px;background:#f4f5f7;color:#222;font-size:17px}
h1{font-size:22px;margin:8px 0 12px}
h2{font-size:18px;margin:20px 0 8px}
button{font-size:17px;padding:12px 16px;border:0;border-radius:10px;background:#2b6cb0;color:#fff;width:100%;cursor:pointer}
button:disabled{background:#9aa5b1}
button.danger{background:#c53030}
.card{background:#fff;border-radius:10px;padding:12px 14px;margin-bottom:10px}
.row{display:flex;justify-content:space-between;padding:6px 0;border-bottom:1px solid #eef0f3}
.row:last-child{border-bottom:0}
.row span:first-child{color:#666}
.btns{display:grid;gap:8px;margin-bottom:8px}
.msg{padding:12px 14px;margin:12px 0;border-radius:10px;background:#fff;display:none}
.box{margin-bottom:8px}
.net{background:#fff;color:#222;border:1px solid #d5d9e0;text-align:left;margin:6px 0 0 0}
.pass{display:none;background:#fff;padding:8px 10px;border:1px solid #d5d9e0;border-top:0;border-radius:0 0 10px 10px}
.pass input{font-size:17px;padding:12px;width:100%;box-sizing:border-box;border:1px solid #bbb;border-radius:10px;margin-bottom:8px}
</style></head><body>
<h1 id="title">Экран</h1>

<div class="card">
  <div class="row"><span>Статус</span><span id="st">...</span></div>
  <div class="row"><span>Сеть</span><span id="net">...</span></div>
  <div class="row"><span>IP</span><span id="ip">...</span></div>
  <div class="row"><span>Сигнал</span><span id="sig">...</span></div>
</div>

<div class="card" id="wcard" style="display:none">
  <div class="row"><span>Вес сейчас</span><span id="live">...</span></div>
  <div class="row"><span>Последний результат</span><span id="res">...</span></div>
  <div class="row"><span>Точки калибровки</span><span id="curve">...</span></div>
</div>

<div class="card" id="ccard" style="display:none">
  <div class="btns">
    <button onclick="zeroIt()">Обнулить</button>
    <input id="known" type="number" step="0.1" min="0.1" placeholder="Известный вес, кг" style="font-size:17px;padding:12px;width:100%;box-sizing:border-box;border:1px solid #bbb;border-radius:10px">
    <button onclick="calibIt()">Калибровать по грузу</button>
    <button class="danger" onclick="curveReset()">Сбросить точки</button>
  </div>
</div>

<div class="btns">
  <button id="openbtn" onclick="openWin()">Открыть окно настройки</button>
  <button id="resetbtn" class="danger" onclick="resetNet()">Сбросить сеть</button>
</div>

<h2>Поиск сетей</h2>
<button id="scan" onclick="scan()">Поиск сетей</button>
<div id="msg" class="msg"></div>
<div id="list"></div>

<script>
const $=id=>document.getElementById(id);
function say(t){const s=$('msg');s.textContent=t;s.style.display=t?'block':'none';}
function setBusy(b){document.querySelectorAll('button').forEach(x=>x.disabled=b);}

async function refresh(){
  try{
    const r=await fetch('/status'); const j=await r.json();
    $('st').textContent=j.status; $('net').textContent=j.network;
    $('ip').textContent=j.ip; $('sig').textContent=j.signal;
    if(j.title) $('title').textContent=j.title;
    const isScale=(j.kind==='scale');
    $('wcard').style.display=isScale?'block':'none';
    $('ccard').style.display=isScale?'block':'none';
    if(isScale){
      $('live').textContent=(j.live===null?'нет':j.live+' кг');
      $('res').textContent=(j.result===null?'нет':j.result+' кг');
      $('curve').textContent=j.curve||'нет';
    }
  }catch(e){}
}

async function openWin(){
  try{ await fetch('/open',{method:'POST'}); say('Окно настройки открыто на 5 минут'); }
  catch(e){ say('Нет связи с устройством'); }
  refresh();
}

async function resetNet(){
  if(!confirm('Сбросить сеть и перезагрузить устройство?')) return;
  setBusy(true); say('Сброс сети, перезагрузка...');
  try{ await fetch('/reset',{method:'POST'}); }catch(e){}
}

async function scan(){
  $('list').textContent='';
  setBusy(true); say('Поиск...');
  try{
    const r=await fetch('/scan');
    const nets=await r.json();
    if(nets.error){ say('Ошибка поиска: '+nets.error); }
    else{
      say(nets.length?'':'Сети не найдены');
      nets.forEach(addNet);
    }
  }catch(e){ say('Нет ответа от устройства при поиске'); }
  setBusy(false);
}

function addNet(n){
  const box=document.createElement('div'); box.className='box';
  const b=document.createElement('button'); b.className='net';
  b.textContent=n.ssid+'  ('+n.rssi+' dBm)'+(n.open?'':'  закрыта');
  const form=document.createElement('div'); form.className='pass';
  const inp=document.createElement('input'); inp.type='password'; inp.placeholder='Пароль';
  const go=document.createElement('button'); go.textContent='Подключить';
  form.append(inp,go);
  box.append(b,form);
  b.onclick=()=>{
    if(n.open){ connect(n.ssid,''); return; }
    form.style.display = form.style.display==='block' ? 'none' : 'block';
    if(form.style.display==='block') inp.focus();
  };
  go.onclick=()=>connect(n.ssid,inp.value);
  $('list').appendChild(box);
}

async function connect(ssid,pass){
  setBusy(true); say('Проверка подключения...');
  try{
    const r=await fetch('/connect',{method:'POST',
      headers:{'Content-Type':'application/x-www-form-urlencoded'},
      body:new URLSearchParams({ssid:ssid,pass:pass})});
    const j=await r.json();
    say(j.msg);
  }catch(e){ say('Нет связи с устройством. Возможно, точка доступа перезапустилась.'); }
  setBusy(false);
  refresh();
}

// Команды выполняются в цикле устройства, статус обновляем с небольшой задержкой
async function zeroIt(){
  try{ const r=await fetch('/zero',{method:'POST'}); const j=await r.json(); say(j.msg); }
  catch(e){ say('Нет связи с устройством'); }
  setTimeout(refresh,1500);
}
async function calibIt(){
  const v=$('known').value;
  if(!v){ say('Укажите известный вес'); return; }
  try{
    const r=await fetch('/calib',{method:'POST',
      headers:{'Content-Type':'application/x-www-form-urlencoded'},
      body:new URLSearchParams({known:v})});
    const j=await r.json(); say(j.msg);
  }catch(e){ say('Нет связи с устройством'); }
  setTimeout(refresh,1500);
}
async function curveReset(){
  if(!confirm('Сбросить все точки калибровки?')) return;
  try{ const r=await fetch('/curve_reset',{method:'POST'}); const j=await r.json(); say(j.msg); }
  catch(e){ say('Нет связи с устройством'); }
  setTimeout(refresh,1500);
}

refresh();
setInterval(refresh,5000);
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

// Состояние: режим, сеть, IP, сигнал, статус
inline std::string status_json() {
  std::string network = "нет", signal = "нет", ip = "нет", mode = "setup";
  wifi_ap_record_t ap;
  if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
    char name[33] = {0};
    memcpy(name, ap.ssid, 32);
    network = name;
    signal = std::to_string((int) ap.rssi) + " dBm";
    mode = "home";
  }
  const char *ifkey = (mode == "home") ? "WIFI_STA_DEF" : "WIFI_AP_DEF";
  esp_netif_t *netif = esp_netif_get_handle_from_ifkey(ifkey);
  esp_netif_ip_info_t ipi = {};
  if (netif != nullptr && esp_netif_get_ip_info(netif, &ipi) == ESP_OK && ipi.ip.addr != 0) {
    char b[20];
    snprintf(b, sizeof(b), IPSTR, IP2STR(&ipi.ip));
    ip = b;
  }
  std::string out = "{";
  out += "\"status\":\"" + json_escape(status_buf) + "\",";
  out += "\"mode\":\"" + mode + "\",";
  out += "\"network\":\"" + json_escape(network.c_str()) + "\",";
  out += "\"ip\":\"" + ip + "\",";
  out += "\"signal\":\"" + signal + "\",";
  out += std::string("\"window\":") + (window_open() ? "true" : "false");
  out += ",\"kind\":\"" + device_kind + "\"";
  out += ",\"title\":\"" + json_escape(device_title.c_str()) + "\"";
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

// Проверка пароля без передачи соседу
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
  if (probe::try_connect(ssid, pass, 15000)) {
    return httpd_resp_sendstr(req,
        "{\"ok\":true,\"msg\":\"Подключение успешно, креды готовы к отправке\"}");
  }
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
  set_status("Сброс сети, перезагрузка");
  httpd_resp_set_type(req, "application/json");
  httpd_resp_sendstr(req, "{\"ok\":true}");
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

inline void start(int port) {
  httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
  cfg.server_port = port;
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
  httpd_register_uri_handler(srv, &u);

  u = {};
  u.uri = "/status"; u.method = HTTP_GET; u.handler = h_status;
  httpd_register_uri_handler(srv, &u);

  u = {};
  u.uri = "/scan"; u.method = HTTP_GET; u.handler = h_scan;
  httpd_register_uri_handler(srv, &u);

  u = {};
  u.uri = "/connect"; u.method = HTTP_POST; u.handler = h_connect;
  httpd_register_uri_handler(srv, &u);

  u = {};
  u.uri = "/open"; u.method = HTTP_POST; u.handler = h_open;
  httpd_register_uri_handler(srv, &u);

  u = {};
  u.uri = "/reset"; u.method = HTTP_POST; u.handler = h_reset;
  httpd_register_uri_handler(srv, &u);

  u = {};
  u.uri = "/prov"; u.method = HTTP_POST; u.handler = h_prov;
  httpd_register_uri_handler(srv, &u);

  u = {};
  u.uri = "/zero"; u.method = HTTP_POST; u.handler = h_zero;
  httpd_register_uri_handler(srv, &u);

  u = {};
  u.uri = "/calib"; u.method = HTTP_POST; u.handler = h_calib;
  httpd_register_uri_handler(srv, &u);

  u = {};
  u.uri = "/curve_reset"; u.method = HTTP_POST; u.handler = h_curve_reset;
  httpd_register_uri_handler(srv, &u);

  ESP_LOGI(TAG_UI, "UI started on port %d", port);
}

}  // namespace ui
