import vm from 'node:vm';
import fs from 'node:fs';
import assert from 'node:assert/strict';
const source=fs.readFileSync(new URL('../spiffs_contents/demo.js',import.meta.url),'utf8');
let assertions=0;
function check(v,msg){assert.ok(v,msg);assertions++}
function environment(demo){let now=Date.parse('2026-10-26T02:00:00Z');const storage=new Map(),native=async()=>new Response('{}');class Clock extends Date{constructor(...a){super(...(a.length?a:[now]))}static now(){return now}}
const c={URL,URLSearchParams,Response,Uint8Array,Date:Clock,Math,JSON,location:{search:demo?'?demo=1':'',reload(){}},document:{baseURI:'http://controller.local/'},fetch:native,localStorage:{getItem:k=>storage.get(k)||null,setItem:(k,v)=>storage.set(k,v),removeItem:k=>storage.delete(k)}};c.window=c;c.parent=c;vm.createContext(c);vm.runInContext(source,c);return {c,native,advance:n=>{now+=n}}}
const plain=environment(false);check(plain.c.fetch===plain.native,'Native fetch retained without demo');check(!plain.c.LUCKY_DEMO_ACTIVE,'Demo disabled by default');
const {c,advance}=environment(true);check(c.LUCKY_DEMO_ACTIVE,'Explicit demo enabled');
async function api(path,method='GET',body){const r=await c.fetch(path,{method,body:body===undefined?undefined:JSON.stringify(body)});return [r,await r.json()]}
let [,pool]=await api('/api/prizes');check(pool.tiers.length===5,'Five initial prize tiers');check(Math.abs(pool.tiers.reduce((n,t)=>n+t.probability,0)-1)<1e-9,'Eligible probabilities sum to 1');
let [,status]=await api('/api/lottery/status');check(status.stats.total_draws===128,'Initial counters');
let [,draw]=await api('/api/draw','POST');check(draw.status==='ok','Draw accepted');let [,busy]=await api('/api/draw','POST');check(busy.status==='busy','Double draw rejected while busy');
advance(1100);[,status]=await api('/api/lottery/status');check(status.state==='printing','Draw enters printing');advance(700);[,status]=await api('/api/lottery/status');check(status.state==='done'&&status.stats.total_draws===129,'Draw completes exactly once');[,status]=await api('/api/lottery/status');check(status.stats.total_draws===129,'Status polling does not duplicate a draw');
let [r]=await api('/api/pages','PUT',{total_pages:2});check(r.status===400,'Page reduction cannot invalidate tiers');
[,pool]=await api('/api/prizes');pool.tiers[0].remaining=0;await api('/api/prizes','PUT',pool);[,pool]=await api('/api/prizes');check(pool.tiers[0].probability===0,'Depleted tier removed from probability calculation');
await api('/api/prizes/reset-stock','POST');[,pool]=await api('/api/prizes');check(pool.tiers[0].remaining===pool.tiers[0].quantity,'Reset stock round trip');
let [,adj]=await api('/api/adjust','PUT',{cell:0,page:2,offset:200});check(adj.offset===127&&adj.value<=255,'Offset and resulting value clamp');
await api('/api/adjust/reset','POST',{cell:0,page:2});[,adj]=await api('/api/adjust');check(adj.offsets[0][2]===0,'Reset selected page offset');
[r]=await api('/api/page/goto','POST',{page:1000});check(r.status===400,'Goto rejects out-of-range page');
await api('/api/page/goto','POST',{page:5});[,status]=await api('/api/lottery/status');check(status.current_page===5,'Goto accepted valid page');
let [,tpl]=await api('/api/receipt/template?mode=1');check(tpl.blocks.some(b=>b.bg===true),'Canvas template keeps background block');tpl.blocks.push({type:'qr',content:'{ticket}{check}',size:6,ecc:49,col:3,row:12});await api('/api/receipt/template','PUT',tpl);[,tpl]=await api('/api/receipt/template?mode=1');check(tpl.blocks.some(b=>b.type==='qr'&&b.ecc===49&&b.col===3&&b.row===12),'QR semantics survive template save');
let [,pv]=await api('/api/receipt/template/preview','POST',{paper_cols:32,blocks:[{type:'line',text:'{prize} {unknown}'}]});check(pv.unknown_params===1&&pv.text.includes('{unknown}'),'Unknown parameters remain visible');
const bitmap=new Uint8Array(16);bitmap.set([76,71,79,49,8,0,8,0]);bitmap.fill(0xaa,8);r=await c.fetch('/api/logo',{method:'POST',body:bitmap});let logo=await r.json();check(logo.exists&&logo.width===8&&logo.height===8,'LGO1 bitmap upload validated');r=await c.fetch('/api/logo/raw');check(new Uint8Array(await r.arrayBuffer()).join(',')===bitmap.join(','),'LGO1 raw bytes preserved');
[r]=await api('/api/wifi','DELETE',{});check(r.status===400,'Network deletion requires explicit index/all');
await api('/api/wifi','PUT',{ssid:"QA O'Brien",password:'not-a-real-secret'});let [,wifi]=await api('/api/wifi');check(wifi.ssid==="QA O'Brien"&&!('password' in wifi),'Network name saved without echoing password');
let [,scan]=await api('/api/wifi/scan');check(scan.aps.length===3,'Scan fixtures');
[r]=await api('/api/unsupported');check(r.status===501,'Unknown routes cannot fake success');
console.log(`PASS · ${assertions} demo and protocol assertions`);
