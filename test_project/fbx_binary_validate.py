"""Independent, from-scratch binary-FBX parser (not using ufbx, not using Maya) that verifies our
own writer's byte-level output against the FBX < 7500 node-record spec: reads each node's
EndOffset/NumProperties/PropertyListLen and checks they actually match the real content that
follows, byte for byte. Used to sanity-check fbx_binary_writer.cpp's output independently of both
readers we already have (ufbx says the file is fine; Maya silently imports nothing from it), since
a bug ufbx tolerates but a stricter reader doesn't would show up here as a mismatched EndOffset or
similar structural inconsistency.

Usage: mayapy.exe fbx_binary_validate.py <path-to.fbx>   (or any Python 3 interpreter)
"""

import struct
import sys


def read_node(data, pos, depth, path):
    end_offset, num_props, prop_list_len, name_len = struct.unpack_from("<IIIB", data, pos)
    header_start = pos
    pos += 13
    if end_offset == 0 and name_len == 0:
        return None, pos  # NULL sentinel

    name = data[pos:pos + name_len].decode("latin1")
    pos += name_len

    props_start = pos
    props = []
    for i in range(num_props):
        t = chr(data[pos])
        pos += 1
        if t == "C":
            v = data[pos]
            pos += 1
            props.append(("bool", v))
        elif t == "Y":
            v = struct.unpack_from("<h", data, pos)[0]
            pos += 2
            props.append(("i16", v))
        elif t == "I":
            v = struct.unpack_from("<i", data, pos)[0]
            pos += 4
            props.append(("i32", v))
        elif t == "L":
            v = struct.unpack_from("<q", data, pos)[0]
            pos += 8
            props.append(("i64", v))
        elif t == "F":
            v = struct.unpack_from("<f", data, pos)[0]
            pos += 4
            props.append(("f32", v))
        elif t == "D":
            v = struct.unpack_from("<d", data, pos)[0]
            pos += 8
            props.append(("f64", v))
        elif t in ("S", "R"):
            slen = struct.unpack_from("<I", data, pos)[0]
            pos += 4
            v = data[pos:pos + slen]
            pos += slen
            props.append(("str" if t == "S" else "raw", v))
        elif t in ("i", "l", "f", "d", "b"):
            count, encoding, enc_size = struct.unpack_from("<III", data, pos)
            pos += 12
            pos += enc_size
            props.append((t + "[]", count, "encoding=%d" % encoding))
        else:
            raise ValueError("unknown prop type %r at pos %d (node %s)" % (t, pos - 1, path))

    actual_prop_list_len = pos - props_start
    if actual_prop_list_len != prop_list_len:
        print("MISMATCH: %s declared PropertyListLen=%d but actual=%d" % (path, prop_list_len, actual_prop_list_len))

    children = []
    if end_offset > pos:
        while True:
            child, pos = read_node(data, pos, depth + 1, path + "/" + name)
            if child is None:
                break
            children.append(child)

    if pos != end_offset:
        print("MISMATCH: %s declared EndOffset=%d but actual end pos=%d (header started at %d)" % (path, end_offset, pos, header_start))

    return {"name": name, "props": props, "children": children, "end_offset": end_offset}, pos


def dump(node, depth=0, max_depth=3):
    if depth > max_depth:
        return
    prefix = "  " * depth
    prop_summary = []
    for p in node["props"]:
        if p[0] == "str":
            prop_summary.append(repr(p[1][:40]))
        else:
            prop_summary.append(str(p[1]) if len(p) > 1 else p[0])
    print("%s%s(%s)" % (prefix, node["name"], ", ".join(prop_summary)))
    for c in node["children"]:
        dump(c, depth + 1, max_depth)


def main():
    path = sys.argv[1]
    with open(path, "rb") as f:
        data = f.read()

    print("file size:", len(data))
    magic = data[0:21]
    print("magic:", magic)
    version = struct.unpack_from("<I", data, 23)[0]
    print("version:", version)

    pos = 27
    top_nodes = []
    while pos < len(data):
        node, new_pos = read_node(data, pos, 0, "")
        if node is None:
            print("top-level NULL sentinel at pos", pos)
            pos = new_pos
            break
        top_nodes.append(node)
        pos = new_pos

    print("\ntop-level nodes:", [n["name"] for n in top_nodes])
    print("\nbytes remaining after top-level NULL sentinel (footer):", len(data) - pos)

    for n in top_nodes:
        if n["name"] == "Objects":
            print("\n--- Objects children ---")
            for c in n["children"]:
                dump(c, 0, 20)
        if n["name"] == "Connections":
            print("\n--- Connections ---")
            for c in n["children"]:
                dump(c, 0, 0)
        if n["name"] in ("FileId", "CreationTime", "Creator", "FBXHeaderExtension", "GlobalSettings", "Documents", "Definitions", "References"):
            print("\n--- %s ---" % n["name"])
            dump(n, 0, 6)


if __name__ == "__main__":
    main()
