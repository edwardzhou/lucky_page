"""Rebuild the offline preview from spiffs_contents. No dependencies."""
from pathlib import Path
import re,json
root=Path(__file__).resolve().parents[1]
assets={p.name:p.read_text() for p in (root/'spiffs_contents').iterdir() if p.suffix in ['.html','.js','.css'] and p.name not in ['preview.html','qa.html']}
pages={}
for name,s in assets.items():
 if not name.endswith('.html'):continue
 def css(m):
  a=re.search(r'href="([^"]+)"',m.group(0));n=a.group(1).split('/')[-1] if a else ''
  return '<style>'+assets[n]+'</style>' if n in assets else m.group(0)
 def js(m):
  n=m.group(1).split('/')[-1]
  return '<script>'+assets[n].replace('</script','<\\/script')+'</script>' if n in assets else m.group(0)
 s=re.sub(r'<link[^>]*rel="stylesheet"[^>]*>',css,s)
 s=re.sub(r'<script\b[^>]*src="([^"]+)"[^>]*></script>',js,s)
 pages[name]=s
safe=lambda v:json.dumps(v,ensure_ascii=False).replace('<','\\u003c').replace('\u2028','\\u2028').replace('\u2029','\\u2029')
head='''<!DOCTYPE html><html lang="zh-CN"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><title>Lucky Studio · 交互预览</title><style>html,body{margin:0;height:100%;background:#f5f7f6}iframe{display:block;width:100%;height:100dvh;border:0}</style></head><body><iframe id="app" title="Lucky Studio 完整交互预览"></iframe><script>'''
script=r'''
const pages=__PAGES__;
const frame=document.getElementById('app');
let state=null;
function render(name){
 if(!pages[name])return;
 const closeScript='<'+'/script>',closeBody='<'+'/body>';
 const encoded=JSON.stringify(state).replace(/</g,'\\u003c');
 const setup='<base href="https://lucky-preview.invalid/"><script>window.LUCKY_STANDALONE=true;window.LUCKY_PREVIEW_STATE='+encoded+';'+closeScript;
 let doc=pages[name].replace(/<head[^>]*>/,'<head>'+setup);
 const bridge='<script>document.addEventListener("click",function(e){var a=e.target.closest("a[href]");if(!a||a.download)return;var raw=a.getAttribute("href");if(!raw||raw.startsWith("#"))return;var u=new URL(a.href,document.baseURI),name=u.pathname.split("/").pop();if(name.endsWith(".html")){e.preventDefault();parent.postMessage({type:"lucky-preview-nav",page:name},"*")}},true);'+closeScript;
 doc=doc.replace(closeBody,bridge+closeBody);
 frame.srcdoc=doc;
}
window.addEventListener('message',function(e){if(e.source!==frame.contentWindow||!e.data)return;const d=e.data;if(d.type==='lucky-preview-state')state=d.state;if(d.type==='lucky-preview-nav'&&pages[d.page])render(d.page);if(d.type==='lucky-preview-reset'){state=null;render('index.html')}});
render('index.html');
'''
(root/'preview.html').write_text(head+script.replace('__PAGES__',safe(pages))+'\n</script></body></html>')
print('Rebuilt preview.html')
