"""Big-endian packed records used by the Turn 10 title services (bin/xtsw bodies).

Layout rules (as documented by forza-live, docs/title_services.md, AGPL-3.0):
fixed-width big-endian integers and floats, bool = 1 byte, GUID = 16 bytes,
string and byte array = u32 length + data, array = u32 count + elements,
timestamps are ISO 8601 text. Fields are packed in declaration order.

A schema is a list of (name, type). A type is a scalar name below, a nested
schema (list), or ("array", type). Licence: AGPL-3.0-or-later.
"""
import struct
from datetime import datetime, timezone

_FIXED = {"u8": ">B", "u16": ">H", "i16": ">h", "u32": ">I", "i32": ">i", "u64": ">Q",
          "i64": ">q", "f32": ">f", "f64": ">d"}


class DecodeError(ValueError):
    pass


class Reader:
    def __init__(self, b):
        self.b, self.i = bytes(b), 0

    def remaining(self):
        return len(self.b) - self.i

    def take(self, n):
        if n < 0 or self.i + n > len(self.b):
            raise DecodeError(f"need {n} bytes at {self.i}, have {self.remaining()}")
        v = self.b[self.i:self.i + n]
        self.i += n
        return v

    def scalar(self, t):
        if t in _FIXED:
            f = _FIXED[t]
            return struct.unpack(f, self.take(struct.calcsize(f)))[0]
        if t == "bool":
            return self.take(1)[0] != 0
        if t == "guid":
            return self.take(16).hex()
        if t in ("str", "time"):
            return self.take(self.scalar("u32")).decode("utf-8", "replace")
        if t == "bytes":
            return self.take(self.scalar("u32"))
        raise DecodeError(f"unknown type {t!r}")

    def value(self, t):
        if isinstance(t, list):
            return self.record(t)
        if isinstance(t, tuple) and t[0] == "array":
            n = self.scalar("u32")
            if n > 1_000_000:
                raise DecodeError(f"array count {n} is not plausible")
            return [self.value(t[1]) for _ in range(n)]
        return self.scalar(t)

    def record(self, schema):
        return {name: self.value(t) for name, t in schema}


def decode(schema, body):
    return Reader(body).record(schema)


def _scalar(t, v):
    if t in _FIXED:
        return struct.pack(_FIXED[t], v)
    if t == "bool":
        return b"\1" if v else b"\0"
    if t == "guid":
        g = bytes.fromhex(v) if isinstance(v, str) else bytes(v)
        return g.rjust(16, b"\0")[:16]
    if t in ("str", "time"):
        b = v.encode("utf-8")
        return struct.pack(">I", len(b)) + b
    if t == "bytes":
        return struct.pack(">I", len(v)) + bytes(v)
    raise ValueError(f"unknown type {t!r}")


def _value(t, v):
    if isinstance(t, list):
        return encode(t, v)
    if isinstance(t, tuple) and t[0] == "array":
        return struct.pack(">I", len(v)) + b"".join(_value(t[1], x) for x in v)
    return _scalar(t, v)


def encode(schema, rec):
    """Missing fields encode as zero/empty so partial dicts are fine."""
    out = []
    for name, t in schema:
        v = rec.get(name)
        if v is None:
            v = _default(t)
        out.append(_value(t, v))
    return b"".join(out)


def _default(t):
    if isinstance(t, list):
        return {}
    if isinstance(t, tuple):
        return []
    return {"bool": False, "guid": "", "str": "", "time": iso(0), "bytes": b""}.get(t, 0)


def iso(ts):
    return datetime.fromtimestamp(ts, timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")


def parse_iso(s):
    return datetime.strptime(s, "%Y-%m-%dT%H:%M:%SZ").replace(tzinfo=timezone.utc).timestamp()
