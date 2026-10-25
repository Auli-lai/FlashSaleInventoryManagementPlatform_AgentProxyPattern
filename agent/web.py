"""
库存运维中枢 —— FastAPI 可视化运维台（现代 / 简洁 / 大方，支持暗色主题）。

===========================================================================
启动
===========================================================================
  python main.py --web            # 默认 http://127.0.0.1:8000
  python main.py --web --dry-run  # 离线演示（不需要 Key / 引擎 / 网络）

或单独：cd agent && python web.py

===========================================================================
页面构成（单文件前端，无需打包工具）
===========================================================================
  · 顶部：产品名 + 引擎连接状态 + 运行模式/Key 状态 + 暗/亮主题切换
  · KPI 卡片：QPS / 连接数 / 累计命令 / 运行时长（运行时长客户端每秒刷新）
  · 左栏：商品信息（类别 / 库存数量 / 状态 / 秒杀价 …）
  · 中栏：实时执行轨迹（AI 思考逐字流出 + 工具调用，SSE 流式）
  · 右栏：运维对话（历史问答全部留存 + 结论逐字流出，SSE 流式）

===========================================================================
实现要点
===========================================================================
  · Agent 的 ReAct 循环改为 run_stream，逐事件吐出：
    thought_chunk / tool_result / answer_start|chunk|end …
    前端用 EventSource 接收并实时渲染，避免「一直等、看不到过程」。
  · 真实模式走 DeepSeek；若未配置 DEEPSEEK_API_KEY 会自动回退到演示模式，
    保证界面永远可用。DRY_RUN=1 强制演示。
  · 无 Key 时也能看完整「思考 → 调工具 → 结论」链路（假数据）。
"""

from __future__ import annotations

import json
import os
import sys
import threading
from queue import Empty, Queue

from fastapi import FastAPI, Request
from fastapi.responses import HTMLResponse, StreamingResponse, Response

_AGENT_DIR = os.path.dirname(os.path.abspath(__file__))
if _AGENT_DIR not in sys.path:
    sys.path.insert(0, _AGENT_DIR)

from agent import InventoryAgent  # noqa: E402
from agent import Session as AgentSession  # noqa: E402
from config import (  # noqa: E402
    DEEPSEEK_API_KEY, DEEPSEEK_BASE_URL, DEEPSEEK_MODEL,
    ENGINE_HOST, ENGINE_PORT, ENGINE_TIMEOUT,
)

app = FastAPI(title="库存运维中枢")

# 服务端多轮会话 + 历史（单机单进程，够用）。历史上限保护内存。
SESSION: AgentSession = AgentSession()
HISTORY: list[dict] = []
_HISTORY_LIMIT = 50


# ---------------------------------------------------------------------------
# 前端页面（现代 / 简洁 / 大方，亮色 + 暗色双主题）
# ---------------------------------------------------------------------------

_HTML = r"""<!DOCTYPE html>
<html lang="zh-CN" data-theme="light">
<head>
<meta charset="utf-8" />
<meta name="viewport" content="width=device-width, initial-scale=1" />
<title>库存运维中枢</title>
<link rel="icon" href="data:image/svg+xml,<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 32 32'><rect width='32' height='32' rx='8' fill='%234f46e5'/><text x='16' y='22' font-size='18' text-anchor='middle' fill='white' font-family='sans-serif'>库</text></svg>" />
<style>
  :root{
    --bg:#f5f6f8; --surface:#ffffff; --surface-2:#fafbfc; --line:#eceef2; --ink:#0f172a;
    --muted:#64748b; --faint:#94a3b8; --accent:#4f46e5; --accent-2:#7c74ff; --accent-soft:#eef0ff;
    --ok:#10b981; --warn:#f59e0b; --err:#ef4444; --radius:18px;
    --shadow:0 1px 2px rgba(15,23,42,.04), 0 12px 28px -12px rgba(15,23,42,.12);
    --glass:rgba(255,255,255,.82); --code-bg:#f3f4f8;
    --font:-apple-system,BlinkMacSystemFont,"Segoe UI",Roboto,"PingFang SC","Microsoft YaHei",sans-serif;
  }
  [data-theme="dark"]{
    --bg:#0f1115; --surface:#171a21; --surface-2:#1c2029; --line:#262b36; --ink:#e6e9ef;
    --muted:#9aa4b2; --faint:#5b6573; --accent:#7c83ff; --accent-2:#9aa0ff; --accent-soft:#1e2233;
    --ok:#34d399; --warn:#fbbf24; --err:#f87171;
    --shadow:0 1px 2px rgba(0,0,0,.3), 0 16px 32px -14px rgba(0,0,0,.6);
    --glass:rgba(23,26,33,.82); --code-bg:#0c0e13;
  }
  *{box-sizing:border-box;}
  html,body{margin:0;height:100%;}
  body{background:var(--bg);color:var(--ink);font-family:var(--font);
       -webkit-font-smoothing:antialiased;display:flex;flex-direction:column;transition:background .25s,color .25s;}
  a{color:var(--accent);text-decoration:none;}

  /* 顶栏 */
  header{display:flex;align-items:center;gap:14px;padding:16px 28px;
         background:var(--glass);backdrop-filter:blur(10px);
         border-bottom:1px solid var(--line);position:sticky;top:0;z-index:10;flex-wrap:wrap;}
  .logo{width:34px;height:34px;border-radius:10px;background:linear-gradient(135deg,var(--accent),var(--accent-2));
        display:flex;align-items:center;justify-content:center;color:#fff;font-weight:700;}
  .title{font-size:17px;font-weight:650;letter-spacing:.2px;}
  .subtitle{font-size:12.5px;color:var(--muted);margin-top:1px;}
  .spacer{flex:1;}
  .pill{font-size:12px;color:var(--muted);background:var(--surface);border:1px solid var(--line);
        padding:6px 12px;border-radius:999px;display:flex;align-items:center;gap:7px;white-space:nowrap;}
  .dot{width:9px;height:9px;border-radius:50%;background:var(--faint);transition:.3s;}
  .dot.on{background:var(--ok);box-shadow:0 0 0 4px rgba(16,185,129,.16);}
  .dot.off{background:var(--err);box-shadow:0 0 0 4px rgba(239,68,68,.16);}
  .theme-btn{font-family:inherit;font-size:12.5px;cursor:pointer;border:1px solid var(--line);
        background:var(--surface);color:var(--ink);padding:7px 14px;border-radius:999px;transition:.15s;}
  .theme-btn:hover{border-color:var(--accent);color:var(--accent);}

  /* KPI */
  .kpis{display:grid;grid-template-columns:repeat(4,1fr);gap:16px;padding:22px 28px 4px;}
  .kpi{background:var(--surface);border:1px solid var(--line);border-radius:var(--radius);
       padding:16px 20px;box-shadow:var(--shadow);}
  .kpi .lab{display:flex;align-items:center;gap:8px;color:var(--muted);font-size:12.5px;}
  .kpi .lab svg{width:17px;height:17px;stroke:var(--accent);fill:none;stroke-width:1.8;}
  .kpi .num{font-size:27px;font-weight:680;margin-top:8px;letter-spacing:-.5px;font-variant-numeric:tabular-nums;}
  .kpi .hint{font-size:11.5px;color:var(--faint);margin-top:2px;}

  /* 主体三栏 */
  main{flex:1;display:grid;grid-template-columns:minmax(300px,1fr) minmax(320px,1.05fr) minmax(340px,1.15fr);
       gap:16px;padding:16px 28px 28px;min-height:0;}
  .card{background:var(--surface);border:1px solid var(--line);border-radius:var(--radius);
        box-shadow:var(--shadow);display:flex;flex-direction:column;min-height:0;overflow:hidden;}
  .card h3{margin:0;padding:14px 16px;font-size:14px;font-weight:620;border-bottom:1px solid var(--line);
           display:flex;align-items:center;gap:8px;}
  .card h3 .tag{margin-left:auto;font-size:11px;font-weight:500;color:var(--faint);}

  /* 商品信息 */
  .prod-body{padding:12px;overflow:auto;flex:1;display:flex;flex-direction:column;gap:10px;}
  .pcard{border:1px solid var(--line);border-radius:14px;padding:12px 14px;background:var(--surface-2);}
  .pcard .top{display:flex;align-items:center;justify-content:space-between;gap:8px;}
  .pcard .name{font-weight:600;font-size:13.5px;}
  .pcard .meta{font-size:11.5px;color:var(--muted);margin-top:3px;}
  .badge{font-size:11px;padding:2px 9px;border-radius:999px;font-weight:600;white-space:nowrap;}
  .badge.ok{color:var(--ok);background:color-mix(in srgb,var(--ok) 14%,transparent);}
  .badge.err{color:var(--err);background:color-mix(in srgb,var(--err) 14%,transparent);}
  .badge.warn{color:var(--warn);background:color-mix(in srgb,var(--warn) 16%,transparent);}
  .pcard .grid{display:grid;grid-template-columns:repeat(2,1fr);gap:7px;margin-top:9px;}
  .pcell{background:var(--bg);border:1px solid var(--line);border-radius:10px;padding:6px 9px;}
  .pcell .lab{font-size:10.5px;color:var(--faint);}
  .pcell .val{font-size:14px;font-weight:620;font-variant-numeric:tabular-nums;margin-top:1px;}

  /* 执行轨迹 */
  .trace-body{padding:14px 16px;overflow:auto;flex:1;font-size:13px;}
  .tl{position:relative;padding-left:18px;margin-bottom:14px;}
  .tl::before{content:"";position:absolute;left:5px;top:4px;bottom:-14px;width:2px;background:var(--line);}
  .tl:last-child::before{display:none;}
  .tl .node{position:absolute;left:0;top:3px;width:12px;height:12px;border-radius:50%;
            background:var(--surface);border:2px solid var(--accent);}
  .tl.tool .node{border-color:var(--warn);}
  .tl.err .node{border-color:var(--err);}
  .tl.final .node{border-color:var(--ok);background:var(--ok);}
  .tl .ttl{font-weight:600;color:var(--ink);font-size:12.5px;}
  .tl .body{color:var(--muted);margin-top:3px;line-height:1.55;white-space:pre-wrap;word-break:break-word;}
  .tl pre{max-height:130px;overflow:auto;background:var(--code-bg);border:1px solid var(--line);
          border-radius:10px;padding:9px 11px;margin:6px 0 0;font-size:11px;color:var(--muted);white-space:pre-wrap;}

  /* 对话 */
  .chat{display:flex;flex-direction:column;min-height:0;}
  .banner{font-size:12px;color:var(--warn);background:color-mix(in srgb,var(--warn) 12%,transparent);
          border-bottom:1px solid var(--line);padding:9px 16px;line-height:1.5;}
  .chips{display:flex;flex-wrap:wrap;gap:8px;padding:12px 16px;border-bottom:1px solid var(--line);}
  .chip{font-size:12.5px;color:var(--accent);background:var(--accent-soft);border:1px solid #dfe2ff;
        padding:6px 12px;border-radius:999px;cursor:pointer;transition:.15s;}
  .chip:hover{background:#e3e6ff;}
  .log{flex:1;overflow:auto;padding:16px;display:flex;flex-direction:column;gap:14px;}
  .bubble{max-width:92%;padding:11px 14px;border-radius:14px;line-height:1.6;font-size:13.5px;white-space:pre-wrap;word-break:break-word;}
  .bubble.user{align-self:flex-end;background:var(--accent);color:#fff;border-bottom-right-radius:4px;}
  .bubble.ai{align-self:flex-start;background:var(--surface-2);border:1px solid var(--line);border-bottom-left-radius:4px;}
  .bubble.ai h2{font-size:14.5px;margin:12px 0 6px;color:var(--accent);}
  .bubble.ai h3{font-size:13.5px;margin:10px 0 5px;}
  .bubble.ai code{background:var(--code-bg);padding:1px 6px;border-radius:6px;font-size:12px;}
  .bubble.ai pre{background:var(--code-bg);border:1px solid var(--line);padding:10px;border-radius:10px;overflow:auto;}
  .bubble.ai ul{padding-left:18px;margin:7px 0;}
  .bubble.ai blockquote{margin:8px 0;padding:7px 12px;border-left:3px solid var(--accent);
                    background:var(--accent-soft);border-radius:0 8px 8px 0;color:var(--muted);}
  .bubble.err{align-self:flex-start;background:color-mix(in srgb,var(--err) 12%,transparent);border:1px solid color-mix(in srgb,var(--err) 40%,transparent);color:var(--err);}
  .carea{display:flex;gap:10px;padding:12px 16px;border-top:1px solid var(--line);background:var(--surface);}
  #q{flex:1;border:1px solid var(--line);background:var(--bg);border-radius:12px;
     padding:12px 15px;font-size:14px;outline:none;font-family:inherit;transition:.15s;color:var(--ink);}
  #q:focus{border-color:var(--accent);box-shadow:0 0 0 3px var(--accent-soft);}
  button{font-family:inherit;font-size:14px;border:none;cursor:pointer;border-radius:12px;}
  .send{background:var(--accent);color:#fff;padding:0 20px;font-weight:600;}
  .send:hover{filter:brightness(.94);}
  .ghost{background:var(--surface);border:1px solid var(--line);color:var(--muted);padding:0 14px;}

  @media(max-width:1200px){
    .kpis{grid-template-columns:repeat(2,1fr);}
    main{grid-template-columns:1fr;}
    .trace-body{max-height:300px;}
    .log{max-height:420px;}
  }
</style>
</head>
<body>
<header>
  <div class="logo">库</div>
  <div>
    <div class="title">库存运维中枢</div>
    <div class="subtitle">FlashSale 自研引擎 · 自然语言运维台</div>
  </div>
  <div class="spacer"></div>
  <div class="pill"><span class="dot" id="dot"></span><span id="statusText">连接中…</span></div>
  <div class="pill" id="cfg">模式加载中…</div>
  <button class="theme-btn" id="themeBtn">暗色</button>
</header>

<div class="kpis">
  <div class="kpi"><div class="lab"><svg viewBox="0 0 24 24"><path d="M3 12h4l3 8 4-16 3 8h4"/></svg>实时 QPS</div>
    <div class="num" id="kpiQps">—</div><div class="hint">最近 5 秒滑动估算</div></div>
  <div class="kpi"><div class="lab"><svg viewBox="0 0 24 24"><path d="M5 12a7 7 0 0114 0M8 12a4 4 0 018 0M12 12v3"/></svg>连接数</div>
    <div class="num" id="kpiConn">—</div><div class="hint">当前在线客户端</div></div>
  <div class="kpi"><div class="lab"><svg viewBox="0 0 24 24"><path d="M4 6h16M4 12h16M4 18h10"/></svg>累计命令</div>
    <div class="num" id="kpiCmd">—</div><div class="hint">自启动以来</div></div>
  <div class="kpi"><div class="lab"><svg viewBox="0 0 24 24"><path d="M12 7v5l3 2M12 3a9 9 0 109 9 9 9 0 00-9-9z"/></svg>运行时长</div>
    <div class="num" id="kpiUp">—</div><div class="hint">客户端逐秒刷新</div></div>
</div>

<main>
  <div class="card">
    <h3>商品信息 <span class="tag" id="prodTag">加载中…</span></h3>
    <div class="prod-body" id="products"><div class="placeholder" style="color:var(--faint)">读取引擎中的商品数据…</div></div>
  </div>

  <div class="card">
    <h3>实时执行轨迹 <span class="tag" id="traceTag">等待提问</span></h3>
    <div class="trace-body" id="trace"><div class="placeholder" style="color:var(--faint)">提问后，Agent 的思考与工具调用会逐字实时出现在这里。</div></div>
  </div>

  <div class="card chat">
    <h3>运维对话 <span class="tag">历史问答全部留存</span></h3>
    <div class="banner" id="banner" style="display:none"></div>
    <div class="chips" id="chips"></div>
    <div class="log" id="log">
      <div class="bubble ai">向 Agent 提问，例如：「stock:1 还剩多少库存，会不会超卖？」<br/>或点上方建议问题。结论与思考过程会实时流出。</div>
    </div>
    <div class="carea">
      <input id="q" placeholder="用自然语言描述你的运维问题…（Enter 发送）" />
      <button class="send" id="send">发送</button>
      <button class="ghost" id="dry">演示模式</button>
    </div>
  </div>
</main>

<script>
let dryRun = false;            // 实际由 /api/config 初始化
let engineStartMs = null;      // 由 STATS.uptime_sec 推算，客户端逐秒刷新
const $ = id => document.getElementById(id);
const dot=$('dot'), statusText=$('statusText');

const SUGGEST = [
  'stock:1 还剩多少库存，会不会超卖？',
  '现在 QPS 多少？热点 key 有哪些？',
  'DECR 返回负数是什么意思？',
  '怎么防止秒杀超卖？',
  'AOF 积压怎么处理？'
];
SUGGEST.forEach(s=>{const c=document.createElement('span');c.className='chip';c.textContent=s;
  c.onclick=()=>ask(s);$('chips').appendChild(c);});

/* ── 主题切换 ── */
function applyTheme(t){
  document.documentElement.dataset.theme = t;
  localStorage.setItem('theme', t);
  $('themeBtn').textContent = (t==='dark' ? '亮色' : '暗色');
}
$('themeBtn').onclick = ()=> applyTheme(document.documentElement.dataset.theme==='dark'?'light':'dark');
applyTheme(localStorage.getItem('theme') || 'light');

/* ── 状态 ── */
function setStatus(ok){ dot.className='dot '+(ok?'on':'off'); statusText.textContent = ok?'引擎已连接':'引擎未连接'; }
function setCfg(j){
  if(j.mode==='demo'){
    $('cfg').textContent = j.fell_back ? '演示模式（未检测到 Key）' : '演示模式';
    if(j.fell_back){
      const b=$('banner'); b.style.display='block';
      b.textContent='未检测到 DEEPSEEK_API_KEY，已自动切换到演示模式（无需联网/Key）。配置 Key 并重启后用真实模型。';
    }
  } else {
    $('cfg').textContent = '真实模式 · '+j.model;
  }
}

/* ── 轮询 STATS ── */
async function pollStats(){
  try{
    const r=await fetch('/api/stats');const j=await r.json();
    if(j.ok){
      setStatus(true);
      const d=j.data||{};
      $('kpiQps').textContent=(d.qps_estimate??'—');
      $('kpiConn').textContent=(d.connections??'—');
      $('kpiCmd').textContent=(d.total_commands??'—').toLocaleString();
      if(typeof d.uptime_sec==='number'){ engineStartMs = Date.now() - d.uptime_sec*1000; }
      $('kpiUp').textContent=fmtUp((engineStartMs!=null?(Date.now()-engineStartMs)/1000:0));
    } else { setStatus(false); }
  }catch(e){ setStatus(false); }
}
function fmtUp(s){s=Math.floor(Number(s)||0);if(s<60)return s+'s';if(s<3600)return (s/60)+'m';
  if(s<86400)return (s/3600)+'h'+((s%3600)/60)+'m';return (s/86400)+'d';}
// 运行时长：客户端每秒平滑刷新（不再等轮询）
setInterval(()=>{ if(engineStartMs!=null){ $('kpiUp').textContent=fmtUp((Date.now()-engineStartMs)/1000); } }, 1000);

/* ── 商品信息 ── */
async function pollProducts(){
  try{
    const r=await fetch('/api/products');const j=await r.json();
    if(j.ok){ renderProducts(j.data||[]); $('prodTag').textContent=(j.count||0)+' 件'; }
    else { $('prodTag').textContent='读取失败'; }
  }catch(e){ $('prodTag').textContent='读取失败'; }
}
function badge(status_kind){ return '<span class="badge '+status_kind+'">'; }
function renderProducts(list){
  const el=$('products');
  if(!list.length){ el.innerHTML='<div class="placeholder" style="color:var(--faint)">暂无商品数据（用 Qt 连接引擎后会自动播种种子数据）。</div>'; return; }
  el.innerHTML = list.map(p=>{
    const st = p.status_kind||'warn';
    const price = (p.price!=null&&p.price!=='')?'¥'+p.price:'—';
    const fprice = (p.flash_price!=null&&p.flash_price!=='')?'¥'+p.flash_price:'—';
    const bstock = (p.base_stock!=null)?p.base_stock:'—';
    const fleft = (p.flash_left!=null)?p.flash_left:'—';
    return `<div class="pcard">
      <div class="top"><span class="name">${esc(p.name||p.id)}</span>${badge(st)}${esc(p.status||'—')}</span></div>
      <div class="meta">${esc(p.category||'—')} · ${esc(p.brand||'—')} · ID ${esc(p.id)}</div>
      <div class="grid">
        <div class="pcell"><div class="lab">原价</div><div class="val">${price}</div></div>
        <div class="pcell"><div class="lab">秒杀价</div><div class="val">${fprice}</div></div>
        <div class="pcell"><div class="lab">普通库存</div><div class="val">${bstock}</div></div>
        <div class="pcell"><div class="lab">秒杀剩余</div><div class="val">${fleft}</div></div>
      </div>
    </div>`;
  }).join('');
}

/* ── 对话历史（过往问答全部留存）── */
function addUserBubble(text){
  const d=document.createElement('div');d.className='bubble user';d.textContent=text;$('log').appendChild(d);
  scrollLog(); return d;
}
function addAiBubble(html){
  const d=document.createElement('div');d.className='bubble ai';d.innerHTML=html;$('log').appendChild(d);
  scrollLog(); return d;
}
function scrollLog(){ const l=$('log'); l.scrollTop=l.scrollHeight; }

async function loadHistory(){
  try{
    const r=await fetch('/api/history');const j=await r.json();
    if(j.ok && Array.isArray(j.data)){
      if(j.data.length){
        // 保留开场提示，清空其余
        $('log').innerHTML='';
        j.data.forEach(h=>{
          addUserBubble(h.question);
          addAiBubble(h.answer ? md(h.answer) : '<span style="color:var(--faint)">（无结论）</span>');
        });
      }
    }
  }catch(e){}
}

/* ── 执行轨迹（实时思考 / 工具调用）── */
let currentThink=null;
function tracePlaceholderClear(){ const ph=$('trace').querySelector('.placeholder');if(ph)ph.remove(); }
function addTL(cls,title,initialBody){
  tracePlaceholderClear();
  const wrap=document.createElement('div');wrap.className='tl '+cls;
  const node=document.createElement('span');node.className='node';wrap.appendChild(node);
  const tt=document.createElement('div');tt.className='ttl';tt.textContent=title;wrap.appendChild(tt);
  const bd=document.createElement('div');bd.className='body';bd.textContent=initialBody||'';wrap.appendChild(bd);
  $('trace').appendChild(wrap);$('trace').scrollTop=$('trace').scrollHeight;
  return {wrap,tt,bd};
}
function ensureThink(round){
  if(currentThink && currentThink._round===round) return currentThink;
  currentThink=addTL('thought','思考 · 第'+round+'轮','');
  currentThink._round=round; return currentThink;
}
function md(t){
  return (t||'').replace(/```([\s\S]*?)```/g,(m,c)=>'<pre>'+esc(c)+'</pre>')
    .replace(/^### (.*)$/gm,'<h3>$1</h3>')
    .replace(/^## (.*)$/gm,'<h2>$1</h2>')
    .replace(/\*\*(.*?)\*\*/g,'<b>$1</b>')
    .replace(/^\s*[-*] (.*)$/gm,'<li>$1</li>')
    .replace(/(<\/li>)(?=<li>)/g,'$1')
    .replace(/<li>[\s\S]*?<\/li>/g,m=>'<ul>'+m+'</ul>')
    .replace(/\n/g,'<br/>');
}
function esc(s){return (s==null?'':String(s)).replace(/[&<>"]/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;'}[c]));}

/* ── 提问（SSE 流式）── */
function ask(q){
  addUserBubble(q);
  const ab=addAiBubble('<span style="color:var(--faint)">思考中…</span>');
  let abRaw=''; let abLive=false;
  currentThink=null;
  $('traceTag').textContent='进行中';
  const params='question='+encodeURIComponent(q)+'&dry_run='+(dryRun?'1':'0');
  const es=new EventSource('/api/chat?'+params);

  es.addEventListener('round_start',e=>{ const d=JSON.parse(e.data); ensureThink(d.round); });
  es.addEventListener('thought_chunk',e=>{ const d=JSON.parse(e.data); const t=ensureThink(d.round); t.bd.textContent+=d.text; });
  es.addEventListener('tool_result',e=>{
    const p=JSON.parse(e.data);
    const cls='tool'+(p.is_error?' err':'');
    let body=p.raw||'';
    if(p.intervention) body='⛔ '+p.intervention+'\n'+body;
    addTL(cls,'工具：'+p.name+(p.intervention?'（已拦截）':''),body);
  });
  es.addEventListener('answer_start',e=>{
    abLive=true; ab.innerHTML='';
    if(currentThink){ currentThink.tt.textContent='结论'; currentThink.wrap.classList.add('final'); }
  });
  es.addEventListener('answer_chunk',e=>{ const d=JSON.parse(e.data); if(abLive){ abRaw+=d.text; ab.textContent=abRaw; scrollLog(); } });
  es.addEventListener('answer_end',e=>{ const d=JSON.parse(e.data); ab.innerHTML=md(d.text); scrollLog(); $('traceTag').textContent='完成'; });
  es.addEventListener('error_event',e=>{ const d=JSON.parse(e.data);
    ab.className='bubble err'; ab.textContent='出错了：'+d.message; scrollLog(); $('traceTag').textContent='出错'; });
  es.addEventListener('final',e=>{ $('traceTag').textContent='完成'; });
  es.addEventListener('finish',e=>{ $('traceTag').textContent='完成'; });
  es.addEventListener('done',()=>{ es.close(); });
}

$('send').onclick=()=>{const v=$('q').value.trim();if(v){ask(v);$('q').value='';}};
$('q').addEventListener('keydown',e=>{if(e.key==='Enter'){const v=$('q').value.trim();if(v){ask(v);$('q').value='';}}});
$('dry').onclick=()=>{dryRun=!dryRun;$('dry').textContent=dryRun?'真实模式':'演示模式';};

/* ── 初始化 ── */
(async ()=>{
  try{ const r=await fetch('/api/config');const j=await r.json(); dryRun=(j.mode==='demo'); setCfg(j);
        $('dry').textContent=dryRun?'真实模式':'演示模式'; }catch(e){}
  await loadHistory();
  pollStats(); pollProducts();
  setInterval(pollStats,4000);
  setInterval(pollProducts,5000);
})();
</script>
</body>
</html>"""


# ---------------------------------------------------------------------------
# 引擎状态接口
# ---------------------------------------------------------------------------

@app.get("/api/stats")
async def api_stats() -> dict:
    try:
        from resp_client import RespClient
        client = RespClient(ENGINE_HOST, ENGINE_PORT, ENGINE_TIMEOUT)
        data = client.stats(10)
        client.close()
        return {"ok": True, "data": data}
    except Exception as exc:  # noqa: BLE001
        return {"ok": False, "error": str(exc)}


@app.get("/api/config")
async def api_config() -> dict:
    env_dry = os.environ.get("DRY_RUN", "false").lower() in ("1", "true", "yes")
    fell_back = (not env_dry) and not DEEPSEEK_API_KEY
    mode = "demo" if (env_dry or fell_back) else "real"
    return {
        "mode": mode,
        "fell_back": fell_back,
        "model": DEEPSEEK_MODEL,
        "base_url": DEEPSEEK_BASE_URL,
        "api_key_set": bool(DEEPSEEK_API_KEY),
        "engine": f"{ENGINE_HOST}:{ENGINE_PORT}",
    }


# ---------------------------------------------------------------------------
# 商品信息接口（读引擎 Hash：product:* + 秒杀库存计数器）
# ---------------------------------------------------------------------------

@app.get("/api/products")
async def api_products() -> dict:
    try:
        from resp_client import RespClient
        client = RespClient(ENGINE_HOST, ENGINE_PORT, ENGINE_TIMEOUT)
        keys = sorted(k for k in (client.keys("product:*") or []) if k.startswith("product:"))
        products = []
        for k in keys:
            h = client.hgetall(k)
            pid = k.split(":", 1)[1] if ":" in k else k
            price = h.get("price", "")
            try:
                base_stock = int(h.get("stock", "0") or 0)
            except (TypeError, ValueError):
                base_stock = None
            try:
                sold = int(h.get("sold", "0") or 0)
            except (TypeError, ValueError):
                sold = 0
            # 秒杀：独立原子计数器（String）+ 活动 Hash
            flash_counter = client.get(f"flash:1:product:{pid}:stock")
            flash_hash = client.hgetall(f"flash:1:product:{pid}")
            flash_price = flash_hash.get("flash_price")
            if flash_counter is not None and str(flash_counter).lstrip("-").isdigit():
                flash_left = int(flash_counter)
                flash_active = True
            else:
                flash_left = None
                flash_active = False

            if flash_active:
                status = "秒杀售罄" if flash_left <= 0 else "秒杀进行中"
                status_kind = "err" if flash_left <= 0 else "ok"
            elif base_stock is not None and base_stock <= 0:
                status, status_kind = "已售罄", "err"
            elif base_stock is not None and base_stock > 0:
                status, status_kind = "在售", "ok"
            else:
                status, status_kind = "未知", "warn"

            products.append({
                "id": pid,
                "name": h.get("name", k),
                "category": h.get("category", "—"),
                "brand": h.get("brand", ""),
                "price": price,
                "base_stock": base_stock,
                "sold": sold,
                "flash_price": flash_price,
                "flash_left": flash_left,
                "flash_active": flash_active,
                "status": status,
                "status_kind": status_kind,
            })
        client.close()
        return {"ok": True, "data": products, "count": len(products)}
    except Exception as exc:  # noqa: BLE001
        return {"ok": False, "error": str(exc)}


# ---------------------------------------------------------------------------
# 历史接口（重载页面也能看到过往对话）
# ---------------------------------------------------------------------------

@app.get("/api/history")
async def api_history() -> dict:
    return {"ok": True, "data": HISTORY}


# ---------------------------------------------------------------------------
# 页面 & SSE 流式对话
# ---------------------------------------------------------------------------

def _resolve_dry_run(param: str | None) -> bool:
    env_dry = os.environ.get("DRY_RUN", "false").lower() in ("1", "true", "yes")
    if param is not None:
        return param.lower() in ("1", "true", "yes")
    if env_dry:
        return True
    # 未显式指定且没配 Key → 自动回退演示模式
    return not bool(DEEPSEEK_API_KEY)


def _build_agent(on_event, dry_run: bool) -> InventoryAgent:
    if dry_run:
        from offline_demo import DemoToolRunner, default_demo_script
        from llm_client import ScriptedChatModel
        return InventoryAgent(
            model=ScriptedChatModel(default_demo_script()),
            runner=DemoToolRunner(),
            on_event=on_event,
        )
    return InventoryAgent(on_event=on_event)


@app.get("/", response_class=HTMLResponse)
async def index() -> HTMLResponse:
    return HTMLResponse(_HTML)


@app.get("/favicon.ico")
async def favicon() -> Response:
    # 浏览器会自动请求 /favicon.ico；页面已用内联 SVG 图标，这里兜底返回 204，
    # 避免日志里出现 "GET /favicon.ico 404 Not Found"。
    return Response(status_code=204)


@app.get("/api/chat")
async def chat_stream(request: Request):
    question = request.query_params.get("question", "")
    dry_run = _resolve_dry_run(request.query_params.get("dry_run"))

    q: "Queue[dict]" = Queue()
    collected: dict[str, str] = {"answer": ""}

    def on_event(kind: str, payload: dict) -> None:
        if kind == "answer_end":
            collected["answer"] = payload.get("text", "")
        try:
            q.put({"kind": kind, "payload": payload})
        except Exception:  # noqa: BLE001
            pass

    agent = _build_agent(on_event, dry_run)

    def worker() -> None:
        try:
            agent.run_stream(question, session=SESSION)
        except Exception as exc:  # noqa: BLE001
            q.put({"kind": "error", "payload": {"message": f"{type(exc).__name__}: {exc}"}})
        finally:
            # 写入历史（用于重载后恢复）
            HISTORY.append({"question": question, "answer": collected["answer"]})
            if len(HISTORY) > _HISTORY_LIMIT:
                del HISTORY[: len(HISTORY) - _HISTORY_LIMIT]
            q.put({"kind": "__done__"})

    threading.Thread(target=worker, daemon=True).start()

    async def event_stream():
        timeout = 60.0
        while True:
            try:
                item = q.get(timeout=timeout)
            except Empty:
                yield ": keep-alive\n\n"
                timeout = 15.0
                continue
            if item["kind"] == "__done__":
                yield "event: done\ndata: {}\n\n"
                break
            kind = item["kind"]
            data = json.dumps(item["payload"], ensure_ascii=False)
            # 这些事件名直接映射到前端 addEventListener
            if kind in (
                "round_start", "thought_chunk", "tool_result",
                "answer_start", "answer_chunk", "answer_end",
                "final", "finish", "error_event",
            ):
                yield f"event: {kind}\ndata: {data}\n\n"
            # start / assistant / forcing_conclusion 等无需前端渲染，跳过

    return StreamingResponse(event_stream(), media_type="text/event-stream",
                            headers={"Cache-Control": "no-cache", "X-Accel-Buffering": "no"})


def run_server(host: str = "127.0.0.1", port: int = 8000) -> None:
    import uvicorn
    mode = "演示模式" if _resolve_dry_run(None) else "真实模式"
    print(f"库存运维中枢已启动（{mode}）： http://{host}:{port}  （Ctrl+C 退出）")
    uvicorn.run(app, host=host, port=port, log_level="warning")


if __name__ == "__main__":
    run_server()
