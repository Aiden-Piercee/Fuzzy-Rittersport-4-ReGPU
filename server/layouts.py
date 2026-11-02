"""Load xtsw-layouts.txt (the game's own type descriptors) into xtsw schemas.

The dump lists every Forza*_Params / Forza*_Results record: members in wire
order with a kind, nested structs and array element types by indentation.
Offsets, sizes and flags describe the game's in-memory structs and are not used
on the wire. Mapping: kinds u8 u16 i16 i32 u32 i64 u64 float bool string bytes
guid systemtime blob8 plus struct and array.

Licence: AGPL-3.0-or-later.
"""
import os, re

_KIND = {"u8": "u8", "u16": "u16", "i16": "i16", "i32": "i32", "u32": "u32", "i64": "i64",
         "u64": "u64", "float": "f32", "bool": "bool", "string": "str", "bytes": "bytes",
         "guid": "guid", "systemtime": "time", "blob8": "blob8"}
_HEAD = re.compile(r"^=== (\w+) @")
_MEMBER = re.compile(r"^( +)([\w.]+) kind=(\w+)\(\d+\)")


def _build(entries):
    """entries: [(indent, name, kind)] in file order -> schema (field list)."""
    def parse(i, indent):
        fields = []
        while i < len(entries) and entries[i][0] >= indent:
            ind, name, kind = entries[i]
            if ind != indent:
                raise ValueError(f"bad indent at {name}")
            i += 1
            if kind == "struct":
                sub, i = parse(i, indent + 2) if i < len(entries) and entries[i][0] > indent else ([], i)
                fields.append((name, sub))
            elif kind == "array":
                if i >= len(entries) or entries[i][0] <= indent:
                    raise ValueError(f"array {name} has no element type")
                ind2, ename, ekind = entries[i]
                i += 1
                if ekind == "struct":
                    sub, i = parse(i, ind2 + 2) if i < len(entries) and entries[i][0] > ind2 else ([], i)
                    fields.append((name, ("array", sub)))
                else:
                    fields.append((name, ("array", _KIND[ekind])))
            else:
                fields.append((name, _KIND[kind]))
        return fields, i
    return parse(0, entries[0][0])[0] if entries else []


def load(path=None):
    path = path or os.path.join(os.path.dirname(os.path.abspath(__file__)), "xtsw-layouts.txt")
    out, cur, entries = {}, None, []
    with open(path, encoding="utf-8") as f:
        for line in f:
            line = line.rstrip("\n")
            h = _HEAD.match(line)
            if h:
                if cur:
                    out[cur] = _build(entries)
                cur, entries = h.group(1), []
                continue
            m = _MEMBER.match(line)
            if m and cur:
                entries.append((len(m.group(1)), m.group(2), m.group(3)))
    if cur:
        out[cur] = _build(entries)
    return out


_CACHE = {}


def table():
    if not _CACHE:
        _CACHE.update(load())
    return _CACHE


def schema(record):
    """e.g. schema('ForzaAuctionPlaceBid_Params'); a missing record is an empty body."""
    return table().get(record, [])


def request(cls, method):
    for stem in _stems(cls, method):
        if f"{stem}_Params" in table():
            return table()[f"{stem}_Params"]
    return []


def result(cls, method):
    return result_for(cls, method) or []


def fill(sch, *sources):
    """Build a record for `sch`: each field is taken from the first source dict that has it,
    recursing into nested structs; arrays and everything else default to empty/zero."""
    rec = {}
    for name, t in sch:
        for s in sources:
            if isinstance(s, dict) and name in s:
                v = s[name]
                break
        else:
            v = None
        if isinstance(t, list):
            subs = [v] if isinstance(v, dict) else list(sources)
            rec[name] = fill(t, *subs)
        elif v is not None:
            rec[name] = v
    return rec


def _stems(cls, method):
    return (f"Forza{cls}{method}", f"ForzaWebServices{cls}{method}", f"{cls}{method}",
            f"WebServices{cls}{method}")


def result_for(cls, method):
    """Results schema for a service call, or None. Some records carry a WebServices infix."""
    t = table()
    for stem in _stems(cls, method):
        if f"{stem}_Results" in t:
            return t[f"{stem}_Results"]
    return None


def has_request(cls, method):
    t = table()
    return any(f"{stem}_Params" in t for stem in _stems(cls, method))
