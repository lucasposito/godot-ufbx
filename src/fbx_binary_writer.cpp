#include "fbx_binary_writer.hpp"

#include <godot_cpp/variant/char_string.hpp>

#include <cstring>

namespace {

// Append-only byte buffer with one backpatch operation (patch_u32), used to fill in each node's
// EndOffset/PropertyListLen only once its real size is known - see write_node().
struct ByteBuf {
	PackedByteArray buf;

	int64_t size() const { return buf.size(); }

	void u8(uint8_t p_v) { buf.push_back(p_v); }

	void u32(uint32_t p_v) {
		u8((uint8_t)(p_v & 0xFF));
		u8((uint8_t)((p_v >> 8) & 0xFF));
		u8((uint8_t)((p_v >> 16) & 0xFF));
		u8((uint8_t)((p_v >> 24) & 0xFF));
	}

	void i32(int32_t p_v) { u32((uint32_t)p_v); }

	void i16(int16_t p_v) {
		uint16_t v = (uint16_t)p_v;
		u8((uint8_t)(v & 0xFF));
		u8((uint8_t)((v >> 8) & 0xFF));
	}

	void u64(uint64_t p_v) {
		for (int i = 0; i < 8; i++) {
			u8((uint8_t)((p_v >> (8 * i)) & 0xFF));
		}
	}

	void i64(int64_t p_v) { u64((uint64_t)p_v); }

	void f32(float p_v) {
		uint32_t bits;
		memcpy(&bits, &p_v, 4);
		u32(bits);
	}

	void f64(double p_v) {
		uint64_t bits;
		memcpy(&bits, &p_v, 8);
		u64(bits);
	}

	void zeros(int p_count) {
		for (int i = 0; i < p_count; i++) {
			u8(0);
		}
	}

	void char_bytes(const CharString &p_cs) {
		for (int64_t i = 0; i < p_cs.length(); i++) {
			u8((uint8_t)p_cs[i]);
		}
	}

	void patch_u32(int64_t p_pos, uint32_t p_v) {
		buf.set(p_pos, (uint8_t)(p_v & 0xFF));
		buf.set(p_pos + 1, (uint8_t)((p_v >> 8) & 0xFF));
		buf.set(p_pos + 2, (uint8_t)((p_v >> 16) & 0xFF));
		buf.set(p_pos + 3, (uint8_t)((p_v >> 24) & 0xFF));
	}
};

// Property type codes and array-header layout verified against ufbx.c's binary *reader* (see
// fbx_binary_writer.hpp's header comment for the exact source lines) - not guessed from memory.
void write_prop(ByteBuf &w, const FbxProp &p) {
	switch (p.type) {
		case FbxPropType::BOOL:
			w.u8('C');
			w.u8(p.b ? 1 : 0);
			break;
		case FbxPropType::INT16:
			w.u8('Y');
			w.i16((int16_t)p.i);
			break;
		case FbxPropType::INT32:
			w.u8('I');
			w.i32((int32_t)p.i);
			break;
		case FbxPropType::INT64:
			w.u8('L');
			w.i64(p.i);
			break;
		case FbxPropType::FLOAT32:
			w.u8('F');
			w.f32((float)p.d);
			break;
		case FbxPropType::FLOAT64:
			w.u8('D');
			w.f64(p.d);
			break;
		case FbxPropType::STRING: {
			CharString cs = p.s.utf8();
			w.u8('S');
			w.u32((uint32_t)cs.length());
			w.char_bytes(cs);
			break;
		}
		case FbxPropType::RAW_BYTES: {
			w.u8('R');
			w.u32((uint32_t)p.raw.size());
			for (int i = 0; i < p.raw.size(); i++) {
				w.u8(p.raw[i]);
			}
			break;
		}
		case FbxPropType::TYPE_AND_NAME: {
			// "Name\x00\x01Type" - reversed order and a different separator than the ASCII
			// "Type::Name" convention (see ufbxi_split_type_and_name() in ufbx.c). The embedded
			// NUL is safe here since this is a length-prefixed string, not C-string terminated.
			CharString name_cs = p.s.utf8();
			CharString type_cs = p.s2.utf8();
			w.u8('S');
			w.u32((uint32_t)(name_cs.length() + 2 + type_cs.length()));
			w.char_bytes(name_cs);
			w.u8(0x00);
			w.u8(0x01);
			w.char_bytes(type_cs);
			break;
		}
		case FbxPropType::INT32_ARRAY:
			w.u8('i');
			w.u32((uint32_t)p.i32a.size());
			w.u32(0); // Encoding: raw (no zlib).
			w.u32((uint32_t)(p.i32a.size() * 4));
			for (int i = 0; i < p.i32a.size(); i++) {
				w.i32(p.i32a[i]);
			}
			break;
		case FbxPropType::INT64_ARRAY:
			w.u8('l');
			w.u32((uint32_t)p.i64a.size());
			w.u32(0);
			w.u32((uint32_t)(p.i64a.size() * 8));
			for (int i = 0; i < p.i64a.size(); i++) {
				w.i64(p.i64a[i]);
			}
			break;
		case FbxPropType::FLOAT32_ARRAY:
			w.u8('f');
			w.u32((uint32_t)p.f32a.size());
			w.u32(0);
			w.u32((uint32_t)(p.f32a.size() * 4));
			for (int i = 0; i < p.f32a.size(); i++) {
				w.f32(p.f32a[i]);
			}
			break;
		case FbxPropType::FLOAT64_ARRAY:
			w.u8('d');
			w.u32((uint32_t)p.f64a.size());
			w.u32(0);
			w.u32((uint32_t)(p.f64a.size() * 8));
			for (int i = 0; i < p.f64a.size(); i++) {
				w.f64(p.f64a[i]);
			}
			break;
	}
}

// Node record layout (FBX version < 7500): EndOffset(u32), NumProperties(u32),
// PropertyListLen(u32), NameLen(u8), Name, property data, [child records...], then a 13-byte
// all-zero NULL sentinel if there are any children - verified against
// ufbxi_binary_parse_node() in ufbx.c. EndOffset/PropertyListLen are backpatched once their real
// values are known, same pattern as build_skeleton_bones() et al. compose transforms forward
// rather than needing a second pass elsewhere in this codebase.
//
// force_braces also triggers the terminator, not just node.children.size() > 0: real Maya's own
// binary writer emits the 13-byte NULL sentinel even for an empty braced {} block (its top-level
// "References" node, which has no children at all) - the terminator is tied to "this node was
// written as a braced block", same condition force_braces already tracks for ASCII output, not to
// whether it ended up with any children.
void write_node(ByteBuf &w, const FbxNode &node) {
	int64_t end_offset_pos = w.size();
	w.u32(0); // EndOffset placeholder.
	w.u32((uint32_t)node.props.size());
	int64_t prop_list_len_pos = w.size();
	w.u32(0); // PropertyListLen placeholder.

	CharString name_cs = node.name.utf8();
	w.u8((uint8_t)name_cs.length());
	w.char_bytes(name_cs);

	int64_t props_start = w.size();
	for (int i = 0; i < node.props.size(); i++) {
		write_prop(w, node.props[i]);
	}
	w.patch_u32(prop_list_len_pos, (uint32_t)(w.size() - props_start));

	if (node.children.size() > 0 || node.force_braces) {
		for (int i = 0; i < node.children.size(); i++) {
			write_node(w, node.children[i]);
		}
		w.zeros(13); // NULL sentinel terminating this node's child list.
	}

	w.patch_u32(end_offset_pos, (uint32_t)w.size());
}

} //namespace

PackedByteArray fbx_binary_serialize(const FbxNode &p_root, int32_t p_version) {
	ByteBuf w;

	// Magic header + version - see fbx_binary_writer.hpp.
	static const char magic[20] = { 'K', 'a', 'y', 'd', 'a', 'r', 'a', ' ', 'F', 'B', 'X', ' ', 'B', 'i', 'n', 'a', 'r', 'y', ' ', ' ' };
	for (int i = 0; i < 20; i++) {
		w.u8((uint8_t)magic[i]);
	}
	w.u8(0x00);
	w.u8(0x1A);
	w.u8(0x00);
	w.u32((uint32_t)p_version);

	for (int i = 0; i < p_root.children.size(); i++) {
		write_node(w, p_root.children[i]);
	}
	w.zeros(13); // Top-level NULL sentinel.

	// Footer: provably never read by ufbx (parsing stops at the top-level NULL sentinel - see
	// ufbxi_parse_toplevel() in ufbx.c), but unlike everything above it, Maya's own FBX SDK reader
	// silently imports zero objects (no error, no warning) from a file whose footer isn't *exactly*
	// this, verbatim. This is the literal 170 footer bytes from a real Maya-exported reference file
	// (test_project/maya_export_reference.py), captured once and reused unconditionally - same
	// rationale as the FileId/CreationTime triple in fbx_scene_writer.cpp's export_scene(). It
	// doesn't need to correspond to this file's actual content or length, just be byte-for-byte
	// what a real FBX SDK build produced. Contains the version (7400) at a fixed offset, so this
	// footer is only valid for p_version == 7400 - the only version this writer emits.
	static const uint8_t footer[170] = {
		0xfa, 0xbc, 0xab, 0x0a, 0xd3, 0xc1, 0xdd, 0x6d, 0xbb, 0x7a, 0xfe, 0x81, 0x14, 0xf1, 0x2f, 0x7f,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0xe8, 0x1c, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0xf8, 0x5a, 0x8c, 0x6a, 0xde, 0xf5, 0xd9, 0x7e, 0xec, 0xe9, 0x0c, 0xe3, 0x75, 0x8f, 0x29, 0x0b
	};
	for (int i = 0; i < 170; i++) {
		w.u8(footer[i]);
	}

	return w.buf;
}
