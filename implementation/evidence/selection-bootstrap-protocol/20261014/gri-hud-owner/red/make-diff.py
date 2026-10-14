from pathlib import Path
import difflib,hashlib,json
root=Path('F:/HawkenServer')
e=root/'implementation/evidence/selection-bootstrap-protocol/20261014/gri-hud-owner/red'
files=['include/hawken/local_udp_owner.hpp','src/local_udp_owner.cpp','tests/local_udp_owner_tests.cpp']
out=[]
for f in files:
 old=(e/('before-'+Path(f).name)).read_bytes(); new=(root/f).read_bytes()
 assert b'\r' not in old and b'\r' not in new
 diff=''.join(difflib.unified_diff(old.decode().splitlines(True),new.decode().splitlines(True),fromfile='before/'+f,tofile=f))
 (e/(Path(f).name+'.diff')).write_text(diff,encoding='utf-8',newline='\n')
 out.append({'path':f,'before_sha256':hashlib.sha256(old).hexdigest(),'sha256':hashlib.sha256(new).hexdigest(),'bytes':len(new),'diff':str((e/(Path(f).name+'.diff')).relative_to(root)).replace('\\','/'),'diff_sha256':hashlib.sha256(diff.encode()).hexdigest()})
 # Existing test body bytes outside the two constructor forwarding edits must be preserved.
 if f.startswith('tests/'):
  prefix=new.split(b'\nnamespace {\nconstexpr std::array<std::uint32_t,4> bootstrap_hg')[0]
  prefix=prefix.replace(b'                                   std::optional<udp::PriIdentityDispatchPlan> pri_identity=std::nullopt,\n                                   std::optional<udp::SelectionBootstrapPlan> selection_bootstrap=std::nullopt)',b'                                   std::optional<udp::PriIdentityDispatchPlan> pri_identity=std::nullopt)')
  prefix=prefix.replace(b'std::move(pri_identity),std::move(selection_bootstrap));',b'std::move(pri_identity));')
  assert prefix==old
(e/'inputs.json').write_text(json.dumps(out,indent=2)+'\n',encoding='utf-8')
print(json.dumps(out,indent=2))
