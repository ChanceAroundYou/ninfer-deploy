var T={chat:'对话',set:'设置',stat:'运行状况'};
var ED={ninfer:['NInfer sm_86（KVMem 环·256K·dflash2）',
               'Qwen3.8-27B · 30.4B 参数 · q4_k 混合量化']};
var ST={running:'运行中',loading:'加载中',starting:'启动中',stopped:'已停止'};
var STC={running:'on',loading:'load',starting:'load',stopped:''};
var hist=[],busy=false,ctl=null,d={};
var off=0;
var tab=localStorage.getItem('llm.tab')||'chat';
 if(['chat','set','stat'].indexOf(location.hash.slice(1))>=0)tab=location.hash.slice(1);
document.documentElement.dataset.theme=localStorage.getItem('llm.theme')||'dark';
document.getElementById('theme').textContent=document.documentElement.dataset.theme==='dark'?'深色':'浅色';
function $(i){return document.getElementById(i)}
function go(v){tab=v;localStorage.setItem('llm.tab',v);
 document.querySelectorAll('.nav button').forEach(function(b){b.dataset.on=b.dataset.v===v?'1':'0'});
 document.querySelectorAll('.view').forEach(function(x){x.dataset.on=x.dataset.v===v?'1':'0'});
 $('ttl').textContent=T[v];
 if(v==='set'){loadSettings();if(!settingsLoaded){var sw=$('setwrap');if(sw)sw.innerHTML='<div class="card"><h3>设置</h3><div class="empty">正在读取配置…</div></div>';}}
 if(v==='stat'){loadStats();if(!statsLoaded){var stw=$('statwrap');if(stw)stw.innerHTML='<div class="card"><h3>运行状况</h3><div class="empty">正在读取状态…</div></div>';}}}
document.querySelectorAll('.nav button').forEach(function(b){b.onclick=function(){go(b.dataset.v)}});
$('theme').onclick=function(){var dk=document.documentElement.dataset.theme==='dark';
 document.documentElement.dataset.theme=dk?'light':'dark';localStorage.setItem('llm.theme',dk?'light':'dark');
 this.textContent=dk?'浅色':'深色';};
function esc(t){return String(t).replace(/&/g,'&amp;').replace(/</g,'&lt;').replace(/>/g,'&gt;')
 .replace(/"/g,'&quot;').replace(/'/g,'&#39;')}
function unesc(t){return t.replace(/&lt;/g,'<').replace(/&gt;/g,'>').replace(/&quot;/g,'"').replace(/&#39;/g,"'").replace(/&amp;/g,'&')}
var codeStore=[];
function addCopyBtns(bub){if(!bub)return;
 bub.querySelectorAll('pre').forEach(function(pre){
  if(pre.querySelector('.cpy'))return;
  var btn=document.createElement('button');btn.className='cpy';btn.type='button';btn.textContent='复制';
  btn.onclick=function(){var ci=pre.getAttribute('data-ci');
   var t=(ci!=null&&codeStore[+ci]!=null)?codeStore[+ci]:pre.textContent;
   var done=function(){btn.textContent='已复制';setTimeout(function(){btn.textContent='复制'},1200)};
   if(navigator.clipboard&&navigator.clipboard.writeText){navigator.clipboard.writeText(t).then(done,function(){btn.textContent='失败'});}
   else{var ta=document.createElement('textarea');ta.value=t;document.body.appendChild(ta);ta.select();
    try{document.execCommand('copy');done()}catch(e){btn.textContent='失败'}document.body.removeChild(ta);}};
  pre.appendChild(btn);});}
function thinkHTML(rfull,streaming,open){
 if(!rfull)return '';
 var sum='思考过程'+(streaming?'中':'')+' · '+rfull.length+' 字';
 var body=streaming?esc(rfull.slice(-300).replace(/\n/g,' ')):md(rfull);
 return '<details class="think"'+(open?' open':'')+'><summary>'+sum+(streaming?'…':'')+'</summary><div class="tbody">'+body+'</div></details>';}
function md(t){var x=esc(t),b=[];
 x=x.replace(/```[a-zA-Z0-9]*\n?([^]*?)(```|$)/g,function(m,l,c){codeStore.push(unesc(c));
  b.push('<pre data-ci="'+(codeStore.length-1)+'"><code>'+c.replace(/\n/g,'<br>')+'</code></pre>');return '%%PH'+(b.length-1)+'%%PH';});
 x=x.replace(/`([^`\n]+)`/g,'<code>$1</code>');
 x=x.replace(/\*\*([^*]+)\*\*/g,'<b>$1</b>');
 x=x.replace(/\n/g,'<br>');
 return x.replace(/%%PH(\d+)%%PH/g,function(m,i){return b[+i]});}
function addMsg(role,html,meta){var m=$('msgs'),e=document.createElement('div');
 e.className='msg'+(role==='user'?' me':'');
 e.innerHTML='<div class="av">'+(role==='user'?'我':'AI')+'</div><div><div class="bub">'+html+'</div>'+
  (meta?'<div class="meta">'+meta+'</div>':'')+'</div>';
 m.appendChild(e);m.scrollTop=m.scrollHeight;return e.querySelector('.bub')}
function saveH(){try{localStorage.setItem('llm.hist',JSON.stringify(hist.slice(-40)))}catch(e){}}
function loadH(){try{var h=JSON.parse(localStorage.getItem('llm.hist')||'[]');hist=h;
 h.forEach(function(x){var b=addMsg(x.role,x.content?md(x.content):'');if(x.role==='assistant')addCopyBtns(b);});}catch(e){}}
var actBusy=false,saveForce=false,barHide=false,lastOver='',toastT=null;
function toast(m,long){var t=$('toast');if(!t)return;t.textContent=m;t.style.display='block';
 clearTimeout(toastT);toastT=setTimeout(function(){t.style.display='none'},long?9000:3500);}
// 正在输入时不要重渲染 —— 否则新值会被下一次 refresh 用旧配置刷回去。
// 这就是「切到自定义以后怎么改都不变」的第三个原因。
var _typing=false;
document.addEventListener('focusin',function(e){
 if(e.target&&e.target.matches('#setwrap input[type=number]'))_typing=true;});
document.addEventListener('focusout',function(e){
 if(e.target&&e.target.matches('#setwrap input[type=number]'))
   setTimeout(function(){_typing=false;},120);
});
async function act(u){
 var m=u.match(/action=([a-z]+)/),name=m?m[1]:'';
 // drain/abort 会失败在途请求：二次确认，不走进度条（同步直调引擎端点）。
 if(name==='cleardrain'&&!confirm('drain 清空将把在途请求按可重试失败计数后清缓存。继续？'))return;
 if(name==='clearabort'&&!confirm('abort 清空将 abort（含 transaction abort）并把在途请求按可重试失败计数后清缓存。继续？'))return;
 var slow=(name==='start'||name==='stop'||name==='restart'||name==='clearidle'||name==='cleardrain'||name==='clearabort');
 var btns=document.querySelectorAll('[data-act="start"],[data-act="stop"],[data-act="restart"]');
 if(slow&&actBusy){toast('已有操作在进行，请等进度条结束');return;}
 if(name==='save'&&saveForce)u+='&force=1';
 if(slow){actBusy=true;btns.forEach(function(b){b.disabled=true});toast('已提交：'+name+'…',true);}
 try{var r=await fetch(u);var j=null;try{j=await r.json()}catch(e){}
  if(j&&!j.ok){toast(j.msg||('操作被拒绝：'+name),true);}
  else if(j&&j.refused){saveForce=true;toast(j.msg,true);}
  else if(j&&j.msg){toast(j.msg);if(name==='save')saveForce=false;}
 }catch(e){toast('面板请求失败：'+e,true);}
 if(_typing){ // 让输入框保持用户手输的值，只拉数据不重渲
  d=Object.assign(d,await (await fetch('/llm/api/status',{cache:'no-store'})).json().catch(function(){return d}));
  setSig=null;return;
 }
 refresh();}
function setv(k,v){act('/do?action=set&k='+k+'&v='+encodeURIComponent(v));}
function setPort(){var v=$('pbox').value;act('/do?action=setport&v='+encodeURIComponent(v));}
function optBtn(g,o){var c=o.dis?' dis':(o.sel?' sel':'');
 return '<a class="opt'+(o.dis?'" data-dis="1" title="'+esc(o.why||'')+'" href="#"':' '+
  '" href="#"')+' data-k="'+g.key+'" data-v="'+o.v+'" data-sel="'+(o.sel?1:0)+'"'+
  ' data-gh="'+(o.gh?1:0)+'" data-sav="'+(o.sav?1:0)+'"'+
  ' data-dirty="'+((o.sel&&!o.sav)?1:0)+'"'+
  (o.dis?' data-dis="1"':'')+' title="'+esc(o.why||'')+stTip(o)+'">'+esc(o.label)+
  stBadges(o)+'</a>';}
// ---- 四态标记：选中 sel / 正在使用 gh / 已保存 sav / 推荐 rec ----
function stTip(o){
 var t=[];
 if(o.sel)t.push(o.sav?'已选中 · 已保存':'已选中 · 改了没保存（点“保存配置”落盘）');
 else if(o.sav)t.push('已保存的值');
 if(o.gh)t.push(o.sel?'运行中的引擎正在用这个':'运行中的引擎正在用这个，但你没选它 —— 该重启了');
 if(o.rec)t.push('推荐值');
 return t.length?' · '+t.join(' · '):'';}
function stBadges(o){
 // 不画文字标签。背景色/虚实线/呼吸三条都在 CSS 里画完了，这里只出 ★。
 return o.rec?'<sup title="推荐值">★</sup>':'';}
var setSig='';
// 廉价签名：在构建整个 HTML 字符串之前判断设置页是否有变化。
// 原来每 2.5 秒构建 ~100 个按钮 + 2 个预设表的字符串再比较，白做。
function setKey(){
 var a=[d.state,d.phase,d.combo,d.dirty,d.need_restart,d.cfg_error,d.cfg_conflict,
  d.port,d.samp_mode,JSON.stringify(d.samp_custom||{}),JSON.stringify(d.samp_presets||{}),
  d.running_cfg?[d.running_cfg.approx,d.running_cfg.mtp,d.running_cfg.think,d.running_cfg.n_max,d.running_cfg.parallel,d.running_cfg.out_len,d.running_cfg.ctx]:null,
  d.action_over?d.action_over.t:0];
 if(d.groups)for(var i=0;i<d.groups.length;i++){var g=d.groups[i];
  for(var j=0;j<g.items.length;j++){var o=g.items[j];a.push((o.sel?1:0)+(o.gh?1:0)+(o.sav?1:0)+(o.dis?1:0));}}
 return a.join('|');
}
function renderSet(){
 var _ab=$('setwrap');
 // 只在「正在输入中」才跳过重渲染（否则新值会被刷回旧值）。
 // 原来是无条件拦 input:focus，导致输入框失焦后那一帧也刷不掉。
 if(_ab&&_ab.querySelector('input:focus'))return;
 // 焦点刚离开时，等一帧再重渲（浏览器 blur 先于 change 落地）
 var sk=setKey();
 if(sk===setSig)return;setSig=sk;
 var h='';
 if(!d.groups){h+='<div class="card"><h3>设置</h3><div class="empty">正在读取配置…'+
   '若一直停在这里，说明 WebUI 服务未运行。'+
   '<br>配置文件在 llm-deploy/webui/config.json。</div></div>';}
 if(d.cfg_error)h+='<div class="strip err"><span class="dot load"></span>'+esc(d.cfg_error)+'</div>';
 if(d.cfg_conflict)h+='<div class="strip err"><span class="dot load"></span>'+esc(d.cfg_conflict)+'</div>';
 if(d.dirty)h+='<div class="strip"><span class="dot load"></span>有未保存的改动，保存后下次启动默认使用该组合</div>';
 var SM=d.samp_mode||'auto', SC=d.samp_custom||{}, SPR=d.samp_presets||{};
 var SAMPK=['temp','top_p','top_k','min_p','presence','repeat'];
 var SLBL={temp:'temperature',top_p:'top_p',top_k:'top_k',min_p:'min_p',
  presence:'presence_penalty',repeat:'repeat_penalty'};
 var SLO={temp:'0~2',top_p:'0~1',top_k:'0~20',min_p:'0~1',presence:'-2~2',repeat:'0~2'};
 var SDEF={temp:1.0,top_p:0.95,top_k:20,min_p:0,presence:1.5,repeat:1.0};
 // 引擎的合法范围（写错会被启动期拒）：--temperature 0..2 / --top-p 0..1 /
 // --top-k 0..20 / --min-p 0..1 / --presence-penalty -2..2
 var SMIN={temp:0,top_p:0,top_k:0,min_p:0,presence:-2,repeat:0};
 var SMAX={temp:2,top_p:1,top_k:20,min_p:1,presence:2,repeat:2};
 function pTbl(p,lock){var r='<table style="margin:6px 0;width:100%"><tbody>';
  SAMPK.forEach(function(k){var w=(k==='top_k'||k==='repeat')?1:0.05;
   r+='<tr><td class="m" style="padding:2px 10px 2px 0;white-space:nowrap">'+SLBL[k]+
   ' <span class="m" style="opacity:.6">'+SLO[k]+'</span></td><td style="padding:2px 0">'+
   (lock?('<b style="color:var(--accent)">'+p[k]+'</b>'):
   ('<input type="number" id="s_'+k+'" step="'+w+'" min="'+SMIN[k]+'" max="'+SMAX[k]+
    '" value="'+(SC[k]!=null?SC[k]:p[k])+'" style="width:96px"'+
    ' onblur="if(+this.value<'+SMIN[k]+')this.value='+SMIN[k]+
    ';if(+this.value>'+SMAX[k]+')this.value='+SMAX[k]+'">'
    +' <button class="btn sm" data-reset="'+k+
    '" title="恢复官方 maintainer 值 '+SDEF[k]+'">↺</button>'))+'</td></tr>';});
  return r+'</tbody></table>';}
 h+='</div>';
 // 操作 section first
 h+='<div class="card"><h3>操作</h3><div class="row" style="margin-bottom:9px">'+
   '<button class="btn pri" data-act="start">启动引擎</button>'+
   '<button class="btn dang" data-act="stop">停止引擎</button>'+
   '<button class="btn" data-act="restart">重启引擎</button></div>'+
   '<div class="row"><button class="btn" data-act="clearkv">重启并清空 KV</button>'+
   '<span class="m" style="margin:0 8px">清空=重启引擎（约 13 秒实测，期间不可用）</span></div>'+
   '<div class="row" style="margin-top:6px"><button class="btn pri" data-act="clearidle">在线清空（仅空闲，忙则拒绝）</button>'+
   '<span class="m" style="margin:0 8px">约 0.2 秒，不碰在途请求；忙时 409 拒绝不清一半</span></div>'+
   '<details style="margin-top:6px"><summary class="m">高级：drain / abort（将失败在途请求，需二次确认）</summary>'+
   '<div class="row" style="margin-top:6px"><button class="btn" data-act="cleardrain">drain 清空（在途可重试失败后清）</button>'+
   '<button class="btn dang" data-act="clearabort">abort 清空（含 transaction abort）</button></div></details>'+
   '<div class="row" style="margin-top:6px">'+
   '<span style="flex:1"></span>'+
   '<button class="btn" data-act="reset">重置为推荐</button>'+
   '<button class="btn pri" data-act="save">保存配置</button></div>'+
   (d.need_restart?'<div class="m" style="margin-top:8px">设置已改动，需重启引擎生效</div>':'')+
   (d.running_cfg&&d.running_cfg.approx?'<div class="m" style="margin-top:8px">引擎是从端口接管的，只有引擎类型确知，其余参数不标「运行中正用」</div>':'')+
   '</div>';
 // 运行参数 section second
 h+='<div class="card"><h3>运行参数</h3>';
 for(var i=1;d.groups&&i<d.groups.length;i++){var g=d.groups[i];
  h+='<div class="row" style="margin-bottom:9px"><span class="lab">'+esc(g.title)+'</span>';
  (g.items||[]).forEach(function(o){h+=optBtn(g,o)});h+='</div>';}
 h+='<div class="row" style="margin-top:4px"><span class="lab">监听端口</span>'+
   '<input type="number" id="pbox" value="'+(d.port||8080)+'" style="width:92px">'+
   '<button class="btn sm" id="pb">应用</button><span class="m">改动需重启引擎生效</span></div>';
 h+='</div>';
 // 采样参数 section last
 h+='<div class="card"><h3>采样参数</h3>'+
  '<p class="sub">两套预设 = 官方 maintainer 配方（docs/04 §L2），与引擎默认值逐字相同，'+
  '唯一区别是 presence 从规格的 0.0 改成 1.5 —— 思考通道不惩罚重复会滑向同义词罗列、'+
  '正文永远出不来（QwenLM/Qwen3.8 issue #88）。'+
  '「自动」= 不注入任何参数，让引擎按每个请求是否思考实时切两套；'+
  '「思考/非思考」= 强制用那一套；「自定义」= 六项都可改，面板转发立即生效。</p>';
 [['auto','自动（推荐 ★）','按每个请求的思考状态实时切两套预设'],
  ['think','思考预设','模型先推理再回答；temp 1.0 适合发散'],
  ['nonthink','非思考预设','直接给答案；temp 0.7 + presence 1.5 更稳'],
  ['custom','自定义','六项可改，改完立即生效']].forEach(function(a){
  h+='<div class="row" style="margin:5px 0"><label style="cursor:pointer;display:flex;align-items:baseline;gap:6px">'+
     '<input type="radio" name="smode" value="'+a[0]+'"'+(SM===a[0]?' checked':'')+'>'+
     '<span><b>'+a[1]+'</b> <span class="m">'+a[2]+'</span></span></label></div>';});
 if(SM==='think') h+=pTbl(SPR.think||{},true);
 else if(SM==='nonthink') h+=pTbl(SPR.nonthink||{},true);
 else if(SM==='custom') h+=pTbl(SC,false);
 else h+='<div style="display:flex;gap:24px;flex-wrap:wrap">'+
   '<div style="flex:1;min-width:240px"><div class="m" style="margin-top:4px">思考请求自动用：</div>'+pTbl(SPR.think||{},true)+'</div>'+
   '<div style="flex:1;min-width:240px"><div class="m" style="margin-top:4px">直接回答自动用：</div>'+pTbl(SPR.nonthink||{},true)+'</div></div>';
 h+='<div class="m" style="margin-top:8px">⚠️ presence_penalty <b>不可超 1.5</b>：'+
   '本引擎无惩罚窗口（全序列累积），值越大越会伤到满屏重复标签的 HTML/SVG。'+
   '接工具调用时改回 <b>0</b>——高 presence 会跟结构化 XML 打架（模型改用自造标签）。</div>';
 h+='</div>';
 $('setwrap').innerHTML=h;
 $('pb').onclick=setPort;
 document.querySelectorAll('input[name=smode]').forEach(function(el){
  el.onchange=function(){act('/do?action=setsamp&mode='+el.value);};});
 // 采样六项：oninput 实时落库（数字框 change 要失焦才触发，用户会以为"没反应"），
 // onchange 兜底（有些浏览器对 number 输入的 input 事件有 quirks）。
 [['temp'],['top_p'],['top_k'],['min_p'],['presence'],['repeat']].forEach(function(a){
  var el=document.getElementById('s_'+a[0]);
  if(!el)return;
  var send=function(){
   var v=el.value;
   if(v===''||isNaN(+v))return;
   act('/do?action=setsamp&k='+a[0]+'&v='+encodeURIComponent(v));
  };
  el.oninput=send;            // 边打边存
  el.onchange=send;           // 失焦/上下箭头再存一次
  el.onblur=send;
 });
 // ↺ 恢复官方 maintainer 值（SDEF 与面板顶部的 JS 常量一致）
 document.querySelectorAll('#setwrap [data-reset]').forEach(function(b){b.onclick=function(e){
  e.preventDefault();var k=b.dataset.reset;
  act('/do?action=setsamp&k='+k+'&v='+SDEF[k]+'&mode=custom');
  toast('已恢复 '+SLBL[k]+' = '+SDEF[k]+'（官方 maintainer 配方）');};});
 document.querySelectorAll('#setwrap .opt').forEach(function(a){a.onclick=function(e){
  e.preventDefault();if(a.dataset.dis==='1')return;setv(a.dataset.k,a.dataset.v);}});
 document.querySelectorAll('#setwrap .ecard').forEach(function(a){a.onclick=function(){setv('engine',a.dataset.v)}});
 document.querySelectorAll('#setwrap [data-act]').forEach(function(b){b.onclick=function(){act('/do?action='+b.dataset.act)}});
}
var statSig='',logBig=false,settingsLoaded=false,statsLoaded=false,logSource=null,logShown=0,paintPend=null,logPend=null;
function loadSettings(){if(settingsLoaded)return;fetch('/llm/api/settings',{cache:'no-store'}).then(function(r){return r.json()}).then(function(j){if(j&&j.groups){d=Object.assign(d,j);settingsLoaded=true;if(tab==='set')renderSet();}}).catch(function(){});}
function loadStats(){if(statsLoaded)return;fetch('/llm/api/stats',{cache:'no-store'}).then(function(r){return r.json()}).then(function(j){d=Object.assign(d,j);statsLoaded=true;if(tab==='stat')renderStat(true);startLogs();}).catch(function(){});}
function startLogs(){if(logSource)return;logSource=new EventSource('/llm/log');logSource.onmessage=function(e){var line;try{line=JSON.parse(e.data)}catch(x){return}d.tail=d.tail||[];d.tail.push(line);d.tail=d.tail.slice(-300);if(!logPend)logPend=requestAnimationFrame(function(){logPend=null;renderStat();});};logSource.onerror=function(){if(logSource){logSource.close();logSource=null;}};}

function _kfmt(n){n=+n||0;return n>=1000?(n/1000).toFixed(0)+'k':(''+n);}
function kvTile(d){
 var c=d.cache;if(!c||!c.tokens_max)return '';
 var pct=c.tokens_max?Math.min(100,Math.round((c.tokens_used||0)/c.tokens_max*100)):0;
 return '<div class="stat"><div class="k">KV 缓存容量</div>'+
  '<div class="v">'+_kfmt(c.tokens_used)+'/'+_kfmt(c.tokens_max)+'</div>'+
  '<div class="bar"><i style="width:'+pct+'%"></i></div>'+
  '<div class="m" style="margin-top:5px">条数 '+(c.slots_used||0)+'/'+(c.slots_max||0)+
  ' · 已用 '+pct+'%</div></div>';
}
function kvEvictTile(d){
 var c=d.cache;if(!c)return '';
 var hostGb=(+c.host_kv_bytes||0)/1073741824;
 return '<div class="stat"><div class="k">逐出 / 落内存</div>'+
  '<div class="v">'+(c.evicted||0)+'</div>'+
  '<div class="bar"><i style="width:'+(c.d2h_pages?100:0)+'%"></i></div>'+
  '<div class="m" style="margin-top:5px">逐出 '+(c.evicted||0)+' · 丢 '+(c.dropped||0)+
  ' · 降级 '+(c.demoted||0)+' · 退化 '+(c.degraded||0)+'</div>'+
  '<div class="m" style="margin-top:3px">下沉发布 '+(c.spill_pages||0)+' 页 · 主KV落内存 '+
  (c.d2h_pages||0)+' 页 · 召回 '+(c.h2d_pages||0)+' 页</div>'+
  '<div class="m" style="margin-top:3px">Host KV '+hostGb.toFixed(2)+' GiB · Host槽 '+
  (c.host_state_slots||0)+' · State下沉/召回 '+(c.state_d2h||0)+'/'+(c.state_h2d||0)+'</div>'+
  '<div class="m" style="margin-top:3px">复用 '+(c.reused_tokens||0)+' tok · root '+
  (c.sel_root||0)+' · 共享 '+(c.sel_shared||0)+' · 私有命中 '+(c.sel_endpoint||0)+'</div></div>';
}
function logHTML(lines){return (lines||[]).map(function(line){
 var x=esc(line),m=x.match(/^(\[?\d{4}-\d{2}-\d{2}[ T]\d{2}:\d{2}:\d{2}(?:\.\d+)?\]?|\[\d{2}:\d{2}:\d{2}\])/);
 return m?'<b class=\"logtime\">'+m[1]+'</b>'+x.slice(m[1].length):x;
 }).join(String.fromCharCode(10));}
function renderStat(force){
 var vu=+d.vram_used||0,vt=+d.vram_total||0,pct=vt?Math.round(vu/vt*100):0;
 var poolGb=parseFloat((d.pool||'').replace(' GiB',''))||0;
 // tailKey 不能进 sig：日志每 5 秒多一行，整页 innerHTML 重建一次就是一次卡顿。
 // 新日志交给 syncLogBox() 增量 append。
 var reqKey=(d.reqs.length)+'|'+(d.reqs.length?d.reqs[0].slice(0,2).join('|'):'-');
 // cache 的三个数进 sig：KV 用量是这里唯一会自己变、又不属于 reqs/ram_load 的量
 var c0=d.cache||{};
 var kvKey=(c0.tokens_used||0)+'/'+(c0.slots_used||0)+'/'+(c0.evicted||0)+'/'+(c0.d2h_pages||0)+'/'+(c0.h2d_pages||0)+'/'+(c0.spill_pages||0)+'/'+(c0.dropped||0);
 var sig=[Math.round(vu/10),d.ram_load,reqKey,d.free_mib,kvKey].join('|');
 var lb0=$('statwrap')?$('statwrap').querySelector('.logbox'):null;
 var atBottom=!lb0||(lb0.scrollTop+lb0.clientHeight>=lb0.scrollHeight-24);
 var prevTop=lb0?lb0.scrollTop:0;
 if(!force&&sig===statSig){var ue=document.getElementById('utp');if(ue)ue.textContent=d.uptime;syncLogBox(lb0,atBottom);return;}
 statSig=sig;
 var h='<div class="stats">'+
  '<div class="stat"><div class="k">显存</div><div class="v">'+(vt?(vu/1024).toFixed(1)+' G':'-')+'</div>'+
   '<div class="bar"><i style="width:'+pct+'%"></i></div><div class="m" style="margin-top:5px">'+vu+' / '+vt+' MiB</div></div>'+
  '<div class="stat"><div class="k">内存负载</div><div class="v">'+d.ram_load+'%</div>'+
   '<div class="bar"><i style="width:'+d.ram_load+'%"></i></div>'+
   (poolGb?('<div class="bar" style="margin-top:4px"><i style="width:'+Math.min(100,Math.round(poolGb/(+d.ram_total||1)*100))+'%;background:var(--gold)"></i></div>'):'')+
   '<div class="m" style="margin-top:5px">可用 '+d.ram_avail+' / '+d.ram_total+' GB'+(poolGb?(' · KV 池 '+d.pool):'')+'</div></div>'+
  '<div class="stat"><div class="k">运行时长</div><div class="v" id="utp">'+d.uptime+'</div>'+
   '<div class="bar"><i style="width:100%"></i></div>'+
   '<div class="m" style="margin-top:5px">PID '+(d.pid||'-')+' · 池 '+(d.pool||'-')+
   (d.free_mib!=null?(' · 启动显存余量 '+d.free_mib+' MiB'):'')+
   (d.free_mib!=null&&d.free_mib<250?'（&lt;250 MiB：prefill 易掉到 300 tok/s 档，先关掉多余浏览器窗口）':'')+
   '</div></div>'+
  kvTile(d)+kvEvictTile(d)+'</div>';
 h+='<div class="card"><h3>最近请求</h3>';
 if(!d.reqs.length)h+='<div class="m">暂无请求记录</div>';
 else{h+='<table><thead><tr><th>时间</th><th>input tok</th><th>output tok</th><th>cache tok</th><th>prefill t/s</th><th>decode t/s</th></tr></thead><tbody>';
  d.reqs.forEach(function(r){h+='<tr>'+r.map(function(c){return '<td>'+esc(c)+'</td>'}).join('')+'</tr>'});
  h+='</tbody></table>';}
 h+='</div><div class="card"><h3>引擎日志</h3><div class="row" style="margin-bottom:7px">'+
   '<button class="btn sm" id="logcopy">复制</button><button class="btn sm" id="logbig">'+(logBig?'收起':'展开')+'</button>'+
   '<span class="m">显示末尾 '+((d.tail||[]).length)+' 行 · 完整日志 '+esc(d.log||'')+'</span></div>'+
   '<div class="logbox" id="logbox" style="height:'+(logBig?'60vh':'200px')+'">'+
   logHTML(d.tail)+'</div></div>';
 $('statwrap').innerHTML=h;
 var lb=$('statwrap').querySelector('.logbox');
 if(lb)lb.scrollTop=atBottom?lb.scrollHeight:prevTop;
 logShown=(d.tail||[]).length;
 var cpy=$('logcopy');if(cpy)cpy.onclick=function(){var t=lb?lb.textContent:'';
  if(navigator.clipboard&&navigator.clipboard.writeText){navigator.clipboard.writeText(t).then(function(){
   cpy.textContent='已复制';setTimeout(function(){cpy.textContent='复制'},1200)});}};
 var bgb=$('logbig');if(bgb)bgb.onclick=function(){logBig=!logBig;renderStat(true)};
}
// 只把新增的日志行 append 进日志盒，避免每 5 秒整盒 innerHTML 重建。
function syncLogBox(lb,atBottom){
 if(!lb)return;
 var lines=d.tail||[];
 if(lines.length<logShown)logShown=0;   // 日志被轮转/截断，重画
 if(lines.length===logShown)return;
 lb.insertAdjacentHTML('beforeend',lines.slice(logShown).map(function(line){
  var x=esc(line),m=x.match(/^(\[?\d{4}-\d{2}-\d{2}[ T]\d{2}:\d{2}:\d{2}(?:\.\d+)?\]?|\[\d{2}:\d{2}:\d{2}\])/);
  return m?'<b class="logtime">'+m[1]+'</b>'+x.slice(m[1].length):x;
 }).join(String.fromCharCode(10)));
 logShown=lines.length;
 if(atBottom)lb.scrollTop=lb.scrollHeight;
}
var miniSig='';
function renderMini(){
 var st=d.state||'stopped';
 var msig=st+'|'+(d.combo||'')+'|'+(d.short||'');
 if(msig===miniSig)return;miniSig=msig;
 $('mini').innerHTML='<div class="t1"><span class="dot '+(STC[st]||'')+'"></span>'+esc(d.short||'-')+'</div>'+
  '<div class="t2">'+esc(d.combo||'')+'</div>'+
  '<div class="btns">'+(st==='stopped'?'<button class="btn sm" data-act="start">启动</button>':
   '<button class="btn sm" data-act="stop">停止</button><button class="btn sm" data-act="restart">重启</button>')+'</div>';
 document.querySelectorAll('#mini [data-act]').forEach(function(b){b.onclick=function(){act('/do?action='+b.dataset.act)}});
}
var raf=0,rafT=null;
function setPoll(ms){clearInterval(rafT);rafT=setInterval(refresh,ms);}
async function refresh(){
 if(raf)return;raf=1;
 var _t0=performance.now();
 var r=null;
 try{var ac=new AbortController();var to=setTimeout(function(){ac.abort()},4000);
     r=await fetch('/llm/api/status',{signal:ac.signal});clearTimeout(to);}catch(e){r=null;}
 raf=0;
 var j=null;
 if(r&&r.ok){try{j=await r.json();}catch(e){j=null;}}
 if(!r||!r.ok||!j){off++;if(off===1)offline();setPoll(7000);return;}
 off=0;clearOffline();setPoll(2500);d=Object.assign(d,j);
 var sp=$('spill');if(sp)sp.title='接口延迟 '+Math.round(performance.now()-_t0)+'ms';
 var _ph=(d.phase&&String(d.phase).indexOf('loading')===0)?(' · '+d.phase):'';
 $('spill').innerHTML='<span class="dot '+(STC[d.state]||'')+'"></span>'+(ST[d.state]||d.state)+_ph;
 var ph=d.phase||'';
 $('mstat').textContent=d.state!=='running'?'引擎未运行':
  (ph.indexOf('loading')===0?('加载权重 '+(ph.replace('loading ',''))):
   (String(d.short||'').indexOf('ninfer')===0&&(d.uptime||0)<120?'预热中 · 首句稍慢':'上下文可用 '+((d.ctx||0)/1024)+'K'));
 document.title=(d.state==='running'&&ph.indexOf('loading')!==0?'':'('+(d.state==='running'?'加载中':ST[d.state]||d.state)+') ')+'NInfer WebUI';
 renderMini();if(tab==='set'){loadSettings();renderSet();}if(tab==='stat'){loadStats();renderStat();}renderAction();tkEst();}
function renderAction(){
 var a=d.action,ov=d.action_over,bar=$('actbar');
 var big=document.querySelectorAll('[data-act="start"],[data-act="stop"],[data-act="restart"]');
 if(a){
  var eta=(a.pct&&a.pct>4)?' · 预计还需 '+Math.max(1,Math.round(a.secs*(100-a.pct)/a.pct))+'s':'';
  if(bar&&!barHide){bar.className='on';
   bar.innerHTML='<span class="dot load"></span><b>'+esc(a.label||a.name)+'</b> · '+esc(a.stage||'进行中')+
    ' · 已 '+a.secs+'s'+eta+(a.err?(' · <b style="color:var(--danger)">'+esc(a.err)+'</b>'):'')+
    '<span style="flex:1"></span><a href="#" id="acthide" style="color:var(--dim)">收起</a>';
   var ah=$('acthide');if(ah)ah.onclick=function(e){e.preventDefault();barHide=true;bar.className='';};}
  actBusy=true;document.body.classList.add('acton');big.forEach(function(b){b.disabled=true});
 }else{
  if(bar){bar.className='';bar.innerHTML='';}
  actBusy=false;document.body.classList.remove('acton');big.forEach(function(b){b.disabled=false});
 }
 if(ov){var k=ov.label+'|'+ov.t;
  if(k!==lastOver){lastOver=k;barHide=false;
   toast(ov.label+(ov.ok===null?' 等待超时：'+(ov.msg||'引擎可能仍在加载'):
        ov.ok?(' 完成 · 耗时 '+ov.secs+'s'):(' 失败：'+(ov.msg||'查看「日志」页'))),true);}}}
function clearOffline(){
 var s2=document.getElementById('stripbox');
 if(s2&&s2.style.display!=='none'){s2.style.display='none';}}
function offline(){
 var e=$('spill');if(e)e.innerHTML='<span class="dot load"></span>面板服务未运行';
 var m=$('mstat');if(m)m.textContent='面板服务未运行';
 var s2=document.getElementById('stripbox');
 if(s2&&s2.style.display==='none'){
   s2.style.display='block';
   s2.innerHTML='<b>面板服务没有在运行。</b>托盘进程已经退出，本页无法再发送指令。<a href="#" id="reload">立即刷新本页</a><span class="m">（若引擎还在，刷新后可继续对话）</span>';
   var rl=document.getElementById('reload');
   if(rl)rl.onclick=function(e2){e2.preventDefault();location.reload();};}}
async function send(){
 if(busy)return;
 var v=$('txt').value.trim();if(!v)return;
 if(d.state!=='running'){addMsg('assistant','引擎还没运行。到「设置」页点<b>启动引擎</b>。');return;}
 if(d.phase&&String(d.phase).indexOf('loading')===0){addMsg('assistant','引擎还在读权重（约 65 秒，当前 '+d.phase.replace('loading ','')+'）。稍等一下再发。');return;}
 hist.push({role:'user',content:v});addMsg('user',md(v));$('txt').value='';saveH();
 busy=true;$('sendb').style.display='none';$('stopb').style.display='';
 var bub=addMsg('assistant','<span class="caret"></span>');var m=$('msgs');
 var tp=Math.min(2,Math.max(0,parseFloat($('temp').value)||0.2));
 var mt=Math.min(32768,Math.max(16,parseInt($('mt').value)||1024));
 var body={messages:hist,temperature:tp,max_tokens:mt,stream:true};
 var t0=performance.now(),full='',rfull='',gen=0,pre=0,tk=null,err='';
 ctl=new AbortController();
 try{
  var res=await fetch('/llm/chat',{method:'POST',headers:{'Content-Type':'application/json'},
   body:JSON.stringify(body),signal:ctl.signal});
  if(!res.ok){var j={};try{j=await res.json()}catch(e){}
   err=j.error||('HTTP '+res.status);throw new Error(err);}
  var rd=res.body.getReader(),dc=new TextDecoder(),bf='';
  while(true){var o=await rd.read();if(o.done)break;
   bf+=dc.decode(o.value,{stream:true});var ls=bf.split(String.fromCharCode(10));bf=ls.pop();
   for(var i=0;i<ls.length;i++){var ln=ls[i];if(ln.indexOf('data:')!==0)continue;
    var p=ln.slice(5).trim();if(!p||p==='[DONE]')continue;
    try{var j2=JSON.parse(p);
     var dl=(j2.choices&&j2.choices[0]&&j2.choices[0].delta)||{};
     if(dl.reasoning_content)rfull+=dl.reasoning_content;
     if(dl.content)full+=dl.content;
     if(j2.usage){gen=j2.usage.completion_tokens;pre=j2.usage.prompt_tokens}
     if(j2.timings)tk=j2.timings;
    }catch(e){}}
   // rAF 合并：每 chunk 渲染是 O(n²) + 强制布局；最多 60fps 重绘一次
   if(!paintPend){paintPend=requestAnimationFrame(function(){paintPend=null;
    bub.innerHTML=thinkHTML(rfull,true,!full)+md(full)+'<span class="caret"></span>';
    m.scrollTop=m.scrollHeight;});}
  }
 }catch(e){full=full||'';err=err||e.message;}
 busy=false;$('stopb').style.display='none';$('sendb').style.display='';
 if(paintPend){cancelAnimationFrame(paintPend);paintPend=null;}
 var fin=thinkHTML(rfull,false,false)+(full?md(full):'');
 if(!full&&err)fin+='<div class="m" style="color:var(--danger)">请求失败：'+esc(err)+'</div>';
 bub.innerHTML=fin;addCopyBtns(bub);
 if(full){hist.push({role:'assistant',content:full});saveH();
  var secs=(performance.now()-t0)/1000;
  if(!gen)gen=Math.max(1,Math.round(full.length/1.6));
  var rate=tk&&tk.predicted_per_second?(+tk.predicted_per_second).toFixed(1):(gen/secs).toFixed(1);
  var pf=tk&&tk.prompt_per_second?(' · prefill '+(+tk.prompt_per_second).toFixed(0)+' t/s'):'';
  bub.parentNode.insertAdjacentHTML('beforeend','<div class="meta">'+gen+' tok · '+rate+' t/s · '+
   secs.toFixed(1)+'s'+pf+(err?' · 已中断':'')+'</div>');}
 else if(err){
  try{if(hist.length&&hist[hist.length-1].role==='user'){hist.pop();saveH();}}catch(e2){}
  var mm=$('msgs');
  if(mm){if(mm.lastElementChild)mm.removeChild(mm.lastElementChild);
         if(mm.lastElementChild)mm.removeChild(mm.lastElementChild);}
  $('txt').value=v;growTxt();}
 m.scrollTop=m.scrollHeight;}
$('sendb').onclick=send;
$('newc').onclick=function(){if(hist.length&&!confirm('开一个新会话？当前历史将从本页移除，引擎不受影响。'))return;
 hist=[];$('msgs').innerHTML=emptyHTML();bindEx();saveH();tkEst();};
$('clr').onclick=function(){if(!hist.length&&!$('msgs').querySelector('.msg')){toast('历史已经是空的');return;}
 if(!confirm('清空全部对话历史？此操作不可恢复（引擎不受影响）。'))return;
 hist=[];try{localStorage.removeItem('llm.hist')}catch(e){}
 $('msgs').innerHTML=emptyHTML();bindEx();saveH();tkEst();toast('已清空对话历史');};
$('stopb').onclick=function(){if(ctl){ctl.abort();toast('已请求中断生成')}};
var txtEl=$('txt');
function growTxt(){txtEl.style.height='auto';txtEl.style.height=Math.min(txtEl.scrollHeight,170)+'px';tkEst();}
function tkEst(){var ch=txtEl.value.length,i;for(i=0;i<hist.length;i++)ch+=(hist[i].content||'').length;
 var n=Math.ceil(ch/1.6),lim=Math.round(d.ctx||0),e=$('tkexp');if(!e)return;
 if(!n){e.textContent='';e.style.color='';return;}
 var f=function(x){return x>=10000?(x/1000).toFixed(1)+'k':x};
 var s='≈'+f(n)+' tok';
 if(lim){var rest=Math.max(0,lim-n);s+=' · 剩余 '+f(rest);
  e.style.color=rest<2000?'var(--danger)':'';}
 e.textContent=s;}
txtEl.addEventListener('input',growTxt);
$('temp').value=localStorage.getItem('llm.temp')||'0.7';
$('mt').value=localStorage.getItem('llm.mt')||'1024';
$('temp').addEventListener('change',function(){localStorage.setItem('llm.temp',this.value)});
$('mt').addEventListener('change',function(){localStorage.setItem('llm.mt',this.value)});
$('txt').addEventListener('keydown',function(e){
 if(e.isComposing||e.keyCode===229)return;
 if(e.keyCode===13&&!e.shiftKey){e.preventDefault();send()}});
var EXS=[['总结长文档','把下面这篇文档整理成要点清单，保留所有数字与结论：\n\n'],
         ['逐行解释代码','逐行解释下面这段代码的作用与潜在边界问题：\n\n'],
         ['改写成正式邮件','把下面的内容改写成正式邮件：\n\n']];
function emptyHTML(){return '<div class="empty"><b>开始对话</b>Qwen3.8-27B · 256K 上下文 · 投机解码可在「设置」里调整</div>'+
 '<div class="exs">'+EXS.map(function(x,i){return '<button class="btn sm exb" data-i="'+i+'">'+esc(x[0])+'</button>'}).join('')+'</div>';}
function bindEx(){$('msgs').querySelectorAll('.exb').forEach(function(b){b.onclick=function(){
 var ex=EXS[+b.dataset.i];txtEl.value=ex[1];growTxt();txtEl.focus();}});}
if(!hist.length&&localStorage.getItem('llm.hist'))loadH();
if(!hist.length){$('msgs').innerHTML=emptyHTML();bindEx();}
document.addEventListener('visibilitychange',function(){
 if(document.hidden){setPoll(15000);}else{setPoll(2500);refresh();}});
go(tab);setPoll(2500);refresh();growTxt();
