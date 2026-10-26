/* pages.js — 页面内容：每页打印文字 + 图片素材，以及素材库上传/删除。
   接口：GET /api/pages、GET/PUT /api/pages/content、GET/POST/DELETE /api/assets。
   浏览器侧把图片转成 1-bit LGO1（与 logo 页同一套二值化逻辑），设备只存原始栅格。 */
(function(){
 'use strict';
 const L=window.Lucky,$=id=>document.getElementById(id);
 const MAX_TEXT=128;              // 与固件 page_content::kMaxTextBytes 一致
 const MAX_W=576,MAX_H=672;       // 与 PrinterLogo 的素材尺寸上限一致
 const NAME_BYTES=24;             // 与 asset_store::kMaxNameBytes 一致
 const state={totalPages:30,items:new Map(),assets:[],thumbs:new Map(),rows:new Map(),
   prepared:null,pagesReady:false,assetsReady:false,busy:false};

 // ---------- 小工具 ----------
 const bytesOf=s=>new TextEncoder().encode(String(s)).length;
 function cutBytes(s,max){let out='';for(const ch of Array.from(String(s))){if(bytesOf(out+ch)>max)break;out+=ch}return out}
 function formatBytes(n){n=Number(n)||0;return n<1024?n+' B':(n/1024).toFixed(1)+' KB'}
 function assetLabel(a){return a&&a.name?a.name:('素材 '+(Number(a&&a.id)+1))}
 function showPages(kind,text){showMsg('pagesMsg',kind,text)}
 function showAssets(kind,text){showMsg('assetsMsg',kind,text)}

 // ---------- 表格 ----------
 function itemFor(page){return state.items.get(page)||{text:'',asset:-1}}
 function fillSelect(sel,value){
   sel.replaceChildren(new Option('无图片','-1'));
   state.assets.forEach(a=>sel.appendChild(new Option(assetLabel(a),String(a.id))));
   if(value!=='-1'&&!state.assets.some(a=>String(a.id)===value))
     sel.appendChild(new Option('素材 #'+(Number(value)+1)+'（不可用）',value));
   sel.value=value;
 }
 function rowOf(el){const tr=el.closest('tr');return tr?state.rows.get(Number(tr.dataset.page)):null}
 function rowDirty(row){return row.input.value!==row.saved.text||Number(row.select.value)!==row.saved.asset}
 function anyDirty(){return Array.from(state.rows.values()).some(rowDirty)}
 function dirtyPages(){return Array.from(state.rows.values()).filter(rowDirty).map(r=>r.page).sort((a,b)=>a-b)}

 function updateRow(row){
   const editable=state.pagesReady&&!state.busy;
   const dirty=rowDirty(row);
   row.tr.classList.toggle('dirty',dirty);
   row.input.disabled=!editable;
   row.select.disabled=!editable;
   row.btn.disabled=!editable||!dirty;
 }
 function updateDirtyUi(){
   const n=dirtyPages().length;
   $('unsavedFlag').classList.toggle('show',n>0);
   $('saveAllBtn').disabled=state.busy||!state.pagesReady||n===0;
 }
 function updateStatus(){
   $('pageChip').textContent=state.pagesReady?('共 '+state.totalPages+' 页'):'页面读取失败';
   $('pageChip').className='chip'+(state.pagesReady?'':' bad');
   $('assetChip').textContent=state.assetsReady?('素材 '+state.assets.length+' 张'):'素材读取失败';
   $('assetChip').className='chip'+(state.assetsReady?'':' bad');
 }
 function updateHint(){
   if(!state.pagesReady){
     $('pagesHint').textContent='页面内容未能完整读取，已暂停编辑以免覆盖设备里已有的内容；请点「刷新」重试。';
     return;
   }
   const setCount=Array.from(state.items.values()).filter(it=>it.text||it.asset>=0).length;
   $('pagesHint').textContent='共 '+state.totalPages+' 页 · 已设置 '+setCount+
     ' 页。文字上限 128 字节（约 42 个汉字）；未设置的页会跳过模板里的「页面文字 / 页面图片」元素。';
 }
 function setBusy(){
   $('refreshBtn').disabled=state.busy;
   $('assetFile').disabled=state.busy||!state.assetsReady;
   $('uploadBtn').disabled=state.busy||!state.assetsReady||!state.prepared;
   $('assetList').querySelectorAll('button').forEach(b=>b.disabled=state.busy);
   state.rows.forEach(updateRow);
   updateDirtyUi();
 }

 function renderRows(){
   const body=$('pageRows');
   body.replaceChildren();
   state.rows.clear();
   for(let page=0;page<state.totalPages;page++){
     const item=itemFor(page);
     const tr=document.createElement('tr');tr.dataset.page=String(page);
     const no=document.createElement('td');no.dataset.label='页码';
     const idx=document.createElement('span');idx.className='page-index';idx.textContent='P'+(page+1);
     no.appendChild(idx);
     const textTd=document.createElement('td');textTd.dataset.label='文字';
     const input=document.createElement('input');
     input.type='text';input.className='page-text';input.value=item.text||'';
     input.placeholder='不设置则跳过「页面文字」';input.autocomplete='off';
     input.setAttribute('aria-label','第 '+(page+1)+' 页打印文字');
     textTd.appendChild(input);
     const assetTd=document.createElement('td');assetTd.dataset.label='图片';
     const select=document.createElement('select');select.className='page-asset';
     select.setAttribute('aria-label','第 '+(page+1)+' 页图片素材');
     fillSelect(select,String(item.asset>=0?item.asset:-1));
     assetTd.appendChild(select);
     const previewTd=document.createElement('td');
     previewTd.dataset.label='预览';previewTd.className='page-preview';
     const opTd=document.createElement('td');opTd.dataset.label='操作';
     const btn=document.createElement('button');
     btn.type='button';btn.className='small-btn';btn.textContent='保存本页';
     opTd.appendChild(btn);
     tr.append(no,textTd,assetTd,previewTd,opTd);
     body.appendChild(tr);
     const row={page,tr,input,select,preview:previewTd,btn,
       saved:{text:item.text||'',asset:item.asset>=0?item.asset:-1}};
     state.rows.set(page,row);
     btn.addEventListener('click',()=>savePages([page]));
     refreshPreview(row);
     updateRow(row);
   }
   updateDirtyUi();
 }

 $('pageRows').addEventListener('input',e=>{
   if(!e.target.classList.contains('page-text'))return;
   const row=rowOf(e.target);
   if(!row)return;
   updateRow(row);updateDirtyUi();
 });
 $('pageRows').addEventListener('change',e=>{
   if(!e.target.classList.contains('page-asset'))return;
   const row=rowOf(e.target);
   if(!row)return;
   refreshPreview(row);updateRow(row);updateDirtyUi();
 });

 // ---------- 缩略图（LGO1 解码） ----------
 function lgoToDataUrl(b){
   if(!b||b.length<8)throw Error('数据过短');
   if(String.fromCharCode(b[0],b[1],b[2],b[3])!=='LGO1')throw Error('缺少 LGO1 头');
   const w=b[4]|b[5]<<8,h=b[6]|b[7]<<8;
   if(!w||!h||w%8||w>MAX_W||h>MAX_H)throw Error('尺寸无效');
   const stride=w>>3;
   if(b.length<8+stride*h)throw Error('数据不完整');
   // 缩略图最多 192 点宽：列表/行内都只有几十像素，没必要按 576×672 原尺寸编码
   const tw=Math.max(1,Math.min(192,w)),th=Math.max(1,Math.round(h*tw/w));
   const cv=document.createElement('canvas');cv.width=tw;cv.height=th;
   const ctx=cv.getContext('2d'),img=ctx.createImageData(tw,th);
   for(let y=0;y<th;y++){
     const sy=Math.min(h-1,Math.floor(y*h/th));
     for(let x=0;x<tw;x++){
       const sx=Math.min(w-1,Math.floor(x*w/tw));
       const on=(b[8+sy*stride+(sx>>3)]>>(7-(sx&7)))&1,i=(y*tw+x)*4,v=on?0:255;
       img.data[i]=img.data[i+1]=img.data[i+2]=v;img.data[i+3]=255;
     }
   }
   ctx.putImageData(img,0,0);
   return cv.toDataURL('image/png');
 }
 async function fetchThumb(id){
   const ctrl=new AbortController(),timer=setTimeout(()=>ctrl.abort(),8000);
   try{
     const r=await fetch('/api/assets/raw?id='+id,{signal:ctrl.signal});
     if(!r.ok)throw Error('HTTP '+r.status);
     return lgoToDataUrl(new Uint8Array(await r.arrayBuffer()));
   }finally{clearTimeout(timer)}
 }
 function assetThumb(id){
   if(!state.thumbs.has(id))
     state.thumbs.set(id,fetchThumb(id).catch(()=>null));
   return state.thumbs.get(id);
 }
 function refreshPreview(row){
   const asset=Number(row.select.value);
   row.preview.replaceChildren();
   const none=document.createElement('span');
   none.className='thumb-none';none.textContent=asset<0?'无图':'读取中…';
   row.preview.appendChild(none);
   if(asset<0)return;
   assetThumb(asset).then(url=>{
     if(Number(row.select.value)!==asset)return;   // 期间又改了选择
     if(!url){none.textContent='无法预览';return}
     const img=document.createElement('img');
     img.className='page-thumb';img.alt='第 '+(row.page+1)+' 页图片预览';img.src=url;
     row.preview.replaceChildren(img);
   });
 }

 // ---------- 素材库 ----------
 function normalizeAssets(slots){
   return (Array.isArray(slots)?slots:[])
     .filter(s=>s&&Number.isInteger(Number(s.id)))
     .map(s=>({id:Number(s.id),name:String(s.name||''),width:Number(s.width)||0,
       height:Number(s.height)||0,bytes:Number(s.bytes)||0}))
     .sort((a,b)=>a.id-b.id);
 }
 function renderAssets(){
   const list=$('assetList');
   list.replaceChildren();
   if(!state.assetsReady){
     const hint=document.createElement('div');hint.className='empty-hint';
     hint.textContent='素材库读取失败，暂不能上传或删除。请点「刷新」重试。';
     list.appendChild(hint);return;
   }
   if(!state.assets.length){
     const hint=document.createElement('div');hint.className='empty-hint';
     hint.textContent='素材库为空，上传 PNG / JPEG 后即可在页面里选用。';
     list.appendChild(hint);return;
   }
   state.assets.forEach(a=>{
     const row=document.createElement('div');row.className='asset-row';
     const box=document.createElement('div');box.className='asset-thumb';
     const img=document.createElement('img');img.alt=assetLabel(a);img.style.display='none';
     const none=document.createElement('span');none.className='thumb-none';none.textContent='…';
     box.append(img,none);
     assetThumb(a.id).then(url=>{
       if(!url){none.textContent='无法预览';return}
       img.src=url;img.style.display='block';none.remove();
     });
     const meta=document.createElement('div');
     const name=document.createElement('strong');name.className='asset-name';name.textContent=assetLabel(a);
     const desc=document.createElement('span');desc.className='asset-desc';
     desc.textContent=(a.width||'?')+'×'+(a.height||'?')+' 点 · '+formatBytes(a.bytes);
     meta.append(name,desc);
     const del=document.createElement('button');
     del.type='button';del.className='small-btn danger';del.textContent='删除';
     del.disabled=state.busy;
     del.addEventListener('click',()=>removeAsset(a));
     row.append(box,meta,del);
     list.appendChild(row);
   });
 }
 function syncSelects(){
   state.rows.forEach(row=>{
     const value=row.select.value;
     fillSelect(row.select,value);
     refreshPreview(row);
     updateRow(row);
   });
   updateDirtyUi();
 }
 async function refreshAssets(){
   try{
     state.assets=normalizeAssets((await L.api('/api/assets')).slots);
     state.assetsReady=true;
     renderAssets();syncSelects();updateStatus();
   }catch(e){
     state.assetsReady=false;
     renderAssets();updateStatus();
     showAssets('err','素材库刷新失败：'+e.message);
   }
 }
 async function removeAsset(a){
   if(state.busy||!state.assetsReady)return;
   if(!await L.confirm('删除素材？','「'+assetLabel(a)+'」将从素材库移除；使用它的页面会变成「无图片」，需要保存对应页面内容。','删除素材'))return;
   state.busy=true;setBusy();
   try{
     await L.api('/api/assets?id='+a.id,'DELETE');
   }catch(e){
     state.busy=false;setBusy();
     showAssets('err','删除失败：'+e.message);toast('删除失败','err');return;
   }
   state.assets=state.assets.filter(x=>x.id!==a.id);
   state.thumbs.delete(a.id);
   let affected=0;
   state.rows.forEach(row=>{
     if(Number(row.select.value)===a.id){row.select.value='-1';affected++;refreshPreview(row);updateRow(row)}
   });
   renderAssets();syncSelects();updateStatus();
   state.busy=false;setBusy();
   showAssets('ok','素材已删除'+(affected?'，'+affected+' 页的图片已设为「无图片」，请保存这些页面':''));
   toast('素材已删除',affected?'warn':'ok');
 }

 // ---------- 图片转 LGO1 ----------
 function buildBinary(img){
   const nw=img.naturalWidth||img.width,nh=img.naturalHeight||img.height;
   if(!nw||!nh)throw Error('图片尺寸无效');
   const scale=Math.min(1,MAX_W/nw,MAX_H/nh);
   const w=Math.max(8,Math.min(MAX_W,Math.floor(nw*scale/8)*8));
   const h=Math.max(1,Math.min(MAX_H,Math.round(nh*scale)));
   const cv=document.createElement('canvas');cv.width=w;cv.height=h;
   const ctx=cv.getContext('2d');
   ctx.fillStyle='#fff';ctx.fillRect(0,0,w,h);
   ctx.imageSmoothingEnabled=true;ctx.imageSmoothingQuality='high';
   ctx.drawImage(img,0,0,w,h);
   const data=ctx.getImageData(0,0,w,h),px=data.data,err=new Float32Array(w*h);
   for(let y=0;y<h;y++)for(let x=0;x<w;x++){
     const i=(y*w+x)*4;
     const gray=0.299*px[i]+0.587*px[i+1]+0.114*px[i+2]+err[y*w+x];
     const on=gray<128;                       // 暗 = 黑点（热敏打印）
     const v=on?0:255,diff=gray-v;
     px[i]=px[i+1]=px[i+2]=v;
     if(x+1<w)err[y*w+x+1]+=diff*7/16;        // Floyd–Steinberg 抖动
     if(y+1<h){const r=(y+1)*w;
       if(x>0)err[r+x-1]+=diff*3/16;
       err[r+x]+=diff*5/16;
       if(x+1<w)err[r+x+1]+=diff*1/16;}
   }
   ctx.putImageData(data,0,0);
   const stride=w>>3,bytes=new Uint8Array(8+stride*h);
   bytes.set([0x4C,0x47,0x4F,0x31],0);        // "LGO1"
   bytes[4]=w&255;bytes[5]=w>>8;bytes[6]=h&255;bytes[7]=h>>8;
   for(let y=0;y<h;y++)for(let x=0;x<w;x++)
     if(px[(y*w+x)*4]===0)bytes[8+y*stride+(x>>3)]|=0x80>>(x&7);   // MSB 在左
   return {w,h,bytes,canvas:cv};
 }
 function showPreview(){
   const p=state.prepared,canvas=$('assetCanvas'),empty=$('assetEmpty');
   if(!p){
     canvas.style.display='none';empty.style.display='block';
     $('assetInfo').textContent='';
     return;
   }
   canvas.width=p.w;canvas.height=p.h;
   canvas.getContext('2d').drawImage(p.canvas,0,0);
   canvas.style.display='block';empty.style.display='none';
   $('assetInfo').textContent=p.w+'×'+p.h+' 点 · '+formatBytes(p.bytes.length);
 }
 function clearPrepared(){
   state.prepared=null;
   $('assetFile').value='';
   showPreview();setBusy();
 }
 function prepareImage(file){
   if(!file)return;
   const typeOk=file.type?/^image\/(png|jpe?g)$/i.test(file.type):/\.(png|jpe?g)$/i.test(file.name);
   if(!typeOk){showAssets('err','请选择 PNG 或 JPEG 图片');return}
   const url=URL.createObjectURL(file),img=new Image();
   img.onload=()=>{
     try{
       state.prepared=buildBinary(img);
       $('assetName').value=cutBytes(file.name.replace(/\.[^.]+$/,''),NAME_BYTES);
       showPreview();setBusy();
       showAssets('ok','已生成黑白点阵，确认名称后点「上传素材」。');
     }catch(e){
       state.prepared=null;showPreview();setBusy();
       showAssets('err','图片处理失败：'+e.message);
     }finally{URL.revokeObjectURL(url)}
   };
   img.onerror=()=>{
     URL.revokeObjectURL(url);state.prepared=null;showPreview();setBusy();
     showAssets('err','图片读取失败，请换一张 PNG / JPEG');
   };
   img.src=url;
 }
 async function uploadAsset(){
   if(state.busy||!state.assetsReady)return;
   if(!state.prepared){showAssets('warn','请先选择图片');return}
   const name=cutBytes(($('assetName').value||'').trim(),NAME_BYTES)||('素材'+(state.assets.length+1));
   state.busy=true;setBusy();
   try{
     const ctrl=new AbortController(),timer=setTimeout(()=>ctrl.abort(),15000);
     let r;
     try{
       r=await fetch('/api/assets?name='+encodeURIComponent(name),
         {method:'POST',headers:{'Content-Type':'application/octet-stream'},
          body:state.prepared.bytes,signal:ctrl.signal});
     }finally{clearTimeout(timer)}
     let d=null;
     try{d=await r.json()}catch{throw Error('设备响应格式错误')}
     if(!r.ok||d.error)throw Error(d.error||('HTTP '+r.status));
     showAssets('ok','素材「'+name+'」已上传');
     toast('素材已上传');
     clearPrepared();
     await refreshAssets();
   }catch(e){
     showAssets('err','上传失败：'+(e.name==='AbortError'?'请求超时':e.message));
     toast('上传失败','err');
   }finally{state.busy=false;setBusy()}
 }

 // ---------- 保存 ----------
 async function savePages(pages){
   if(state.busy||!state.pagesReady)return;
   const list=pages.map(p=>state.rows.get(p)).filter(Boolean).filter(rowDirty);
   if(!list.length){toast('没有需要保存的修改','warn');return}
   for(const row of list){
     if(bytesOf(row.input.value)>MAX_TEXT){
       showPages('err','第 '+(row.page+1)+' 页文字超过 '+MAX_TEXT+' 字节（约 42 个汉字），请精简后再保存');
       toast('第 '+(row.page+1)+' 页文字过长','warn');
       row.input.focus();return;
     }
   }
   state.busy=true;setBusy();
   let ok=0,error='';
   try{
     for(const row of list){
       const payload={page:row.page,text:row.input.value.trim(),asset:Number(row.select.value)};
       await L.api('/api/pages/content','PUT',payload);
       row.input.value=payload.text;
       row.saved={text:payload.text,asset:payload.asset};
       if(payload.text||payload.asset>=0)state.items.set(row.page,{text:payload.text,asset:payload.asset});
       else state.items.delete(row.page);
       ok++;
       updateRow(row);updateHint();
     }
   }catch(e){error=e.message}
   state.busy=false;setBusy();updateStatus();
   if(error){
     showPages('err','保存中断：'+error+(ok?'（已保存 '+ok+' 页）':''));
     toast('保存失败','err');
   }else if(ok===1){
     showPages('ok','第 '+(list[0].page+1)+' 页已保存');
     toast('第 '+(list[0].page+1)+' 页已保存');
   }else{
     showPages('ok','已保存 '+ok+' 页');
     toast('已保存 '+ok+' 页');
   }
 }

 // ---------- 读取 ----------
 async function load(){
   if(state.busy)return;
   state.busy=true;setBusy();
   hideMsg('pagesMsg');hideMsg('assetsMsg');
   $('pageChip').textContent='读取中…';
   $('assetChip').textContent='读取中…';
   const results=await Promise.allSettled([
     L.api('/api/pages'),L.api('/api/pages/content'),L.api('/api/assets')]);
   const pageErrors=[];
   state.items.clear();state.thumbs.clear();
   const pg=results[0],ct=results[1],as=results[2];
   if(pg.status==='fulfilled'){
     const n=Number(pg.value.total_pages);
     state.totalPages=Number.isInteger(n)&&n>0&&n<=128?n:30;
   }else{
     pageErrors.push('页数读取失败：'+pg.reason.message);
     // 页面内容接口也带 total_pages，可作兜底
     const n=ct.status==='fulfilled'?Number(ct.value.total_pages):NaN;
     if(Number.isInteger(n)&&n>0&&n<=128)state.totalPages=n;
   }
   if(ct.status==='fulfilled'){
     (ct.value.items||[]).forEach(it=>{
       const page=Number(it&&it.page);
       if(!Number.isInteger(page)||page<0||page>=128)return;
       state.items.set(page,{text:it.text==null?'':String(it.text),
         asset:Number.isInteger(it.asset)&&it.asset>=0?it.asset:-1});
     });
   }else pageErrors.push('页面内容读取失败：'+ct.reason.message);
   state.assets=as.status==='fulfilled'?normalizeAssets(as.value.slots):[];
   // 只有「页面内容」读成功才允许写：读失败时编辑再保存会覆盖设备里的旧内容
   state.pagesReady=ct.status==='fulfilled';
   state.assetsReady=as.status==='fulfilled';
   state.busy=false;
   renderRows();renderAssets();updateStatus();updateHint();setBusy();
   if(pageErrors.length)
     showPages('err',pageErrors.join('；')+
       (state.pagesReady?'。页面文字仍可正常保存。':'。为避免覆盖设备里已有的内容，页面内容已暂停编辑，请点「刷新」重试。'));
   if(!state.assetsReady)
     showAssets('err','素材库读取失败：'+(as.reason?as.reason.message:'未知错误')+
       '。上传与删除暂不可用，页面文字不受影响。');
 }

 async function reload(){
   if(state.busy)return;
   if(anyDirty()&&!await L.confirm('放弃未保存的更改？','刷新会丢弃表格里尚未保存的文字或图片选择。','放弃并刷新'))return;
   await load();
 }

 // ---------- 事件 ----------
 $('refreshBtn').addEventListener('click',reload);
 $('saveAllBtn').addEventListener('click',()=>{
   const list=dirtyPages();
   if(!list.length){toast('没有需要保存的修改','warn');return}
   savePages(list);
 });
 $('assetFile').addEventListener('change',e=>{
   const file=e.target.files&&e.target.files[0];
   if(file)prepareImage(file);
 });
 $('assetName').addEventListener('blur',e=>{
   e.target.value=cutBytes(e.target.value.trim(),NAME_BYTES);
 });
 $('uploadBtn').addEventListener('click',uploadAsset);
 window.addEventListener('beforeunload',e=>{
   if(anyDirty()){e.preventDefault();e.returnValue=''}
 });

 showPreview();
 load();
})();
