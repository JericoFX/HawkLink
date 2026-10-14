from pathlib import Path
import hashlib,json,difflib
root=Path('F:/HawkenServer');e=root/'implementation/evidence/selection-bootstrap-protocol/20261014/gri-hud-owner/green'
files=['include/hawken/local_udp_owner.hpp','src/local_udp_owner.cpp','tests/local_udp_owner_tests.cpp']; pins=[]
for f in files:
 p=root/f;old=(e/('before-'+p.name)).read_bytes();new=p.read_bytes()
 if f!='src/local_udp_owner.cpp': assert old==new
 assert b'\r' not in new
 delta=''.join(difflib.unified_diff(old.decode().splitlines(True),new.decode().splitlines(True),fromfile='RED/'+f,tofile='GREEN/'+f))
 if delta:(e/(p.name+'.diff')).write_text(delta,encoding='utf-8',newline='\n')
 pins.append({'path':f,'sha256':hashlib.sha256(new).hexdigest(),'red_sha256':hashlib.sha256(old).hexdigest(),'bytes':len(new),'changed':old!=new})
(e/'inputs.json').write_text(json.dumps(pins,indent=2)+'\n',encoding='utf-8')
print(json.dumps(pins,indent=2))
print((e/'local_udp_owner.cpp.diff').read_text())
