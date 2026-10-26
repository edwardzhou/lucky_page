/* sound.js — 自定义音效（欢迎 / 中奖 / 落空）：状态、试听、上传、恢复默认。
   接口：GET /api/sounds、POST/DELETE /api/sounds?role=、GET /api/sounds/raw?role=、
   POST /api/sounds/play?role=。
   上传流程（关键）：浏览器 AudioContext 解码任意音频 → 校验 ≤4 秒 →
   OfflineAudioContext(1, ceil(duration*24000), 24000) 渲染成 24kHz 单声道 →
   手工编码 16bit PCM WAV（44 字节头）→ POST /api/sounds?role=...
   设备只收 WAV，固件因此不需要 MP3/OGG 解码器。 */
(function(){
 'use strict';
 const L=window.Lucky,$=id=>document.getElementById(id);
 const ROLES=[{key:'welcome',name:'欢迎音'},{key:'win',name:'中奖音效'},{key:'lose',name:'落空音效'}];
 const MAX_SECONDS=4;                 // 与 sound_decode::kMaxSeconds 一致
 const RATE=24000;                    // 与 sound_store::kRate 一致
 const MAX_FILE_BYTES=16*1024*1024;   // 仅防超大文件撑爆手机解码器；有效上限仍是 4 秒
 const state={ready:false,busy:false,roles:{}};

 // ---------- 小工具 ----------
 const roleName=key=>(ROLES.find(r=>r.key===key)||{}).name||key;
 const fmtMs=ms=>Math.round(Number(ms)||0)+' ms';
 const fmtBytes=n=>{n=Math.max(0,Math.round(Number(n)||0));return n<1024?n+' B':(n/1024).toFixed(1)+' KB'};
 // 状态行额外带上精确字节数（接口 bytes 为 PCM 字节）
 const fmtSize=n=>{n=Math.max(0,Math.round(Number(n)||0));return n<1024?n+' B':(n/1024).toFixed(1)+' KB ('+n+' B)'};
 const fmtSec=s=>String(Math.round((Number(s)||0)*1000)/1000);
 const cardMsg=(role,kind,text)=>showMsg('msg-'+role,kind,text);
 const hideCardMsg=role=>hideMsg('msg-'+role);
 const hasFile=role=>{const el=$('file-'+role);return !!(el&&el.files&&el.files.length)};

 // ---------- 状态渲染 ----------
 function render(){
   const custom=ROLES.filter(r=>state.ready&&state.roles[r.key]&&state.roles[r.key].custom).length;
   const chip=$('soundChip');
   if(!state.ready){chip.textContent='音效不可用';chip.className='chip bad'}
   else{chip.textContent='自定义 '+custom+' / '+ROLES.length;chip.className='chip'+(custom?' ok':'')}
   ROLES.forEach(r=>{
     const c=state.ready?state.roles[r.key]:null;
     const chipEl=$('chip-'+r.key),dur=$('dur-'+r.key),bytes=$('bytes-'+r.key);
     if(!c){
       chipEl.textContent='不可用';chipEl.className='chip bad';
       dur.textContent='—';bytes.textContent='—';
     }else if(c.custom){
       chipEl.textContent='自定义';chipEl.className='chip ok';
       dur.textContent=fmtMs(c.duration_ms);bytes.textContent=fmtSize(c.bytes);
     }else{
       chipEl.textContent='默认音效';chipEl.className='chip';
       dur.textContent='内置默认';bytes.textContent='—';
     }
     $('upload-'+r.key).textContent=c&&c.custom?'更换音效':'上传音效';
   });
   syncControls();
 }
 function syncControls(){
   const canEdit=state.ready&&!state.busy&&!L.demo;
   ROLES.forEach(r=>{
     const c=state.ready?state.roles[r.key]:null;
     $('file-'+r.key).disabled=!canEdit;
     $('preview-'+r.key).disabled=!canEdit||!c;
     $('upload-'+r.key).disabled=!canEdit||!c||!hasFile(r.key);
     $('restore-'+r.key).disabled=!canEdit||!c||!c.custom;
   });
   $('refreshBtn').disabled=state.busy;
 }

 // ---------- 读取 ----------
 async function load(){
   if(state.busy)return;
   state.busy=true;syncControls();
   hideMsg('soundMsg');
   $('soundChip').textContent='读取中…';$('soundChip').className='chip';
   try{
     const d=await L.api('/api/sounds');
     const list=Array.isArray(d.roles)?d.roles:[];
     const missing=[];
     state.roles={};
     ROLES.forEach(r=>{
       const it=list.find(x=>x&&x.role===r.key);
       if(!it){missing.push(r.name);return}
       state.roles[r.key]={custom:!!it.custom,
         duration_ms:Number(it.duration_ms)||0,bytes:Number(it.bytes)||0};
     });
     state.ready=true;
     if(missing.length)
       showMsg('soundMsg','warn','接口没有返回 '+missing.join('、')+' 的状态，这几项显示为「不可用」；请点「刷新」重试。');
   }catch(e){
     state.ready=false;state.roles={};
     if(L.demo){
       showMsg('soundMsg','warn','演示环境不支持音效上传与试听：音效文件保存在设备上，?demo=1 下相关接口返回 501（'+
         e.message+'）。请连接设备后操作。');
     }else{
       showMsg('soundMsg','err','音效状态读取失败：'+e.message+
         '。为避免把自定义音效误当成默认音效，上传、试听与恢复默认已暂停；请点「刷新」重试。');
     }
   }
   state.busy=false;render();
 }

 // ---------- 浏览器侧转码：任意音频 → 24kHz 单声道 16bit WAV ----------
 function decodeAudioData(ac,buf){
   return new Promise((resolve,reject)=>{
     let done=false;
     const ok=b=>{if(!done){done=true;resolve(b)}};
     const bad=e=>{if(!done){done=true;reject(e||new Error('解码失败'))}};
     let p;
     try{p=ac.decodeAudioData(buf,ok,bad)}catch(e){bad(e);return}
     if(p&&typeof p.then==='function')p.then(ok,bad);
   });
 }
 function encodeWav(samples,rate){
   const dataBytes=samples.length*2;
   const out=new Uint8Array(44+dataBytes);
   const view=new DataView(out.buffer);
   const tag=(off,text)=>{for(let i=0;i<text.length;i++)out[off+i]=text.charCodeAt(i)};
   tag(0,'RIFF');view.setUint32(4,36+dataBytes,true);tag(8,'WAVE');
   tag(12,'fmt ');view.setUint32(16,16,true);view.setUint16(20,1,true);view.setUint16(22,1,true);
   view.setUint32(24,rate,true);view.setUint32(28,rate*2,true);
   view.setUint16(32,2,true);view.setUint16(34,16,true);
   tag(36,'data');view.setUint32(40,dataBytes,true);
   for(let i=0;i<samples.length;i++){
     const s=Math.max(-1,Math.min(1,samples[i]));
     view.setInt16(44+i*2,s<0?s*0x8000:s*0x7FFF,true);
   }
   return out;
 }
 async function transcode(file){
   if(file.size>MAX_FILE_BYTES)
     throw Error('音频文件过大（'+fmtBytes(file.size)+'），请先裁剪到 4 秒以内再上传');
   const Ctor=window.AudioContext||window.webkitAudioContext;
   if(!Ctor)throw Error('当前浏览器不支持 Web Audio，无法转码');
   let buffer;
   const ac=new Ctor();
   try{
     buffer=await decodeAudioData(ac,await file.arrayBuffer());
   }catch(e){
     throw Error('无法解码这段音频（浏览器不支持该格式），请换 WAV / MP3 / OGG 后再试');
   }finally{
     try{await ac.close()}catch{}
   }
   const seconds=buffer.duration;
   if(!Number.isFinite(seconds)||seconds<=0)throw Error('音频时长无效');
   if(seconds>MAX_SECONDS)throw Error('音频最长 4 秒，当前 '+fmtSec(seconds)+' 秒');
   const frames=Math.max(1,Math.ceil(seconds*RATE));
   const Off=window.OfflineAudioContext||window.webkitOfflineAudioContext;
   if(!Off)throw Error('当前浏览器不支持离线音频渲染');
   let data;
   try{
     const oc=new Off(1,frames,RATE);
     const src=oc.createBufferSource();
     src.buffer=buffer;src.connect(oc.destination);src.start(0);
     const rendered=await oc.startRendering();
     data=rendered.getChannelData(0);
   }catch(e){
     throw Error('音频渲染失败：'+(e&&e.message?e.message:'未知错误'));
   }
   if(!data||!data.length)throw Error('音频为空');
   return {bytes:encodeWav(data,RATE),duration_ms:Math.round(data.length/RATE*1000)};
 }

 // ---------- 试听（必须由用户点击触发，不自动播放） ----------
 const audio=new Audio();
 audio.preload='auto';
 audio.setAttribute('aria-hidden','true');
 audio.style.display='none';
 document.body.appendChild(audio);
 let audioUrl='';
 function stopAudio(){
   try{audio.pause();audio.currentTime=0}catch{}
   audio.removeAttribute('src');
   if(audioUrl){URL.revokeObjectURL(audioUrl);audioUrl=''}
 }
 async function playLocal(blob){
   stopAudio();
   audioUrl=URL.createObjectURL(blob);
   audio.src=audioUrl;
   try{
     await audio.play();
   }catch(e){
     throw Error(e&&e.name==='NotAllowedError'
       ?'浏览器拦截了播放，请再点一次「试听」'
       :(e&&e.message)||'手机无法播放这段音频');
   }
 }
 async function preview(role){
   if(state.busy||!state.ready)return;
   const c=state.roles[role];
   if(!c)return;
   hideCardMsg(role);
   stopAudio();                         // 切换角色试听时先停掉上一段
   const ctrl=new AbortController(),timer=setTimeout(()=>ctrl.abort(),8000);
   try{
     let r;
     try{
       r=await fetch('/api/sounds/raw?role='+encodeURIComponent(role),
         {cache:'no-store',signal:ctrl.signal});
     }finally{clearTimeout(timer)}
     if(r.ok){
       const blob=await r.blob();
       if(!blob.size)throw Error('设备返回的音效为空');
       await playLocal(blob);
       cardMsg(role,'ok','正在手机上试听自定义'+roleName(role)+'（'+fmtBytes(blob.size)+'）。');
       return;
     }
     let d=null;try{d=await r.json()}catch{}
     if(r.status!==404)throw Error(d&&d.error?d.error:'HTTP '+r.status);
     // 无自定义（404）→ 让设备喇叭播放默认音效
     await L.api('/api/sounds/play?role='+encodeURIComponent(role),'POST');
     cardMsg(role,'ok','没有自定义音效，已让设备喇叭播放默认'+roleName(role)+'。');
     toast('设备正在播放默认音效');
   }catch(e){
     cardMsg(role,'err','试听失败：'+(e.name==='AbortError'?'请求超时（设备未响应）':e.message));
   }
 }

 // ---------- 上传 ----------
 async function upload(role){
   if(state.busy||!state.ready)return;
   const el=$('file-'+role);
   const file=el.files&&el.files[0];
   if(!file){cardMsg(role,'warn','请先选择音频文件');return}
   state.busy=true;syncControls();hideCardMsg(role);
   try{
     cardMsg(role,'warn','正在解码并转成 24kHz 单声道…');
     const wav=await transcode(file);
     cardMsg(role,'warn','已转码 '+fmtMs(wav.duration_ms)+' · '+fmtBytes(wav.bytes.length)+'，正在上传…');
     const ctrl=new AbortController(),timer=setTimeout(()=>ctrl.abort(),20000);
     let r;
     try{
       r=await fetch('/api/sounds?role='+encodeURIComponent(role),
         {method:'POST',headers:{'Content-Type':'audio/wav'},body:wav.bytes,signal:ctrl.signal});
     }finally{clearTimeout(timer)}
     let d=null;try{d=await r.json()}catch{}
     if(!r.ok||!d||d.error)throw Error(d&&d.error?d.error:'HTTP '+r.status);
     el.value='';
     hideCardMsg(role);
     state.busy=false;
     await load();                       // 刷新时长/字节等真实状态
     cardMsg(role,'ok','已保存到设备：'+fmtMs(wav.duration_ms)+' · '+fmtBytes(wav.bytes.length)+
       '。之后会自动使用这段自定义'+roleName(role)+'。');
     toast(roleName(role)+'已上传');
   }catch(e){
     cardMsg(role,'err','上传失败：'+(e.name==='AbortError'?'请求超时（设备未响应）':e.message));
     toast('上传失败','err');
   }finally{
     if(state.busy){state.busy=false;render()}else syncControls();
   }
 }

 // ---------- 恢复默认 ----------
 async function restore(role){
   if(state.busy||!state.ready)return;
   const c=state.roles[role];
   if(!c||!c.custom)return;
   if(!await L.confirm('恢复默认'+roleName(role)+'？',
     '设备上的自定义音频会被删除，之后播放内置的默认'+roleName(role)+'。','恢复默认'))return;
   state.busy=true;syncControls();hideCardMsg(role);
   try{
     await L.api('/api/sounds?role='+encodeURIComponent(role),'DELETE');
     state.busy=false;
     await load();
     cardMsg(role,'ok','已恢复默认'+roleName(role)+'（自定义音频已删除）。');
     toast('已恢复默认音效');
   }catch(e){
     cardMsg(role,'err','恢复默认失败：'+e.message);
     toast('恢复默认失败','err');
   }finally{
     if(state.busy){state.busy=false;render()}else syncControls();
   }
 }

 // ---------- 事件 ----------
 ROLES.forEach(r=>{
   $('file-'+r.key).addEventListener('change',()=>{hideCardMsg(r.key);syncControls()});
   $('upload-'+r.key).addEventListener('click',()=>upload(r.key));
   $('preview-'+r.key).addEventListener('click',()=>preview(r.key));
   $('restore-'+r.key).addEventListener('click',()=>restore(r.key));
 });
 $('refreshBtn').addEventListener('click',()=>load());
 window.addEventListener('pagehide',stopAudio);

 render();
 load();
})();
