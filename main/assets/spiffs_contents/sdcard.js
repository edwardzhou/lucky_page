/* sdcard.js — SD 卡文件管理：挂载状态与容量、目录浏览、上传/下载、新建文件夹、重命名、删除。
   接口：GET /api/sdcard、GET /api/sdcard/list、POST /api/sdcard/upload|mkdir|mount|unmount、
        GET /api/sdcard/download、DELETE /api/sdcard/file、POST /api/sdcard/rename。
   ?demo=1 时这些接口在 demo.js 里统一 501（演示环境没有 SD 卡）：本页检测到演示标记后
   **不发起请求**直接渲染「演示环境不支持 SD 卡」，避免浏览器控制台出现 501 噪声；
   请求层仍保留 501 → 中文提示的兜底（真机/独立预览下遇到也能降级）。
   文件名一律当纯文本处理（设备 FAT 可能回非 UTF-8 名称，只做原样展示，绝不拼 HTML）。 */
(function(){
 'use strict';
 const L=window.Lucky,$=id=>document.getElementById(id);
 const ROOT='/';
 const DEMO_TEXT='演示环境不支持 SD 卡';
 const NAME_MAX_BYTES=120;                // 与 sd_paths::ValidName 的上限一致
 const BAD_NAME=/[\/\\:*?"<>|]/;
 // 静态图标（不含用户数据，才允许 innerHTML；文件名一律 textContent）
 const ICON_DIR='<svg class="lp-icon" viewBox="0 0 24 24" aria-hidden="true">'+
   '<path d="M3 7a2 2 0 0 1 2-2h5l2 2h7a2 2 0 0 1 2 2v9a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2Z"/></svg>';
 const ICON_FILE='<svg class="lp-icon" viewBox="0 0 24 24" aria-hidden="true">'+
   '<path d="M6 3h8l4 4v13H6zM14 3v4h4"/></svg>';
 const state={demo:!!(L&&L.demo),mounted:false,statusKnown:false,busy:false,
   error:'',path:ROOT,info:null,items:[]};

 // ---------- 小工具 ----------
 const bytesOf=s=>new TextEncoder().encode(String(s)).length;
 function setText(id,text){const el=$(id);if(el)el.textContent=text}
 function setChip(el,text,kind){if(!el)return;el.textContent=text;el.className='chip'+(kind?' '+kind:'')}
 function fmtBytes(n){
   const v=Number(n);
   if(!Number.isFinite(v)||v<0)return '—';
   if(v<1024)return v+' B';
   const kb=v/1024;
   if(kb<1024)return kb.toFixed(1)+' KB';
   const mb=kb/1024;
   if(mb<1024)return mb.toFixed(1)+' MB';
   return (mb/1024).toFixed(2)+' GB';
 }
 function fmtInt(n){
   const v=Math.round(Number(n));
   if(!Number.isFinite(v))return '—';
   return String(v).replace(/\B(?=(\d{3})+(?!\d))/g,',');
 }
 function detail(n){return Number.isFinite(Number(n))?fmtBytes(n)+'（'+fmtInt(n)+' 字节）':fmtBytes(n)}

 // 路径：/a/b；拒绝 .. 上跳（设备端也会再校验一次）
 function normalizePath(p){
   let s=String(p==null?'':p).trim();
   if(!s||s.charAt(0)!=='/')s='/'+s;
   const parts=[];
   s.split('/').forEach(seg=>{
     if(!seg||seg==='.')return;
     if(seg==='..'){parts.pop();return}
     parts.push(seg);
   });
   return parts.length?'/'+parts.join('/'):ROOT;
 }
 function joinPath(dir,name){
   const d=normalizePath(dir);
   return d===ROOT?ROOT+name:d+'/'+name;
 }
 function displayPath(p){const v=normalizePath(p);return v===ROOT?'根目录':v}
 // 逐段 encodeURIComponent：保留 / 分隔符，中文/空格/% 都会被编码；
 // 固件 QueryValue 会解码 %xx，且其查询值缓冲只有 96 字节，整体编码中文路径会过长
 function encodePath(p){return normalizePath(p).split('/').map(encodeURIComponent).join('/')}
 function nameError(name,label){
   const v=String(name||'');
   if(!v)return label+'不能为空';
   if(v==='.'||v==='..')return label+'不能是「.」或「..」';
   if(bytesOf(v)>NAME_MAX_BYTES)return label+'过长：最多 '+NAME_MAX_BYTES+' 字节（约 40 个汉字）';
   if(BAD_NAME.test(v))return label+'不能包含 / \\ : * ? " < > |';
   if(/[\u0000-\u001f\u007f]/.test(v))return label+'不能包含控制字符';
   return '';
 }

 // ---------- 请求层 ----------
 // L.api 只保留中文错误、丢掉状态码；这里需要区分 501（演示环境）与普通失败，
 // 上传还要带原始字节，故单独实现一个带状态码的薄封装。
 async function request(url,opts){
   const o=opts||{};
   const method=o.method||'GET';
   const timeout=Number.isFinite(o.timeout)?o.timeout:(method==='GET'?8000:20000);
   const ctrl=new AbortController();
   const timer=setTimeout(()=>ctrl.abort(),timeout);
   let res;
   try{
     const init={method:method,signal:ctrl.signal};
     if(o.json!==undefined){init.headers={'Content-Type':'application/json'};init.body=JSON.stringify(o.json)}
     else if(o.body!==undefined){init.headers={'Content-Type':'application/octet-stream'};init.body=o.body}
     res=await fetch(url,init);
   }catch(e){
     clearTimeout(timer);
     throw new Error(e&&e.name==='AbortError'?'请求超时':('无法连接设备（'+((e&&e.message)||'网络错误')+'）'));
   }
   clearTimeout(timer);
   let raw='';
   try{raw=await res.text()}catch(e){raw=''}
   let data=null;
   if(raw){try{data=JSON.parse(raw)}catch(e){data=null}}
   if(!res.ok||(data&&data.error)){
     const err=new Error(res.status===501?DEMO_TEXT:((data&&data.error)||('设备返回 HTTP '+res.status)));
     err.status=res.status;
     err.demo=res.status===501;
     throw err;
   }
   return data||{};
 }

 // ---------- 消息 ----------
 function showError(prefix,e){
   if(e&&e.demo){
     showMsg('sdMsg','err',DEMO_TEXT+'：演示数据里没有存储设备，文件管理不可用。');
     toast(DEMO_TEXT,'warn');
     return;
   }
   showMsg('sdMsg','err',prefix+'：'+((e&&e.message)||'未知错误'));
   toast((prefix||'操作').replace(/失败$/,'')+'失败','err');
 }
 function demoNotice(){
   showMsg('sdMsg','err',DEMO_TEXT+'：演示数据里没有存储设备，文件管理不可用。');
   toast(DEMO_TEXT,'warn');
 }
 function needMounted(){
   if(state.demo){demoNotice();return true}
   if(state.busy)return true;
   if(!state.mounted){
     showMsg('sdMsg','warn','SD 卡未挂载：请先点「挂载」，或插卡后点「刷新」。抽奖与打印不受影响。');
     return true;
   }
   return false;
 }

 // ---------- 状态与列表 ----------
 function applyStatus(d){
   const mounted=!!(d&&d.mounted);
   const prev=state.info||{};
   const info={card:'',mount:(d&&typeof d.mount==='string'&&d.mount)?d.mount:(prev.mount||'/sdcard'),
     total:0,used:0,free:0,hasCapacity:false};
   if(mounted){
     info.card=(d&&typeof d.card==='string'&&d.card)?d.card:(prev.card||'');
     const total=Number(d&&d.total_bytes),used=Number(d&&d.used_bytes),free=Number(d&&d.free_bytes);
     if(Number.isFinite(total)&&total>0&&Number.isFinite(used)&&used>=0){
       info.total=total;
       info.used=Math.min(used,total);
       info.free=(Number.isFinite(free)&&free>=0)?free:Math.max(0,total-info.used);
       info.hasCapacity=true;
     }else if(prev.hasCapacity&&state.mounted){
       // list 接口在容量不可用时省略字段 → 沿用状态接口刚读到的容量
       info.total=prev.total;info.used=prev.used;info.free=prev.free;info.hasCapacity=true;
     }
   }
   state.mounted=mounted;
   state.statusKnown=true;
   state.demo=false;
   state.error='';
   state.info=info;
   if(!mounted)state.items=[];
 }
 function applyDemo(){
   state.demo=true;
   state.mounted=false;
   state.statusKnown=false;
   state.busy=false;
   state.info=null;
   state.items=[];
   state.path=ROOT;
   state.error='';
   render();
   showMsg('sdMsg','err',DEMO_TEXT+'：演示数据里没有存储设备，文件管理不可用；其他页面仍可正常体验。');
 }
 function normalizeItems(raw){
   const src=Array.isArray(raw)?raw:[];
   const out=[];
   for(const it of src){
     if(!it||typeof it!=='object')continue;
     const name=typeof it.name==='string'?it.name:(it.name==null?'':String(it.name));
     if(!name||name==='.'||name==='..')continue;   // 设备已过滤，这里防御异常数据
     const type=it.type==='dir'?'dir':'file';
     let size=Number(it.size);
     if(!Number.isFinite(size)||size<0)size=0;
     out.push({name:name,type:type,size:size});
   }
   out.sort((a,b)=>{
     if(a.type!==b.type)return a.type==='dir'?-1:1;   // 目录在前、文件在后
     return compareName(a.name,b.name);
   });
   return out;
 }
 function compareName(a,b){
   try{return String(a).localeCompare(String(b),'zh-Hans-CN',{numeric:true,sensitivity:'base'})}
   catch(e){return String(a)<String(b)?-1:(String(a)>String(b)?1:0)}
 }

 async function loadStatus(){
   try{
     const d=await request('/api/sdcard');
     applyStatus(d||{});
     render();
     return true;
   }catch(e){
     if(e&&e.demo){applyDemo();return false}
     state.statusKnown=false;
     state.error=(e&&e.message)||'未知错误';
     render();
     showError('读取 SD 卡状态失败',e);
     return false;
   }
 }

 // 返回是否成功；失败时**不**改 state.path（避免把界面带到一个没读成功的目录）
 async function loadList(path){
   const target=normalizePath(path);
   try{
     const d=await request('/api/sdcard/list?path='+encodePath(target));
     if(!d||d.mounted===false){
       state.mounted=false;
       state.statusKnown=true;
       state.info=null;
       state.items=[];
       render();
       showMsg('sdMsg','warn','未检测到 SD 卡：请确认卡已插入卡槽，然后点「挂载」重试。');
       return false;
     }
     // 兼容省略 mounted 字段的响应：能返回 items 就按已挂载处理
     applyStatus(Object.assign({},d,{mounted:true}));
     state.path=(typeof d.path==='string'&&d.path)?normalizePath(d.path):target;
     state.items=normalizeItems(d.items);
     render();
     return true;
   }catch(e){
     if(e&&e.demo){applyDemo();return false}
     showError('读取文件列表失败',e);
     syncControls();
     return false;
   }
 }

 async function refreshAll(){
   if(state.demo){applyDemo();return}
   const ok=await loadStatus();
   if(ok&&state.mounted)await loadList(state.path);
   else if(!state.mounted){state.items=[];render();}
 }

 // ---------- 渲染 ----------
 function render(){
   const info=state.info||{card:'',mount:'/sdcard',total:0,used:0,free:0,hasCapacity:false};
   const mounted=state.mounted&&!state.demo;
   const known=state.statusKnown&&!state.demo;

   setChip($('sdChip'),state.demo?'演示环境':(known?(mounted?'已挂载':'未挂载'):'读取失败'),
     state.demo?'warn':(known?(mounted?'ok':'bad'):'bad'));
   setChip($('sdMountChip'),known?(mounted?'已挂载':'未挂载'):'未知',mounted?'ok':'bad');

   setText('sdCard',mounted&&info.card?info.card:'—');
   setText('sdMount',info.mount||'/sdcard');
   setText('sdTotal',mounted&&info.hasCapacity?fmtBytes(info.total):'—');
   setText('sdUsed',mounted&&info.hasCapacity?
     (fmtBytes(info.used)+(info.total?'（'+Math.min(100,Math.round(info.used/info.total*100))+'%）':'')):'—');
   setText('sdFree',mounted&&info.hasCapacity?fmtBytes(info.free):'—');
   const totalEl=$('sdTotal'),usedEl=$('sdUsed'),freeEl=$('sdFree');
   if(totalEl)totalEl.title=mounted&&info.hasCapacity?detail(info.total):'';
   if(usedEl)usedEl.title=mounted&&info.hasCapacity?detail(info.used):'';
   if(freeEl)freeEl.title=mounted&&info.hasCapacity?detail(info.free):'';

   const pct=(mounted&&info.hasCapacity)?Math.max(0,Math.min(100,info.used/info.total*100)):0;
   const bar=$('sdBar');
   if(bar)bar.style.width=pct.toFixed(1)+'%';
   const wrap=$('sdBarWrap');
   if(wrap){
     wrap.setAttribute('aria-valuenow',String(Math.round(pct)));
     wrap.setAttribute('aria-valuetext',(mounted&&info.hasCapacity)?
       ('已用 '+fmtBytes(info.used)+'，共 '+fmtBytes(info.total)):'容量未知');
   }

   const note=$('sdNote');
   if(note){
     let text='',cls='sd-note';
     if(state.demo){
       text=DEMO_TEXT;
       cls='sd-note err';
     }else if(!known){
       text='无法读取 SD 卡状态：'+(state.error||'未知错误')+'。请点「刷新」重试。';
       cls='sd-note err';
     }else if(!mounted){
       text='未检测到 SD 卡：请确认卡已插入卡槽，然后点「挂载」重试。文件管理暂不可用，抽奖与打印不受影响。';
     }else if(!info.hasCapacity){
       text='SD 卡已挂载，但容量信息不可用（可能不是 FAT 分区）。';
     }else{
       text='共 '+fmtBytes(info.total)+'，已用 '+fmtBytes(info.used)+'，可用 '+fmtBytes(info.free)+
         '。删除文件后无法恢复，请谨慎操作。';
     }
     if(note.textContent!==text)note.textContent=text;
     note.className=cls;
   }

   const fileCard=$('sdFileCard');
   if(fileCard)fileCard.hidden=!mounted;
   renderList();
   syncControls();
 }

 function renderList(){
   const body=$('sdRows');
   if(!body)return;
   body.replaceChildren();
   renderCrumb();
   const mounted=state.mounted&&!state.demo;
   if(!mounted){
     setText('sdCount','—');
     body.appendChild(emptyRow('SD 卡未挂载，文件列表不可用。'));
     return;
   }
   const items=state.items;
   if(!items.length){
     body.appendChild(emptyRow('当前目录为空，可以在上方选择文件上传，或新建文件夹。'));
   }else{
     items.forEach(it=>body.appendChild(rowFor(it)));
   }
   const dirs=items.filter(it=>it.type==='dir').length;
   setText('sdCount','共 '+items.length+' 项 · 目录 '+dirs+' · 文件 '+(items.length-dirs));
 }
 function emptyRow(text){
   const tr=document.createElement('tr');
   tr.className='sd-empty';
   const td=document.createElement('td');
   td.colSpan=4;
   td.className='empty-hint';
   td.textContent=text;
   tr.appendChild(td);
   return tr;
}

 function renderCrumb(){
   const box=$('sdCrumb');
   if(!box)return;
   box.replaceChildren();
   if(!(state.mounted&&!state.demo))return;
   const parts=state.path===ROOT?[]:state.path.split('/').filter(Boolean);
   const sep=()=>{
     const s=document.createElement('span');
     s.className='sd-sep';
     s.setAttribute('aria-hidden','true');
     s.textContent='/';
     box.appendChild(s);
   };
   const node=(label,path,current)=>{
     if(current){
       const span=document.createElement('span');
       span.className='sd-crumb-current';
       span.setAttribute('aria-current','page');
       span.textContent=label;
       box.appendChild(span);
       return;
     }
     const btn=document.createElement('button');
     btn.type='button';
     btn.className='sd-crumb-btn small-btn';
     btn.textContent=label;
     btn.title='打开 '+path;
     btn.addEventListener('click',()=>navigate(path));
     box.appendChild(btn);
   };
   node('根目录',ROOT,parts.length===0);
   parts.forEach((part,i)=>{
     sep();
     node(part,ROOT+parts.slice(0,i+1).join('/'),i===parts.length-1);
   });
 }

 function actButton(label,extraClass,title,onClick){
   const btn=document.createElement('button');
   btn.type='button';
   btn.className='small-btn'+(extraClass?' '+extraClass:'');
   btn.textContent=label;
   if(title)btn.title=title;
   btn.addEventListener('click',onClick);
   return btn;
 }
 function rowFor(item){
   const isDir=item.type==='dir';
   const name=String(item.name==null?'':item.name);
   const full=joinPath(state.path,name);
   const tr=document.createElement('tr');

   const nameTd=document.createElement('td');
   nameTd.dataset.label='名称';
   if(isDir){
     const btn=document.createElement('button');
     btn.type='button';
     btn.className='sd-name sd-dir';
     btn.innerHTML=ICON_DIR;                     // 静态图标
     const span=document.createElement('span');
     span.textContent=name;
     btn.appendChild(span);
     btn.title='进入目录 '+name;
     btn.setAttribute('aria-label','进入目录 '+name);
     btn.addEventListener('click',()=>navigate(full));
     nameTd.appendChild(btn);
   }else{
     const cell=document.createElement('span');
     cell.className='sd-name';
     cell.innerHTML=ICON_FILE;                   // 静态图标
     const span=document.createElement('span');
     span.textContent=name;
     cell.appendChild(span);
     cell.title=name;
     nameTd.appendChild(cell);
   }

   const typeTd=document.createElement('td');
   typeTd.dataset.label='类型';
   const type=document.createElement('span');
   type.className='sd-type';
   type.textContent=isDir?'目录':'文件';
   typeTd.appendChild(type);

   const sizeTd=document.createElement('td');
   sizeTd.dataset.label='大小';
   const size=document.createElement('span');
   size.className='sd-size';
   size.textContent=isDir?'—':fmtBytes(item.size);
   size.title=isDir?'':detail(item.size);
   sizeTd.appendChild(size);

   const actTd=document.createElement('td');
   actTd.dataset.label='操作';
   const acts=document.createElement('div');
   acts.className='sd-acts';
   if(isDir){
     acts.appendChild(actButton('进入','','进入目录 '+name,()=>navigate(full)));
   }else{
     const a=document.createElement('a');
     a.className='small-btn';
     a.textContent='下载';
     a.setAttribute('href','/api/sdcard/download?path='+encodePath(full));
     a.setAttribute('download',name);            // 触发浏览器下载（app.js 不会改写带 download 的链接）
     a.title='下载 '+name;
     a.setAttribute('aria-label','下载 '+name);
     acts.appendChild(a);
   }
   acts.appendChild(actButton('重命名','','重命名 '+name,()=>renameItem(item)));
   acts.appendChild(actButton('删除','danger','删除 '+name,()=>removeItem(item)));
   actTd.appendChild(acts);

   tr.append(nameTd,typeTd,sizeTd,actTd);
   return tr;
 }

 function syncControls(){
   const editable=state.mounted&&state.statusKnown&&!state.demo&&!state.busy;
   const mountBtn=$('sdMountBtn'),unmountBtn=$('sdUnmountBtn'),refreshBtn=$('sdRefreshBtn');
   if(mountBtn)mountBtn.disabled=state.busy||state.mounted||state.demo;
   if(unmountBtn)unmountBtn.disabled=state.busy||!state.mounted||state.demo;
   if(refreshBtn)refreshBtn.disabled=state.busy;
   const mkdir=$('sdMkdirBtn');
   if(mkdir)mkdir.disabled=!editable;
   const file=$('sdFile');
   if(file)file.disabled=!editable;
   const body=$('sdRows');
   if(body)body.querySelectorAll('button').forEach(btn=>{
     btn.disabled=state.busy||!(state.mounted&&!state.demo);
   });
   const crumb=$('sdCrumb');
   if(crumb)crumb.querySelectorAll('button').forEach(btn=>{btn.disabled=state.busy});
 }

 // ---------- 命名对话框（新建文件夹 / 重命名共用） ----------
 let pendingName=null;
 function askName(opts){
   const dlg=$('sdNameDialog'),input=$('sdNameInput');
   if(!dlg||!input||typeof dlg.showModal!=='function'){
     // 极老浏览器没有 <dialog>：退回 prompt，功能不中断
     const value=window.prompt(opts.title||'名称',opts.value||'');
     return Promise.resolve(value);
   }
   if(dlg.open)dlg.close();
   setText('sdNameTitle',opts.title||'名称');
   setText('sdNameHint',opts.message||'');
   setText('sdNameLabel',opts.label||'名称');
   const ok=$('sdNameOk');
   if(ok)ok.textContent=opts.confirm||'确定';
   input.value=opts.value||'';
   dlg.returnValue='';
   return new Promise(resolve=>{
     pendingName=resolve;
     dlg.showModal();
     setTimeout(()=>{try{input.focus();input.select()}catch(e){}},30);
   });
 }
 function settleName(value){
   const dlg=$('sdNameDialog'),resolve=pendingName;
   pendingName=null;
   if(dlg&&dlg.open)dlg.close();
   if(resolve)resolve(value);
 }
 function wireNameDialog(){
   const dlg=$('sdNameDialog');
   if(!dlg)return;
   dlg.addEventListener('cancel',e=>{e.preventDefault();settleName(null)});
   const cancel=$('sdNameCancel');
   if(cancel)cancel.addEventListener('click',()=>settleName(null));
   const form=$('sdNameForm');
   if(form)form.addEventListener('submit',e=>{e.preventDefault();settleName($('sdNameInput').value)});
 }

 // ---------- 操作 ----------
 async function busyRun(prefix,fn){
   if(state.busy)return false;
   state.busy=true;
   syncControls();
   try{
     await fn();
     return true;
   }catch(e){
     if(e&&e.demo)applyDemo();
     else showError(prefix,e);
     return false;
   }finally{
     state.busy=false;
     syncControls();
   }
 }

 async function navigate(path){
   if(state.busy)return;
   if(!state.mounted){needMounted();return}
   const target=normalizePath(path);
   state.busy=true;
   syncControls();
   try{
     const ok=await loadList(target);
     if(ok)hideMsg('sdMsg');
   }finally{
     state.busy=false;
     syncControls();
   }
 }

 async function createFolder(){
   if(needMounted())return;
   const name=await askName({
     title:'新建文件夹',
     message:'在「'+displayPath(state.path)+'」下创建文件夹。',
     label:'文件夹名称',
     confirm:'创建'
   });
   if(name===null)return;
   const clean=String(name).trim();
   const bad=nameError(clean,'文件夹名称');
   if(bad){showMsg('sdMsg','err',bad);return}
   const ok=await busyRun('新建文件夹失败',async()=>{
     await request('/api/sdcard/mkdir?path='+encodePath(state.path)+'&name='+encodeURIComponent(clean),
       {method:'POST'});
     showMsg('sdMsg','ok','已创建文件夹「'+clean+'」');
     toast('文件夹已创建');
     await loadList(state.path);
   });
   return ok;
 }

 async function renameItem(item){
   if(needMounted())return;
   const old=String((item&&item.name)||'');
   const next=await askName({
     title:'重命名',
     message:'重命名「'+old+'」：内容与位置不变，只改名称。',
     label:'新名称',
     value:old,
     confirm:'重命名'
   });
   if(next===null)return;
   const name=String(next).trim();
   if(name===old){showMsg('sdMsg','warn','名称没有变化');return}
   const bad=nameError(name,'新名称');
   if(bad){showMsg('sdMsg','err',bad);return}
   await busyRun('重命名失败',async()=>{
     await request('/api/sdcard/rename',{method:'POST',json:{path:joinPath(state.path,old),name:name}});
     showMsg('sdMsg','ok','已重命名为「'+name+'」');
     toast('已重命名');
     await loadList(state.path);
   });
 }

 async function removeItem(item){
   if(needMounted())return;
   const isDir=item.type==='dir';
   const name=String(item.name==null?'':item.name);
   const ok=await L.confirm(isDir?'删除目录？':'删除文件？',
     (isDir?'目录「':'文件「')+name+'」将从 SD 卡永久删除，无法恢复。'+
       (isDir?'目录必须为空才能删除。':''),
     '删除');
   if(!ok)return;
   await busyRun('删除失败',async()=>{
     await request('/api/sdcard/file?path='+encodePath(joinPath(state.path,name)),{method:'DELETE'});
     showMsg('sdMsg','ok','已删除'+(isDir?'目录':'文件')+'「'+name+'」');
     toast('已删除');
     await loadList(state.path);
   });
 }

 async function uploadFiles(fileList){
   const files=Array.prototype.slice.call(fileList||[]);
   const input=$('sdFile');
   if(!files.length)return;
   if(needMounted())return;
   state.busy=true;
   syncControls();
   let ok=0;
   const fails=[];
   try{
     for(let i=0;i<files.length;i++){
       const file=files[i];
       const bad=nameError(file.name,'文件名');
       if(bad){
         fails.push(file.name+'（'+bad+'）');
         continue;
       }
       showMsg('sdMsg','ok','正在上传 '+(i+1)+'/'+files.length+'：'+file.name);
       try{
         await request('/api/sdcard/upload?path='+encodePath(state.path)+
           '&name='+encodeURIComponent(file.name),
           {method:'POST',body:file,timeout:120000});
         ok++;
       }catch(e){
         if(e&&e.demo){applyDemo();return}
         fails.push(file.name+'（'+((e&&e.message)||'上传失败')+'）');
       }
     }
     if(fails.length){
       showMsg('sdMsg','err','已上传 '+ok+' 个文件，'+fails.length+' 个失败：'+
         fails.slice(0,3).join('；')+(fails.length>3?' 等':''));
       toast('部分文件上传失败','err');
     }else{
       showMsg('sdMsg','ok','已上传 '+ok+' 个文件到「'+displayPath(state.path)+'」');
       toast('已上传 '+ok+' 个文件');
     }
     await loadList(state.path);
   }finally{
     if(input)input.value='';
     state.busy=false;
     syncControls();
   }
 }

 async function mountCard(on){
   if(state.busy)return;
   if(state.demo){demoNotice();return}
   state.busy=true;
   syncControls();
   hideMsg('sdMsg');
   try{
     const d=await request('/api/sdcard/'+(on?'mount':'unmount'),{method:'POST',timeout:20000});
     const mounted=(d&&typeof d.mounted==='boolean')?d.mounted:on;
     if(on&&mounted){
       showMsg('sdMsg','ok','SD 卡已挂载');
       toast('SD 卡已挂载');
       await refreshAll();
     }else if(on){
       state.mounted=false;
       state.statusKnown=true;
       state.info=null;
       state.items=[];
       render();
       showMsg('sdMsg','err','挂载失败：未检测到 SD 卡，请确认卡已插好再重试。');
       toast('挂载失败','err');
     }else if(!mounted){
       state.mounted=false;
       state.statusKnown=true;
       state.info=null;
       state.items=[];
       render();
       showMsg('sdMsg','ok','已卸载 SD 卡；插卡后可再次挂载。');
       toast('已卸载 SD 卡','warn');
     }else{
       showMsg('sdMsg','err','卸载失败：卡仍在使用（可能有文件正在读写），请稍后重试。');
       toast('卸载失败','err');
     }
   }catch(e){
     if(e&&e.demo)applyDemo();
     else showError(on?'挂载失败':'卸载失败',e);
   }finally{
     state.busy=false;
     syncControls();
   }
 }

 async function refreshClick(){
   if(state.busy)return;
   hideMsg('sdMsg');
   if(state.demo){applyDemo();return}
   state.busy=true;
   syncControls();
   try{
     await refreshAll();
   }catch(e){
     showError('刷新失败',e);
   }finally{
     state.busy=false;
     syncControls();
   }
 }

 // ---------- 启动 ----------
 function wire(){
   wireNameDialog();
   const mount=$('sdMountBtn');
   if(mount)mount.addEventListener('click',()=>mountCard(true));
   const unmount=$('sdUnmountBtn');
   if(unmount)unmount.addEventListener('click',()=>mountCard(false));
   const refresh=$('sdRefreshBtn');
   if(refresh)refresh.addEventListener('click',refreshClick);
   const mkdir=$('sdMkdirBtn');
   if(mkdir)mkdir.addEventListener('click',createFolder);
   const file=$('sdFile');
   if(file)file.addEventListener('change',e=>{uploadFiles(e.target.files)});
 }

 async function boot(){
   wire();
   if(state.demo){
     applyDemo();
     return;
   }
   state.busy=true;
   syncControls();
   try{
     await refreshAll();
   }catch(e){
     showError('读取 SD 卡状态失败',e);
   }finally{
     state.busy=false;
     syncControls();
   }
 }

 // 页面自诊断：任何未捕获异常都在页面内可见，现场排障不必连电脑
 window.addEventListener('error',e=>{
   if(e&&e.message)showMsg('sdMsg','err','页面脚本错误：'+e.message);
 });
 window.addEventListener('unhandledrejection',e=>{
   const reason=e&&e.reason;
   showMsg('sdMsg','err','操作未完成：'+((reason&&reason.message)||'未知错误')+'，请点「刷新」重试。');
 });

 boot();
})();
