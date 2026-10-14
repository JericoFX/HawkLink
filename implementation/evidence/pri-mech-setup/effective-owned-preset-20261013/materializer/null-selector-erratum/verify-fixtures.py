# Read-only independent absolute-bit fixture verifier. No builder/codec execution.
from pathlib import Path
import hashlib
import json

base = Path(__file__).resolve().parent.parent
erratum = base / "null-selector-erratum"
for name in ("baseline-fixture.json", "baseline-fixture-holo255.json"):
    before = json.loads((erratum / "before" / name).read_text())
    current = json.loads((base / name).read_text())
    assert current["logical_bits"] == before["logical_bits"] == 544
    assert current["caller"] == before["caller"]
    assert current["expected_rows"][:-1] == before["expected_rows"][:-1]
    output = bytearray(68)
    occupied = set()
    for row in current["expected_rows"]:
        at, width, number = row["absolute_bit_offset"], row["bits"], int(row["value"])
        for bit in range(width):
            position = at + bit
            assert position not in occupied
            occupied.add(position)
            if (number >> bit) & 1:
                output[position // 8] |= 1 << (position % 8)
    assert occupied == set(range(544))
    assert output.hex() == current["hex"]
    old = bytes.fromhex(before["hex"])
    changes = [bit for bit in range(544)
               if ((old[bit // 8] ^ output[bit // 8]) >> (bit % 8)) & 1]
    assert changes == [532]
    template = current["expected_rows"][-1]
    assert template["semantic_value"] is None and template["value"] == 1
    assert template["wire_encoding"]["primary_selector"] == 1
    assert template["wire_encoding"]["channel_index"] == 0
    print(name, "544bits; only original null-selector bit532 corrected")
