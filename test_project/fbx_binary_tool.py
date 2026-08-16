"""Round-trip binary-FBX read/write tool (parse into a mutable tree, then re-serialize), so we can
take Maya's own known-good exported file and remove pieces of it one at a time to binary-search
for whatever Maya's FBX SDK reader actually requires that our writer doesn't produce yet.
"""

import struct


class Node:
    __slots__ = ("name", "props", "children")

    def __init__(self, name, props=None, children=None):
        self.name = name
        self.props = props or []
        self.children = children or []

    def find(self, name):
        for c in self.children:
            if c.name == name:
                return c
        return None

    def find_all(self, name):
        return [c for c in self.children if c.name == name]


def read_file(path):
    with open(path, "rb") as f:
        data = f.read()
    pos = 27
    top = []
    while pos < len(data):
        node, pos = _read_node(data, pos)
        if node is None:
            break
        top.append(node)
    return top


def _read_node(data, pos):
    end_offset, num_props, prop_list_len, name_len = struct.unpack_from("<IIIB", data, pos)
    pos += 13
    if end_offset == 0 and name_len == 0:
        return None, pos
    name = data[pos:pos + name_len].decode("latin1")
    pos += name_len

    props = []
    for _ in range(num_props):
        t = chr(data[pos])
        pos += 1
        if t == "C":
            v = data[pos]
            pos += 1
        elif t == "Y":
            v = struct.unpack_from("<h", data, pos)[0]
            pos += 2
        elif t == "I":
            v = struct.unpack_from("<i", data, pos)[0]
            pos += 4
        elif t == "L":
            v = struct.unpack_from("<q", data, pos)[0]
            pos += 8
        elif t == "F":
            v = struct.unpack_from("<f", data, pos)[0]
            pos += 4
        elif t == "D":
            v = struct.unpack_from("<d", data, pos)[0]
            pos += 8
        elif t in ("S", "R"):
            slen = struct.unpack_from("<I", data, pos)[0]
            pos += 4
            v = data[pos:pos + slen]
            pos += slen
        elif t in ("i", "l", "f", "d", "b"):
            count, encoding, enc_size = struct.unpack_from("<III", data, pos)
            pos += 12
            raw = data[pos:pos + enc_size]
            pos += enc_size
            v = (count, encoding, raw)
        else:
            raise ValueError("unknown prop type %r at %d" % (t, pos - 1))
        props.append((t, v))

    children = []
    if end_offset > pos:
        while True:
            child, pos = _read_node(data, pos)
            if child is None:
                break
            children.append(child)

    return Node(name, props, children), pos


def write_file(path, top_nodes, magic_version=7400, footer=b""):
    buf = bytearray()
    buf += b"Kaydara FBX Binary  \x00\x1a\x00"
    buf += struct.pack("<I", magic_version)
    for n in top_nodes:
        _write_node(buf, n)
    buf += b"\x00" * 13
    buf += footer
    with open(path, "wb") as f:
        f.write(bytes(buf))


def _write_node(buf, node):
    end_offset_pos = len(buf)
    buf += struct.pack("<III", 0, len(node.props), 0)
    prop_list_len_pos = end_offset_pos + 8
    name_b = node.name.encode("latin1")
    buf += struct.pack("<B", len(name_b))
    buf += name_b

    props_start = len(buf)
    for t, v in node.props:
        buf += t.encode("ascii")
        if t == "C":
            buf += struct.pack("<B", v)
        elif t == "Y":
            buf += struct.pack("<h", v)
        elif t == "I":
            buf += struct.pack("<i", v)
        elif t == "L":
            buf += struct.pack("<q", v)
        elif t == "F":
            buf += struct.pack("<f", v)
        elif t == "D":
            buf += struct.pack("<d", v)
        elif t in ("S", "R"):
            buf += struct.pack("<I", len(v))
            buf += v
        elif t in ("i", "l", "f", "d", "b"):
            count, encoding, raw = v
            buf += struct.pack("<III", count, encoding, len(raw))
            buf += raw
        else:
            raise ValueError("unknown prop type %r" % t)
    struct.pack_into("<I", buf, prop_list_len_pos, len(buf) - props_start)

    if node.children:
        for c in node.children:
            _write_node(buf, c)
        buf += b"\x00" * 13

    struct.pack_into("<I", buf, end_offset_pos, len(buf))
