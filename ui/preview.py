#!/usr/bin/env python3
"""Собирает локальный предпросмотр страницы устройства: ui/page.html + имитация устройства (fetch).

Использование:  python3 ui/preview.py [выходной_файл]     (по умолчанию ui/preview.html, в git не входит)
Страница та же, что вшита в прошивку; данные придуманные: /status, /devcfg, полный тест, подсказка HX711
и перезагрузка при смене режима имитируются в браузере. Вверху справа кнопка ⚙: вид устройства, связь,
датчик, плата. Формат выхода: «тело» страницы без <html>/<head> (так её принимает публикация артефакта).
"""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent
src = (ROOT / "page.html").read_text(encoding="utf-8")

style = re.search(r"<style>(.*?)</style>", src, re.S).group(1)
body_all = src.split("</style></head><body>", 1)[1]
body_html = body_all.split("<script>", 1)[0]
scripts = re.findall(r"<script>(.*?)</script>", src, re.S)

# Темы: тёмная палитра и по системной настройке, и по явному data-theme (требование публикации)
m = re.search(r"@media \(prefers-color-scheme:dark\)\{:root\{(.*?)\}\}", style, re.S)
dark = m.group(1)
style = style.replace(
    m.group(0),
    "@media (prefers-color-scheme:dark){:root:not([data-theme=\"light\"]){" + dark + "color-scheme:dark}}\n"
    ":root[data-theme=\"dark\"]{" + dark + "color-scheme:dark}",
)

PREVIEW_CSS = """
.labbtn{position:fixed;top:calc(8px + env(safe-area-inset-top,0px));right:12px;z-index:90;width:44px;height:44px;border-radius:50%;border:1px solid var(--stroke);background:var(--glass2);color:var(--text);font-size:22px;line-height:1;cursor:pointer}
.labpanel{position:fixed;left:12px;right:12px;top:calc(60px + env(safe-area-inset-top,0px));z-index:91;max-width:560px;margin:0 auto;padding:16px 18px;border-radius:20px;border:1px solid var(--stroke);background:var(--glass2);box-shadow:var(--shadow);font-size:15px;color:var(--text)}
.labpanel h4{margin:0 0 10px;font-size:17px}
.labpanel .r{display:flex;align-items:center;justify-content:space-between;gap:12px;padding:6px 0}
.labpanel select{font:inherit;color:var(--text);background:var(--field);border:1px solid var(--stroke);border-radius:12px;padding:8px 10px;min-width:0;max-width:60%}
.labpanel p{margin:8px 0 0;color:var(--text2);font-size:13px}
"""

LAB_HTML = """
<button class="labbtn" id="lab-btn" type="button" aria-label="Настройки предпросмотра">⚙</button>
<div class="labpanel" id="lab" hidden>
  <h4>Предпросмотр: имитация устройства</h4>
  <div class="r"><label for="L-kind">Вид устройства</label><select id="L-kind"><option value="scale">Весы</option><option value="screen">Экран</option></select></div>
  <div class="r"><label for="L-link">Связь с соседом</label><select id="L-link"><option value="1">Есть</option><option value="0">Нет</option></select></div>
  <div class="r"><label for="L-hx">Датчик HX711</label><select id="L-hx"><option value="2">Отвечает</option><option value="1">Нет данных</option><option value="3">Значения замерли</option><option value="4">Упор в предел</option><option value="0">Нет в прошивке</option></select></div>
  <div class="r"><label for="L-chip">Плата</label><select id="L-chip"><option>ESP32-C6</option><option>ESP32</option></select></div>
  <p>Режим, «Применить на пробу», «Подтвердить», откат и полный тест работают через имитацию: смена режима на «Простой» после подтверждения «перезагружает» устройство на 4 секунды.</p>
</div>
"""

MOCK_JS = r"""
(function(){
  const M={kind:'scale',mode:0,peer_ip:'',peer_mac:'',peer_role:'',link:true,hx:2,chip:'ESP32-C6',ft:0,
    prev:null,trialEnd:0,downUntil:0,n:0,tl:[]};
  window.__M=M;
  const now=()=>Date.now();
  const left=()=>M.trialEnd>now()?Math.ceil((M.trialEnd-now())/1000):0;
  const tick=()=>{if(M.prev&&M.trialEnd&&M.trialEnd<=now()){Object.assign(M,M.prev);M.prev=null;M.trialEnd=0}};
  const cfg=()=>({ok:true,mode:M.mode,kind:M.kind,peer_ip:M.peer_ip,peer_mac:M.peer_mac,peer_role:M.peer_role,trial:left(),trial_total:180,fw:'12.3.0'});
  const scaleData=()=>({live:+(72.4+((M.n++)%5)/10).toFixed(1),result:71.9,curve:'ноль 8421543 | 10.0 кг | 40.0 кг | 80.0 кг'});
  const status=()=>{
    const simple=M.mode===1,sc=M.kind==='scale',linked=M.link&&!simple;
    const o={status:'Ожидание',role:simple?'Точка доступа (мастер)':'Домашняя сеть',network:'Xiaomi_1F6A',ip:'192.168.31.112',
      signal:(-62-(M.n%5))+' dBm',window:false,kind:M.kind,title:sc?'Весы':'Экран',proto:1,mac:'CC:8D:A2:F3:E2:B0',
      peer_checked:true,peer_applicable:!simple,peer_link:linked,clock_ts:Math.floor(now()/1000),
      ft:M.ft,mode:M.mode,trial:left(),chip:M.chip,hx:sc?M.hx:0,hx_dhz:M.hx===2&&sc?100:0};
    if(linked)o.peer_role='Домашняя сеть';
    if(sc)Object.assign(o,scaleData());else if(linked)o.scale=scaleData();
    return o;
  };
  const ftStep=()=>{M.tl.forEach(clearTimeout);M.tl=[];[[1,0],[2,6000],[3,12000],[0,18000]].forEach(([p,t])=>M.tl.push(setTimeout(()=>{M.ft=p},t)))};
  window.fetch=async function(u,o){
    u=String(u);o=o||{};
    await new Promise(r=>setTimeout(r,30+Math.random()*60));
    if(M.downUntil>now())throw new TypeError('Failed to fetch');
    tick();
    const p=new URLSearchParams(o.body||'');
    let d={ok:true,msg:'Команда принята (имитация)'};
    if(u.indexOf('/status')===0)d=status();
    else if(u.indexOf('/devcfg_confirm')===0){
      const changed=M.prev&&M.prev.mode!==M.mode;
      M.prev=null;M.trialEnd=0;
      if(changed){M.downUntil=now()+4000;d={ok:true,reboot:true,msg:'Режим изменён, устройство перезагружается'}}
      else d={ok:true,msg:'Настройки подтверждены'};
    }
    else if(u.indexOf('/devcfg_revert')===0){if(M.prev){Object.assign(M,M.prev);M.prev=null;M.trialEnd=0}d={ok:true,msg:'Прежние настройки возвращены'}}
    else if(u.indexOf('/devcfg')===0){
      if(o.method==='POST'){
        const nv={mode:p.get('mode')==='1'?1:0,peer_ip:p.get('peer_ip')||'',peer_mac:p.get('peer_mac')||'',peer_role:p.get('peer_role')||''};
        if(nv.peer_ip&&!/^\d{1,3}(\.\d{1,3}){3}$/.test(nv.peer_ip))d={ok:false,msg:'Неверный IP соседа (нужен вид 192.168.1.20)'};
        else if(nv.mode===M.mode&&nv.peer_ip===M.peer_ip&&nv.peer_mac===M.peer_mac&&nv.peer_role===M.peer_role)d={ok:true,msg:'Без изменений'};
        else{if(!M.prev)M.prev={mode:M.mode,peer_ip:M.peer_ip,peer_mac:M.peer_mac,peer_role:M.peer_role};Object.assign(M,nv);M.trialEnd=now()+180000;
          d={ok:true,msg:'Применено пробно. Подтвердите в течение 3 минут, иначе вернутся прежние'}}
      }else d=cfg();
    }
    else if(u.indexOf('/ft_start')===0){if(M.mode===1)d={ok:true,msg:'Недоступно в простом режиме: полному тесту нужен сосед'};else{ftStep();d={ok:true,msg:'Тест запущен на обоих устройствах'}}}
    else if(u.indexOf('/ft_cancel')===0){M.tl.forEach(clearTimeout);M.ft=0;d={ok:true,msg:'Тест остановлен'}}
    else if(u.indexOf('/test')===0){const n=p.get('name');d={ok:true,msg:n==='hx'?(M.kind==='scale'?'Тест датчика запущен (30 с), итог в чёрном ящике':'Тест датчика: в этой прошивке HX711 нет'):'Тест принят (имитация)'}}
    else if(u.indexOf('/events')===0)d={ev:[{i:1,t:Math.floor(now()/1000),l:'i',m:'Предпросмотр: данные имитированы'}],last:1};
    else if(u.indexOf('/scan')===0)d=[{ssid:'Xiaomi_1F6A',rssi:-57,open:false},{ssid:'DOMRU_9C32',rssi:-59,open:false}];
    else if(u.indexOf('/update_info')===0)d={ok:true,chip:M.chip,built:'09 Nov 2026, 01:30',running:'ota_0',next:'ota_1',max:1835008};
    else if(u.indexOf('/export')===0)d={ok:true,app:'floor-scale',v:1,kind:M.kind,blob:'ab'.repeat(40),net:true,cal:M.kind==='scale'};
    return {json:async()=>d,ok:true};
  };
  const $=id=>document.getElementById(id);
  $('lab-btn').onclick=()=>{$('lab').hidden=!$('lab').hidden};
  $('L-kind').onchange=e=>{M.kind=e.target.value};
  $('L-link').onchange=e=>{M.link=e.target.value==='1'};
  $('L-hx').onchange=e=>{M.hx=+e.target.value};
  $('L-chip').onchange=e=>{M.chip=e.target.value};
})();
"""

# Фоновая отладочная врезка страницы (#debug) в предпросмотре не нужна
page_js = [s for s in scripts if "Отладка по адресу #debug" not in s]

out = (
    "<title>Предпросмотр Floor Scale</title>\n"
    "<style>" + style + PREVIEW_CSS + "</style>\n"
    + body_html + LAB_HTML
    + "<script>" + MOCK_JS + "</script>\n"
    + "".join("<script>" + s + "</script>\n" for s in page_js)
)
dst = Path(sys.argv[1]) if len(sys.argv) > 1 else ROOT / "preview.html"
dst.write_text(out, encoding="utf-8")
print(f"Предпросмотр: {len(out)} байт -> {dst}")
