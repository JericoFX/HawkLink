import hashlib,json,mmap,struct
from pathlib import Path
folder=Path(__file__).parent
original=Path(r"F:\SteamLibrary\steamapps\common\Hawken\Binaries\Win64\HawkenGame-Win64-Shipping.exe")
refs=[folder/"function-bind-owner-rdata.json",folder/"function-bind-owner-rdata.py",
 folder/"function-dispatch-bind-proof.json",
 Path("implementation/evidence/mech-struct-wire/ancestor-binding/native/contract.json"),
 Path("implementation/evidence/complete-artifact-map-20261013/parent/native-functions.json")]
with original.open("rb") as stream:
 with mmap.mmap(stream.fileno(),0,access=mmap.ACCESS_READ) as data:
  assert hashlib.sha256(data).hexdigest()=="4a861ecb4ac7add9554f8daf8f98ad2c12b9cb6c1865a83e8be808beb46ea8a2"
  pe=struct.unpack_from("<I",data,0x3c)[0]; opt=pe+24
  count=struct.unpack_from("<H",data,pe+6)[0];table=opt+struct.unpack_from("<H",data,pe+20)[0]
  sections=[struct.unpack_from("<IIII",data,table+i*40+8) for i in range(count)]
  def raw(rva):return next(start+rva-first for _,first,size,start in sections if first<=rva<first+size)
  anchors=[]
  for rva,expected,meaning in [
   (0x12bb470,0x14007bb50,"Confirmed ScriptStruct table12bb230 +240 points ProcessEvent7bb50"),
   (0x12bb740,0x14007bb50,"ProcessEvent7bb50 cell adjacent to binder cell; candidate base12bb500 only if same+240 slot"),
   (0x12bb798,0x14006e540,"Unique exact binder QWORD reference in .rdata"),
   (0x12bb500,None,"Candidate table start, not constructor/class-binding proof")]:
   offset=raw(rva);value=struct.unpack_from("<Q",data,offset)[0]
   if expected is not None:assert value==expected
   span=data[offset:offset+8]
   anchors.append(dict(rva=hex(rva),raw=hex(offset),bytes=span.hex(),qword=hex(value),sha256=hashlib.sha256(span).hexdigest(),meaning=meaning))
result={"schema":"function-bind-owner-corroboration/v1","status":"CONFIRMED_RDATA_REFERENCE;CANDIDATE_TABLE_SLOT;CORE_FUNCTION_OWNER_NOT_VERIFIED",
 "original":str(original),"original_sha256":"4a861ecb4ac7add9554f8daf8f98ad2c12b9cb6c1865a83e8be808beb46ea8a2",
 "anchors":anchors,
 "conditional_table":{"candidate_base_rva":"0x12bb500","process_event_offset":"0x240","binder_offset":"0x298",
  "premise":"Candidate base inferred from common inherited ProcessEvent slot. A contiguous pointer run is not a vtable-boundary identity proof.",
  "core_function_owner":"NO_VERIFICADO: no retained named factory/constructor installing12bb500 was found in the explicitly reviewed records."},
 "reviewed_retained_scopes":["mech-struct-wire/ancestor-binding (positive ScriptStruct only)","controller-effective-field-cache/property-link-20261013/receiver-bindings.json (ScriptStruct and Property bindings, not this owner)","controller-netfield-cache and controller-rpc-wire retained constructors", "research/2026-10-10/evidence/game/connection-defaults", "complete-artifact-map parent native-functions index: no positively named UFunction constructor record"],
 "next_finite_discriminator":"A positive constructor/factory that installs candidate12bb500 and names Core.Function, or exact reflected UFunction vtable under separate runtime authorization. A bounded static RIP-relative xref to12bb500 could locate that constructor, but was not performed in this scope.",
 "consequence":"Parent's byte-verified6e540 writes7bf30 for nativebit400clear. Assigning that binder behavior to CheckJumpOrHealOrTaunt/ChangeCanFire remains conditional on actual UFunction binding and absence of later mutation; no runtime target or callback execution is established.",
 "provenance":[dict(path=str(p.resolve()),sha256=hashlib.sha256(p.read_bytes()).hexdigest()) for p in refs],
 "limits":["No repeated RTTI search (previous zero hits retained)","No code bodies, disassembly or Ghidra extracted", "No code/config/doc/build/runtime/Git change", "Exact QWORD scan and adjacent retained-owner comparison only", "Tests live_player_reader_tests.cpp remain3763eb35eb461bfd0dc91266bcb72dadec417991ec5e54587fe228451c1fdaab", "Own persistent helpers0"]}
out=Path(__file__).with_suffix(".json");out.write_text(json.dumps(result,indent=2)+"\n",encoding="utf-8")
print(json.dumps({"output":str(out),"sha256":hashlib.sha256(out.read_bytes()).hexdigest(),"anchors":anchors,"status":result["status"]}))
