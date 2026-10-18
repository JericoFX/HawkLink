import hashlib,json,mmap,struct
from pathlib import Path
original=Path(r"F:\SteamLibrary\steamapps\common\Hawken\Binaries\Win64\HawkenGame-Win64-Shipping.exe")
with original.open('rb') as source:
 with mmap.mmap(source.fileno(),0,access=mmap.ACCESS_READ) as data:
  digest=hashlib.sha256(data).hexdigest();assert digest=='4a861ecb4ac7add9554f8daf8f98ad2c12b9cb6c1865a83e8be808beb46ea8a2'
  pe=struct.unpack_from('<I',data,0x3c)[0];opt=pe+24;table=opt+struct.unpack_from('<H',data,pe+20)[0]
  sections=[]
  for i in range(struct.unpack_from('<H',data,pe+6)[0]):
   pos=table+i*40;name=data[pos:pos+8].rstrip(b'\0').decode('ascii');vsize,rva,size,raw=struct.unpack_from('<IIII',data,pos+8)
   sections.append(dict(name=name,rva=rva,size=size,virtual_size=vsize,raw=raw))
  def raw(rva):return next(s['raw']+rva-s['rva'] for s in sections if s['rva']<=rva<s['rva']+s['size'])
  text=next(s for s in sections if s['name']=='.text');exception_rva,exception_size=struct.unpack_from('<II',data,opt+112+24)
  rows=list(struct.iter_unpack('<III',data[raw(exception_rva):raw(exception_rva)+exception_size]))
  matches=[]
  for prefix in (b'\x48\x8d',b'\x4c\x8d'):
   cursor=text['raw'];end=cursor+text['size']
   while True:
    hit=data.find(prefix,cursor,end)
    if hit<0:break
    cursor=hit+1
    if hit+7>end or data[hit+2]&0xc7!=0x05:continue
    rva=hit-text['raw']+text['rva'];target=rva+7+struct.unpack_from('<i',data,hit+3)[0]
    if target!=0x12bb500:continue
    chunks=[dict(rva=hex(a),end_rva=hex(b),size=b-a,unwind_rva=hex(c),raw=hex(raw(a)),sha256=hashlib.sha256(data[raw(a):raw(a)+b-a]).hexdigest()) for a,b,c in rows if a<=rva<b]
    matches.append(dict(rva=hex(rva),raw=hex(hit),bytes=data[hit:hit+7].hex(),target_rva=hex(target),pdata_chunks=chunks))
  result={'schema':'function-bind-owner-ctor-rip-xrefs/v1','original':str(original),'sha256':digest,'scan':'Only REX.W LEA RIP-relative instructions targeting exact candidate12bb500 in .text. No indirect references or generic names searched.','matches':matches,'chunk_limit_total':3072,'limit':'.pdata denotes recorded chunks only, not proof of full enclosing function or complete callers.'}
out=Path(__file__).with_suffix('.json');out.write_text(json.dumps(result,indent=2)+'\n',encoding='utf-8');print(json.dumps(result))
