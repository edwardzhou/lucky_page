(function(){
 'use strict';const L=window.Lucky,page=document.body.dataset.page;
 if(page==='logo'){
  const hint=Array.from(document.querySelectorAll('.hint')).find(e=>e.textContent.includes('数值越小'));
  if(hint)hint.textContent='阈值越大，更多像素会转为黑点。请以黑白点阵预览为准。';
  const oldDel=window.del;window.del=async()=>{if(!await L.confirm('删除票头 Logo？','使用这张图片的小票将不再打印该 Logo，其他版式内容保持不变。','删除 Logo'))return;try{await L.api('/api/logo','DELETE');show('ok','Logo 已删除');toast('Logo 已删除');showCurrent()}catch(e){show('err','删除失败：'+e.message)}};
 }
 if(page==='calibrate'){
  // Keep physical controls disabled until the real calibration read succeeds.
  const controls=Array.from(document.querySelectorAll('.grid3 button,.grid4 button,#jumpBtn,.actbar button,#advBase button'));
  const check=()=>{const available=typeof mapping!=='undefined'&&mapping.length>0&&typeof offsets!=='undefined'&&offsets.length>=mapping.length;controls.forEach(b=>b.disabled=!available);if(!available){document.getElementById('pageChip').textContent='标定数据未就绪';if(typeof mapping==='undefined'||!mapping.length){['stCur','stBase','stOff','stAct'].forEach(id=>document.getElementById(id).textContent='—')}}};check();const t=setInterval(check,800);window.addEventListener('pagehide',()=>clearInterval(t));
 }
 if(page==='receipt'){
  const baselineTitle=document.querySelector('.lp-page-actions');
  const badge=document.createElement('span');badge.className='lp-unsaved-tag';badge.hidden=true;badge.textContent='未保存更改';baselineTitle.appendChild(badge);
  let baseline=null,initialized=false;
  const snapshot=()=>{try{return JSON.stringify(payload())}catch{return null}};
  const capture=()=>{if(!initialized&&document.getElementById('params').textContent.trim()!=='加载中…'){baseline=snapshot();initialized=true}};capture();
  const originalSave=window.save;window.save=async()=>{try{await originalSave();const msg=document.getElementById('msg');if(msg.textContent.includes('已保存')){baseline=snapshot();badge.hidden=true;toast('小票模板已保存')}}catch(e){toast('保存失败：'+e.message,'err')}};
  const originalLoadMode=window.loadTemplateForMode;
  window.loadTemplateForMode=async m=>{await originalLoadMode(m);if(!document.getElementById('msg').textContent.includes('失败')){baseline=snapshot();badge.hidden=true;initialized=true}};
  const dirty=()=>initialized&&baseline!==snapshot();
  const originalSetMode=window.setMode;window.setMode=async m=>{if(m===mode)return;if(dirty()&&!await L.confirm('切换小票版式？','当前未保存的更改会丢失。保存后再切换可以保留这些改动。','切换版式')){document.getElementById('mode').value=mode;return}originalSetMode(m)};
  const timer=setInterval(()=>{capture();badge.hidden=!dirty()},500);
  window.addEventListener('beforeunload',e=>{if(dirty()){e.preventDefault();e.returnValue=''}});
  window.addEventListener('pagehide',()=>clearInterval(timer));
  // Existing receipt code uses real block semantics, including bg, col, row, QR size and ECC.
 }
})();
