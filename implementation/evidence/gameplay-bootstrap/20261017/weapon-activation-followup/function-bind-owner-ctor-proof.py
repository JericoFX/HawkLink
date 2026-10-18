import hashlib,json,mmap,re,struct
from pathlib import Path
folder=Path(__file__).parent
original=Path(r"F:\SteamLibrary\steamapps\common\Hawken\Binaries\Win64\HawkenGame-Win64-Shipping.exe")
records=[(0x6bd50,0x6bda5,"function-bind-owner-ctor-6bd50-machine.txt"),(0x77df0,0x77e31,"function-bind-owner-ctor-77df0-machine.txt"),(0x752e0,0x75387,"function-bind-owner-ctor-factory-752e0-machine.txt")]
with original.open('rb') as source:
 with mmap.mmap(source.fileno(),0,access=mmap.ACCESS_READ) as data:
  digest=hashlib.sha256(data).hexdigest();assert digest=='4a861ecb4ac7add9554f8daf8f98ad2c12b9cb6c1865a83e8be808beb46ea8a2'
  pe=struct.unpack_from('<I',data,0x3c)[0];opt=pe+24;table=opt+struct.unpack_from('<H',data,pe+20)[0]
  sections=[struct.unpack_from('<IIII',data,table+i*40+8) for i in range(struct.unpack_from('<H',data,pe+6)[0])]
  def raw(rva):return next(start+rva-first for _,first,size,start in sections if first<=rva<first+size)
  bodies=[]
  for a,b,name in records:
   path=folder/name;seen=set();instruction_lines=0
   for line in path.read_text().splitlines():
    match=re.match(r'^\s*([0-9a-f]+):\s*((?:[0-9a-f]{2}[ \t]+)+)',line)
    if not match:continue
    address=int(match[1],16)-0x140000000;span=bytes.fromhex(match[2])
    assert a<=address and address+len(span)<=b
    assert data[raw(address):raw(address)+len(span)]==span
    seen.update(range(address,address+len(span)));instruction_lines+=1
   assert seen==set(range(a,b))
   body=data[raw(a):raw(a)+b-a]
   bodies.append(dict(rva=hex(a),end_rva=hex(b),raw=hex(raw(a)),size=b-a,sha256=hashlib.sha256(body).hexdigest(),machine_file=str(path.resolve()),machine_sha256=hashlib.sha256(path.read_bytes()).hexdigest(),byte_chunks=instruction_lines,full_chunk_bytes_match=True,gaps=0))
  anchors=[]
  for rva,size,meaning in [
   (0x6bd6d,10,'LEA vtable12bb500 then MOV[RCX],RAX'),
   (0x77e20,10,'LEA vtable12bb500 then MOV[RBX],RAX'),
   (0x75326,12,'LEA constructor77df0 then save argument10 stack+48'),
   (0x75352,12,'LEA UTF16Function then save argument6 stack+28'),
   (0x7534d,5,'Factory incoming RBX passed as argument7 stack+30: outer not named here'),
   (0x75377,5,'Call retained UClass constructor69290')]:
    span=data[raw(rva):raw(rva)+size];anchors.append(dict(rva=hex(rva),raw=hex(raw(rva)),bytes=span.hex(),sha256=hashlib.sha256(span).hexdigest(),meaning=meaning))
  literals=[]
  for rva in [0x12b6c9a,0x12a59f0]:
   buf=data[raw(rva):raw(rva)+128];end=next(i for i in range(0,128,2) if buf[i:i+2]==b'\0\0');span=buf[:end+2]
   literals.append(dict(rva=hex(rva),raw=hex(raw(rva)),text=buf[:end].decode('utf-16le'),bytes=span.hex(),sha256=hashlib.sha256(span).hexdigest()))
result={'schema':'function-bind-owner-constructor-proof/v1','status':'CONFIRMED_NATIVE_FUNCTION_NAMED_FACTORY_CONSTRUCTOR_VTABLE_BINDER;CORE_OUTER_AND_LIVE_BINDING_UNOBSERVED',
 'original':str(original),'original_sha256':digest,'total_new_code_bytes':sum(b['size'] for b in bodies),'authorized_code_cap':3072,'bodies':bodies,'anchors':anchors,'literals':literals,
 'chain':[
  'Only two RIP-relative LEA xrefs to12bb500 in.text:6bd6d and77e20. Both install it at receiver+0.',
  '77df0..77e31 is an initialization body with c60e0 base call, zero initialization and final vtable install.6bd50..6bda5 is a cleanup/deallocation-style path, not named as a constructor.',
  'Only one exact LEA-to77df0 found:75326 in752e0..75387. This factory passes exact UTF16Function at argument6, caller-supplied outer argument7, and constructor77df0 at argument10 to69290.',
  'Retained69290 stores argument10 inUClass+1dc. Existing ScriptStruct factory proof establishes the same named factory/constructor convention.',
  'Installed table12bb500 plus298 gives12bb798, originalQWORD14006e540. Base+240 gives ProcessEvent7bb50. Candidate table/slot is now constructor-bound for the native class namedFunction.'
 ],
 'package_limit':'The factory outer is incoming RBX, not an original Core literal here. The separate Engine string is argument8, stored by retained69290 in+138; it must NOT be used as a package claim. Native Function naming is positively bound; exact Core outer caller is not independently closed in this slice.',
 'binder_consequence':'Together with parent function-dispatch-bind-proof.json, native Function table slot298 selects6e540. Non-native bit400clear selects7bf30 in that body. Actual CheckJumpOrHealOrTaunt/ChangeCanFire runtime vtable, invocation of Bind and later+f0 mutation are not observed; no callback execution/gameplay claim follows.',
 'branches_outside_reviewed_chunks':[],
 'external_calls_not_expanded':['6bd77->c8c30','6bd80->6a430','6bd92->c66c0','77dfd->c60e0','752fc->4ea00','75377->69290 (retained pseudocode reused only)'],
 'retained_pins':[],
 'limits':['No RTTI repeated','No Ghidra/newprovider/build/runtime/source/config/doc/Git edits','Only317B new code, two initialization/table xrefs and one directly bound named factory','Current tests3763eb35eb461bfd0dc91266bcb72dadec417991ec5e54587fe228451c1fdaab preserved','Own persistent helpers0']}
for path in [folder/'function-dispatch-bind-proof.json',folder/'function-bind-owner-rdata.json',folder/'function-bind-owner-ctor-xrefs.json',folder/'function-bind-owner-ctor-factory-xrefs.json',Path('research/2026-10-10/evidence/game/connection-defaults/class-link-functions.jsonl'),Path('implementation/evidence/mech-struct-wire/ancestor-binding/native/contract.json')]:
 result['retained_pins'].append(dict(path=str(path.resolve()),sha256=hashlib.sha256(path.read_bytes()).hexdigest()))
out=Path(__file__).with_suffix('.json');out.write_text(json.dumps(result,indent=2)+'\n',encoding='utf-8');print(json.dumps({'output':str(out),'sha256':hashlib.sha256(out.read_bytes()).hexdigest(),'bytes_verified':result['total_new_code_bytes'],'bodies':bodies}))
