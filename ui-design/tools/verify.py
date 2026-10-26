"""Validate syntax, local assets, page structure, and simulated protocols."""
from pathlib import Path
from html.parser import HTMLParser
import subprocess,tempfile,re,json
root=Path(__file__).resolve().parents[1];assets=root/'spiffs_contents'
class Parser(HTMLParser):
 def __init__(self):super().__init__();self.scripts=[];self.current=None;self.refs=[];self.ids=[];self.bodies=0
 def handle_starttag(self,t,attrs):
  a=dict(attrs)
  if t=='body':self.bodies+=1
  if a.get('id'):self.ids.append(a['id'])
  if t=='script':
   if a.get('src'):self.refs.append(a['src'])
   else:self.current=[]
  if t=='link' and a.get('rel')=='stylesheet':self.refs.append(a['href'])
 def handle_endtag(self,t):
  if t=='script' and self.current is not None:self.scripts.append(''.join(self.current));self.current=None
 def handle_data(self,s):
  if self.current is not None:self.current.append(s)
checks=0
with tempfile.TemporaryDirectory() as td:
 for p in sorted(assets.glob('*.js')):
  subprocess.run(['node','--check',str(p)],check=True,capture_output=True);checks+=1
 for p in [p for p in assets.glob('*.html') if p.name not in ['preview.html','qa.html']]+[root/'preview.html']:
  parser=Parser();parser.feed(p.read_text())
  assert parser.bodies==1,f'Expected one body: {p.name}'
  assert len(parser.ids)==len(set(parser.ids)),f'Duplicate IDs: {p.name}'
  for i,script in enumerate(parser.scripts):
   js=Path(td)/f'{p.stem}-{i}.js';js.write_text(script);subprocess.run(['node','--check',str(js)],check=True,capture_output=True);checks+=1
  for ref in parser.refs:
   if re.match(r'https?:|data:',ref):continue
   assert (assets/ref.removeprefix('./').lstrip('/')).exists(),f'Missing local resource: {ref}'
subprocess.run(['node',str(root/'tools/test-demo.mjs')],check=True)
print(f'PASS · {checks} JavaScript syntax checks; unique IDs, one body per page, local references')
