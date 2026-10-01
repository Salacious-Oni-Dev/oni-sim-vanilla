#!/usr/bin/env python3
"""Generate abi/sim_abi.h from the game's own Assembly-CSharp.dll.

Usage:
    gen_sim_abi.py <path to Assembly-CSharp.dll | game install directory> [-o abi/sim_abi.h]

The game talks to its simulation library through message structs declared in managed code.
Their layouts are part of the game, so this repository does not carry them: this script reads
them from the player's installed copy. It reads only .NET metadata (type names, field types,
struct packing and enum constants), which the runtime itself uses to marshal these structs.
No method bodies are read.

Output:
  * enum class SimMessageHash - every member of the game's SimMessageHashes enum
  * one packed struct per message payload and per struct those reference, each followed by a
    static_assert on its size, so a game update that changes a layout fails the build instead
    of corrupting messages at run time.

Standard library only; Python 3.8+.
"""
import argparse
import os
import struct
import sys

# Structs the simulation library uses, as named in the game. Structs they reference are
# added automatically.
ROOTS = """
AddBuildingHeatExchangeMessage RegisterBuildingToBuildingHeatExchangeMessage
AddDiseaseEmitterMessage AddElementChunkMessage AddElementConsumerMessage
AddElementEmitterMessage AddBuildingToBuildingHeatExchangeMessage AddRadiationEmitterMessage
CellDiseaseModification CellRadiationModification CellPropertiesMessage ConsumeDiseaseMessage
DigMessage CreateElementInteractionsMsg MassConsumptionMessage MassEmissionMessage
SetBackwallDataMsg ModifyBuildingEnergyMessage ModifyBuildingHeatExchangeMessage
ModifyCellMessage ModifyCellEnergyMessage CellWorldZoneModification
ModifyElementChunkAdjusterMessage ModifyDiseaseEmitterMessage ModifyElementChunkEnergyMessage
ModifyElementEmitterMessage ModifyRadiationEmitterMessage MoveElementChunkMessage
RadiationParamsModification RemoveBuildingHeatExchangeMessage
RemoveBuildingInContactFromBuildingToBuildingHeatExchangeMessage
RemoveBuildingToBuildingHeatExchangeMessage RemoveDiseaseEmitterMessage
RemoveElementChunkMessage RemoveElementConsumerMessage RemoveElementEmitterMessage
RemoveRadiationEmitterMessage DebugProperties SetElementChunkDataMessage
SetElementConsumerDataMessage SetCellFloatValueMessage SetSavedOptionsMessage NewGameFrame
ElementInteraction PhysicsData Element Cell DiseaseCell SimBackwall GameDataUpdate
""".split()
# Types whose nested structs are searched, outermost enclosing type first.
SCOPES = ("Sim", "SimMessages")
HASH_ENUM = "SimMessageHashes"

# ---------------------------------------------------------------------------------------------
# Minimal ECMA-335 metadata reader
# ---------------------------------------------------------------------------------------------

STR, GUID, BLOB = "s", "g", "b"
# Coded index kinds: (tag bits, tables).
CODED = {
    "TypeDefOrRef": (2, [0x02, 0x01, 0x1B]),
    "HasConstant": (2, [0x04, 0x08, 0x17]),
    "HasCustomAttribute": (5, [0x06, 0x04, 0x01, 0x02, 0x08, 0x09, 0x0A, 0x00, 0x0E, 0x17, 0x14,
                               0x11, 0x1A, 0x1B, 0x20, 0x23, 0x26, 0x27, 0x28, 0x2A, 0x2C, 0x2B]),
    "HasFieldMarshal": (1, [0x04, 0x08]),
    "HasDeclSecurity": (2, [0x02, 0x06, 0x20]),
    "MemberRefParent": (3, [0x02, 0x01, 0x1A, 0x06, 0x1B]),
    "HasSemantics": (1, [0x14, 0x17]),
    "MethodDefOrRef": (1, [0x06, 0x0A]),
    "MemberForwarded": (1, [0x04, 0x06]),
    "Implementation": (2, [0x26, 0x23, 0x27]),
    "CustomAttributeType": (3, [0x06, 0x0A]),  # tag values 2 and 3; the width is what matters
    "ResolutionScope": (2, [0x00, 0x1A, 0x23, 0x01]),
    "TypeOrMethodDef": (1, [0x02, 0x06]),
}
# Column schemas. An int is a fixed width in bytes; ("t", n) is an index into table n.
T = lambda n: ("t", n)
SCHEMA = {
    0x00: [2, STR, GUID, GUID, GUID],
    0x01: ["ResolutionScope", STR, STR],
    0x02: [4, STR, STR, "TypeDefOrRef", T(0x04), T(0x06)],
    0x03: [T(0x04)],
    0x04: [2, STR, BLOB],
    0x05: [T(0x06)],
    0x06: [4, 2, 2, STR, BLOB, T(0x08)],
    0x07: [T(0x08)],
    0x08: [2, 2, STR],
    0x09: [T(0x02), "TypeDefOrRef"],
    0x0A: ["MemberRefParent", STR, BLOB],
    0x0B: [1, 1, "HasConstant", BLOB],
    0x0C: ["HasCustomAttribute", "CustomAttributeType", BLOB],
    0x0D: ["HasFieldMarshal", BLOB],
    0x0E: [2, "HasDeclSecurity", BLOB],
    0x0F: [2, 4, T(0x02)],
    0x10: [4, T(0x04)],
    0x11: [BLOB],
    0x12: [T(0x02), T(0x14)],
    0x13: [T(0x14)],
    0x14: [2, STR, "TypeDefOrRef"],
    0x15: [T(0x02), T(0x17)],
    0x16: [T(0x17)],
    0x17: [2, STR, BLOB],
    0x18: [2, T(0x06), "HasSemantics"],
    0x19: [T(0x02), "MethodDefOrRef", "MethodDefOrRef"],
    0x1A: [STR],
    0x1B: [BLOB],
    0x1C: [2, "MemberForwarded", STR, T(0x1A)],
    0x1D: [4, T(0x04)],
    0x1E: [4, 4],
    0x1F: [4],
    0x20: [4, 2, 2, 2, 2, 4, BLOB, STR, STR],
    0x21: [4],
    0x22: [4, 4, 4],
    0x23: [2, 2, 2, 2, 4, BLOB, STR, STR, BLOB],
    0x24: [4, T(0x23)],
    0x25: [4, 4, 4, T(0x23)],
    0x26: [4, STR, BLOB],
    0x27: [4, 4, STR, STR, "Implementation"],
    0x28: [4, 4, STR, "Implementation"],
    0x29: [T(0x02), T(0x02)],
    0x2A: [2, 2, "TypeOrMethodDef", STR],
    0x2B: ["MethodDefOrRef", BLOB],
    0x2C: [T(0x2A), "TypeDefOrRef"],
}


class Metadata:
    def __init__(self, path):
        with open(path, "rb") as f:
            self.data = data = f.read()
        pe = struct.unpack_from("<I", data, 0x3C)[0]
        if data[pe:pe + 4] != b"PE\0\0":
            raise ValueError("not a PE file")
        nsect = struct.unpack_from("<H", data, pe + 6)[0]
        opt_size = struct.unpack_from("<H", data, pe + 20)[0]
        opt = pe + 24
        magic = struct.unpack_from("<H", data, opt)[0]
        ddir = opt + (96 if magic == 0x10B else 112)
        cli_rva = struct.unpack_from("<I", data, ddir + 14 * 8)[0]
        self.sections = []
        for i in range(nsect):
            s = opt + opt_size + i * 40
            vsize, va, rsize, raw = struct.unpack_from("<IIII", data, s + 8)
            self.sections.append((va, max(vsize, rsize), raw))
        cli = self.rva(cli_rva)
        md = self.rva(struct.unpack_from("<I", data, cli + 8)[0])
        if struct.unpack_from("<I", data, md)[0] != 0x424A5342:
            raise ValueError("no .NET metadata")
        vlen = struct.unpack_from("<I", data, md + 12)[0]
        p = md + 16 + vlen + 2
        nstreams = struct.unpack_from("<H", data, p)[0]
        p += 2
        streams = {}
        for _ in range(nstreams):
            off, size = struct.unpack_from("<II", data, p)
            p += 8
            end = data.index(b"\0", p)
            name = data[p:end].decode()
            p += (end - p + 4) & ~3
            streams[name] = md + off
        self.strings, self.blobs = streams["#Strings"], streams["#Blob"]
        self._tables(streams.get("#~") or streams["#-"])

    def rva(self, rva):
        for va, size, raw in self.sections:
            if va <= rva < va + size:
                return rva - va + raw
        raise ValueError(f"RVA {rva:#x} outside every section")

    def _tables(self, p):
        d = self.data
        heap = d[p + 6]
        valid = struct.unpack_from("<Q", d, p + 8)[0]
        p += 24
        self.rows = {}
        for t in range(64):
            if valid >> t & 1:
                self.rows[t] = struct.unpack_from("<I", d, p)[0]
                p += 4
        if heap & 0x40:
            p += 4  # extra data word
        sz = {STR: 4 if heap & 1 else 2, GUID: 4 if heap & 2 else 2, BLOB: 4 if heap & 4 else 2}

        def width(col):
            if isinstance(col, int):
                return col
            if col in sz:
                return sz[col]
            if isinstance(col, tuple):
                return 4 if self.rows.get(col[1], 0) > 0xFFFF else 2
            bits, tables = CODED[col]
            return 4 if max(self.rows.get(t, 0) for t in tables) >= 1 << (16 - bits) else 2

        self.tables = {}
        for t in range(0x2D):
            n = self.rows.get(t, 0)
            if not n:
                continue
            widths = [width(c) for c in SCHEMA[t]]
            rowsize = sum(widths)
            self.tables[t] = (p, widths, rowsize, SCHEMA[t])
            p += n * rowsize
        for t in self.rows:
            if t >= 0x2D:
                raise ValueError(f"unknown metadata table {t:#x}")

    def row(self, t, i):
        """Row i (1-based) of table t as a list of raw column values."""
        base, widths, rowsize, _ = self.tables[t]
        p = base + (i - 1) * rowsize
        out = []
        for w in widths:
            out.append(struct.unpack_from("<H" if w == 2 else "<I" if w == 4 else "<B", self.data, p)[0])
            p += w
        return out

    def string(self, i):
        p = self.strings + i
        return self.data[p:self.data.index(b"\0", p)].decode("utf-8")

    def blob(self, i):
        p = self.blobs + i
        n, used = decompress(self.data, p)
        return self.data[p + used:p + used + n]


def decompress(buf, p):
    b = buf[p]
    if b & 0x80 == 0:
        return b, 1
    if b & 0xC0 == 0x80:
        return ((b & 0x3F) << 8) | buf[p + 1], 2
    return ((b & 0x1F) << 24) | (buf[p + 1] << 16) | (buf[p + 2] << 8) | buf[p + 3], 4


# ---------------------------------------------------------------------------------------------
# Type model
# ---------------------------------------------------------------------------------------------

# ELEMENT_TYPE -> (C++ type, size)
PRIM = {
    0x02: ("uint8_t", 1),   # bool: a C# struct bool is one byte
    0x03: ("uint16_t", 2),  # char
    0x04: ("int8_t", 1), 0x05: ("uint8_t", 1),
    0x06: ("int16_t", 2), 0x07: ("uint16_t", 2),
    0x08: ("int32_t", 4), 0x09: ("uint32_t", 4),
    0x0A: ("int64_t", 8), 0x0B: ("uint64_t", 8),
    0x0C: ("float", 4), 0x0D: ("double", 8),
    0x18: ("void*", 8), 0x19: ("void*", 8),  # IntPtr / UIntPtr on x64
}
TD_EXPLICIT_LAYOUT = 0x10
FIELD_STATIC, FIELD_LITERAL = 0x10, 0x40


class Model:
    def __init__(self, md):
        self.md = md
        n_types = md.rows.get(0x02, 0)
        n_fields = md.rows.get(0x04, 0)
        self.types = []
        for i in range(1, n_types + 1):
            flags, name, ns, extends, field_start, _ = md.row(0x02, i)
            self.types.append({"flags": flags, "name": md.string(name), "ns": md.string(ns),
                               "extends": extends, "fields": field_start})
        for i, t in enumerate(self.types):
            end = self.types[i + 1]["fields"] if i + 1 < n_types else n_fields + 1
            t["fields"] = range(t["fields"], end)
        self.enclosing = {}
        for i in range(1, md.rows.get(0x29, 0) + 1):
            nested, outer = md.row(0x29, i)
            self.enclosing[nested] = outer
        self.layout = {}
        for i in range(1, md.rows.get(0x0F, 0) + 1):
            pack, size, parent = md.row(0x0F, i)
            self.layout[parent] = (pack, size)
        self.explicit_offsets = set()
        for i in range(1, md.rows.get(0x10, 0) + 1):
            self.explicit_offsets.add(md.row(0x10, i)[1])
        self.constants = {}
        for i in range(1, md.rows.get(0x0B, 0) + 1):
            etype, _, parent, value = md.row(0x0B, i)
            if parent & 3 == 0:  # a Field
                self.constants[parent >> 2] = (etype, md.blob(value))

    def outermost(self, ti):
        while ti in self.enclosing:
            ti = self.enclosing[ti]
        return self.types[ti - 1]["name"]

    def field(self, fi):
        flags, name, sig = self.md.row(0x04, fi)
        return flags, self.md.string(name), self.md.blob(sig)

    def typeref_name(self, ri):
        _, name, ns = self.md.row(0x01, ri)
        return self.md.string(ns), self.md.string(name)

    def base_is(self, ti, ns, name):
        ext = self.types[ti - 1]["extends"]
        return ext & 3 == 1 and self.typeref_name(ext >> 2) == (ns, name)

    def instance_fields(self, ti):
        out = []
        for fi in self.types[ti - 1]["fields"]:
            flags, name, sig = self.field(fi)
            if flags & (FIELD_STATIC | FIELD_LITERAL):
                continue
            if fi in self.explicit_offsets:
                raise SystemExit(f"{self.types[ti - 1]['name']}.{name} has an explicit offset; "
                                 "this generator only handles sequential layout")
            out.append((fi, name, sig))
        return out

    def parse_sig(self, sig):
        """Field signature -> ('prim', etype) | ('ptr', inner) | ('def', ti) | ('ref', ns, name)."""
        if sig[0] != 0x06:
            raise ValueError("not a field signature")
        p = 1
        while sig[p] in (0x1F, 0x20):  # custom modifiers
            p += 1
            _, used = decompress(sig, p)
            p += used
        return self._type(sig, p)[0]

    def _type(self, sig, p):
        et = sig[p]
        p += 1
        if et in PRIM:
            return ("prim", et), p
        if et == 0x01:
            return ("void",), p
        if et == 0x0F:
            inner, p = self._type(sig, p)
            return ("ptr", inner), p
        if et == 0x11:
            tok, used = decompress(sig, p)
            p += used
            table, idx = tok & 3, tok >> 2
            if table == 0:
                return ("def", idx), p
            if table == 1:
                return ("ref",) + self.typeref_name(idx), p
        raise SystemExit(f"unsupported field type {et:#x} in a message struct")


def find_types(model):
    """Short name -> TypeDef index, for every struct and enum inside the SCOPES types."""
    found = {}
    for ti, t in enumerate(model.types, 1):
        if model.outermost(ti) not in SCOPES or ti not in model.enclosing:
            continue
        if not (model.base_is(ti, "System", "ValueType") or model.base_is(ti, "System", "Enum")):
            continue
        if t["name"] in found:
            raise SystemExit(f"two types named {t['name']} in {SCOPES}; names would collide")
        found[t["name"]] = ti
    return found


class Layout:
    def __init__(self, model):
        self.m = model
        self.cache = {}

    def fixed_buffer(self, ti):
        """(element C++ type, element size, count) for a compiler-generated fixed buffer."""
        t = self.m.types[ti - 1]
        if "FixedBuffer" not in t["name"]:
            return None
        (_, _, sig), = self.m.instance_fields(ti)
        kind = self.m.parse_sig(sig)
        cty, esz = PRIM[kind[1]]
        _, size = self.m.layout[ti]
        return cty, esz, size // esz

    def enum_prim(self, ti):
        if not self.m.base_is(ti, "System", "Enum"):
            return None
        (_, _, sig), = self.m.instance_fields(ti)
        return PRIM[self.m.parse_sig(sig)[1]]

    def describe(self, kind):
        """(C++ type, size, align, array count, referenced struct TypeDef or None)."""
        if kind[0] == "prim":
            cty, sz = PRIM[kind[1]]
            return cty, sz, sz, 1, None
        if kind[0] == "ptr":
            inner = kind[1]
            if inner[0] == "prim":
                return PRIM[inner[1]][0].replace("void*", "void") + "*", 8, 8, 1, None
            if inner[0] == "def" and not self.enum_prim(inner[1]):
                return self.m.types[inner[1] - 1]["name"] + "*", 8, 8, 1, inner[1]
            return "void*", 8, 8, 1, None
        if kind[0] == "def":
            ti = kind[1]
            fb = self.fixed_buffer(ti)
            if fb:
                return fb[0], fb[1], fb[1], fb[2], None
            ep = self.enum_prim(ti)
            if ep:
                return ep[0], ep[1], ep[1], 1, None
            size, align = self.struct(ti)
            return self.m.types[ti - 1]["name"], size, align, 1, ti
        raise SystemExit(f"field of external type {kind[1]}.{kind[2]} in a message struct")

    def struct(self, ti):
        """Sequential layout with the declared Pack: (size, align), matching C# sizeof(T)."""
        if ti in self.cache:
            return self.cache[ti]
        if self.m.types[ti - 1]["flags"] & TD_EXPLICIT_LAYOUT:
            raise SystemExit(f"{self.m.types[ti - 1]['name']} uses explicit layout")
        pack, class_size = self.m.layout.get(ti, (0, 0))
        pack = pack or 8
        offset, align = 0, 1
        for _, _, sig in self.m.instance_fields(ti):
            _, sz, a, count, _ = self.describe(self.m.parse_sig(sig))
            a = min(a, pack)
            offset = (offset + a - 1) // a * a + sz * count
            align = max(align, a)
        size = (offset + align - 1) // align * align
        if class_size and class_size != size:
            size = class_size  # an explicit StructLayout Size wins
        self.cache[ti] = (size, align)
        return size, align


def generate(dll):
    model = Model(Metadata(dll))
    types = find_types(model)
    lay = Layout(model)

    hash_ti = next((i for i, t in enumerate(model.types, 1) if t["name"] == HASH_ENUM), None)
    if not hash_ti:
        raise SystemExit(f"{HASH_ENUM} not found; is this the game's Assembly-CSharp.dll?")
    hashes = []
    for fi in model.types[hash_ti - 1]["fields"]:
        flags, name, _ = model.field(fi)
        if flags & FIELD_LITERAL:
            etype, blob = model.constants[fi]
            hashes.append((name, struct.unpack("<i", blob[:4])[0]))

    missing = [r for r in ROOTS if r not in types]
    if missing:
        raise SystemExit("not found in this game build: " + ", ".join(missing))

    order, seen = [], set()

    def collect(ti):
        if ti in seen:
            return
        seen.add(ti)
        order.append(ti)
        for _, _, sig in model.instance_fields(ti):
            ref = lay.describe(model.parse_sig(sig))[4]
            if ref:
                collect(ref)

    for r in ROOTS:
        collect(types[r])

    lines = [
        "// Generated by tools/gen_sim_abi.py from the game's Assembly-CSharp.dll.",
        "// Do not edit by hand; re-run after every game update.",
        "#pragma once",
        "#include <cstdint>",
        "",
        "namespace oni_sim {",
        "",
        "enum class SimMessageHash : int32_t {",
    ]
    lines += [f"    {n} = {v}," for n, v in hashes]
    lines += ["};", "", "// Forward declarations, so pointer fields can name structs defined later."]
    lines += [f"struct {model.types[ti - 1]['name']};" for ti in order]
    lines.append("")

    emitted = set()

    def emit(ti):
        if ti in emitted:
            return
        emitted.add(ti)
        fields = []
        for _, name, sig in model.instance_fields(ti):
            kind = model.parse_sig(sig)
            cty, _, _, count, ref = lay.describe(kind)
            if ref and kind[0] == "def":
                emit(ref)  # a by-value member must be complete first
            fields.append(f"    {cty} {name}{f'[{count}]' if count != 1 else ''};")
        name = model.types[ti - 1]["name"]
        pack = model.layout.get(ti, (0, 0))[0] or 8
        lines.append(f"#pragma pack(push, {pack})")
        lines.append(f"struct {name} {{")
        lines.extend(fields)
        lines.append("};")
        lines.append("#pragma pack(pop)")
        lines.append(f'static_assert(sizeof({name}) == {lay.struct(ti)[0]}, "{name} layout drift");')
        lines.append("")

    for ti in order:
        emit(ti)
    lines += ["}  // namespace oni_sim", ""]
    return "\n".join(lines), len(hashes), len(order)


def main():
    ap = argparse.ArgumentParser(description="Generate abi/sim_abi.h from the game's Assembly-CSharp.dll.")
    ap.add_argument("game", help="Assembly-CSharp.dll, or the game's install directory")
    ap.add_argument("-o", "--output", default=os.path.join(os.path.dirname(__file__), "..", "abi", "sim_abi.h"))
    a = ap.parse_args()
    dll = a.game
    if os.path.isdir(dll):
        dll = os.path.join(dll, "OxygenNotIncluded_Data", "Managed", "Assembly-CSharp.dll")
    if not os.path.isfile(dll):
        sys.exit(f"gen_sim_abi: not found: {dll}")
    text, n_hash, n_struct = generate(dll)
    os.makedirs(os.path.dirname(os.path.abspath(a.output)), exist_ok=True)
    with open(a.output, "w", encoding="utf-8", newline="\n") as f:
        f.write(text)
    print(f"gen_sim_abi: {n_hash} message ids, {n_struct} structs -> {a.output}")


if __name__ == "__main__":
    main()
