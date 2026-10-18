import hashlib,json,mmap,struct
from pathlib import Path
original=Path(r"F:\SteamLibrary\steamapps\common\Hawken\Binaries\Win64\HawkenGame-Win64-Shipping.exe")
with original.open("rb") as source:
    with mmap.mmap(source.fileno(),0,access=mmap.ACCESS_READ) as data:
        digest=hashlib.sha256(data).hexdigest()
        assert digest=="4a861ecb4ac7add9554f8daf8f98ad2c12b9cb6c1865a83e8be808beb46ea8a2"
        pe=struct.unpack_from("<I",data,0x3c)[0]; opt=pe+24
        base=struct.unpack_from("<Q",data,opt+24)[0]
        table=opt+struct.unpack_from("<H",data,pe+20)[0]
        sections=[]
        for i in range(struct.unpack_from("<H",data,pe+6)[0]):
            pos=table+i*40;name=data[pos:pos+8].rstrip(b"\0").decode("ascii")
            virtual_size,rva,size,raw=struct.unpack_from("<IIII",data,pos+8)
            sections.append(dict(name=name,rva=rva,virtual_size=virtual_size,size=size,raw=raw))
        section=next(s for s in sections if s["name"]==".rdata")
        text=next(s for s in sections if s["name"]==".text")
        def text_pointer(value):
            return base+text["rva"]<=value<base+text["rva"]+text["virtual_size"]
        def rva(raw):return raw-section["raw"]+section["rva"]
        needle=struct.pack("<Q",base+0x6e540);end=section["raw"]+section["size"]
        cursor=section["raw"];matches=[]
        while True:
            cell=data.find(needle,cursor,end)
            if cell<0:break
            cursor=cell+1
            first=cell;last=cell+8
            while first>=section["raw"]+8 and text_pointer(struct.unpack_from("<Q",data,first-8)[0]):first-=8
            while last+8<=end and text_pointer(struct.unpack_from("<Q",data,last)[0]):last+=8
            start=max(section["raw"],cell-64);stop=min(end,cell+72)
            values=[{"rva":hex(rva(pos)),"raw":hex(pos),"qword":hex(struct.unpack_from("<Q",data,pos)[0])} for pos in range(start,stop,8)]
            matches.append({"cell_rva":hex(rva(cell)),"cell_raw":hex(cell),"aligned8":cell%8==0,
                "cell_bytes":data[cell:cell+8].hex(),"neighborhood_raw":hex(start),
                "neighborhood_bytes":data[start:stop].hex(),"neighborhood_sha256":hashlib.sha256(data[start:stop]).hexdigest(),
                "neighborhood_qwords":values,"contiguous_text_pointer_run":{"rva":hex(rva(first)),"end_rva":hex(rva(last)),
                "count":(last-first)//8,"candidate_offset":hex(cell-first),
                "preceding_qword":hex(struct.unpack_from("<Q",data,first-8)[0]),
                "status":"Run boundary only; not a class/vtable-start identity proof"}})
        output={"schema":"function-bind-owner-rdata/v1","original":str(original),"sha256":digest,"image_base":hex(base),
            "scanned_section":section,"target_va":hex(base+0x6e540),"matches":matches,
            "scope":"Only exact QWORD references in original .rdata plus adjacent pointer-run metadata. No RTTI scan, code execution or new Ghidra.",
            "owner_status":"UNVERIFIED_PENDING_RETAINED_CONSTRUCTOR_BINDING"}
Path(__file__).with_suffix(".json").write_text(json.dumps(output,indent=2)+"\n",encoding="utf-8")
print(json.dumps(output))
