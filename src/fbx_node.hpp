#pragma once

// Format-agnostic FBX object-graph node/property tree, built once by FbxSceneWriter::export_scene()
// (fbx_scene_writer.cpp) and consumed by two independent serializers: fbx_ascii_writer.cpp (text)
// and fbx_binary_writer.cpp (the real Autodesk binary format). Keeping the tree itself untyped-by-
// format means the ~700 lines of mesh/skeleton/material/skin object-graph construction only need
// to exist once - each serializer only has to know how to render an already-built FbxProp/FbxNode,
// not how to derive one from a WriterMesh/WriterBone.

#include <godot_cpp/templates/vector.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/string.hpp>

using namespace godot;

enum class FbxPropType {
	BOOL,
	INT16,
	INT32,
	INT64,
	FLOAT32,
	FLOAT64,
	STRING,
	// The ASCII "Type::Name" object-header convention is encoded as "Name\x00\x01Type" in binary
	// (name first, reversed order, NUL+SOH separator instead of "::" - see
	// ufbxi_split_type_and_name() in ufbx.c) - the two formats render this differently, so it's
	// its own prop type (s = name, s2 = type) rather than something callers pre-format themselves.
	TYPE_AND_NAME,
	// Binary FBX's 'R' type - an opaque length-prefixed byte blob (used for the top-level FileId).
	// Rendered as a hex string in ASCII output since its content is never meant to be parsed back.
	RAW_BYTES,
	INT32_ARRAY,
	INT64_ARRAY,
	FLOAT32_ARRAY,
	FLOAT64_ARRAY,
};

struct FbxProp {
	FbxPropType type = FbxPropType::INT32;
	bool b = false;
	int64_t i = 0;
	double d = 0.0;
	String s;
	String s2; // Only used by TYPE_AND_NAME (the "type" half; s holds the "name" half).
	PackedByteArray raw; // Only used by RAW_BYTES.
	Vector<int32_t> i32a;
	Vector<int64_t> i64a;
	Vector<float> f32a;
	Vector<double> f64a;
};

struct FbxNode {
	String name;
	Vector<FbxProp> props;
	Vector<FbxNode> children;
	// Marks this node as a braced {} block container even with zero children/props (real FBX files
	// always brace block containers like Objects/Connections/References, even when this export has
	// nothing to put in them - e.g. no skeleton). ASCII output uses this to decide whether to emit
	// "{ }"; binary output uses it to decide whether to emit the child-list NULL terminator even
	// with no real children (see write_node() in fbx_binary_writer.cpp).
	bool force_braces = false;

	FbxNode &add_child(const String &p_name);

	void add_bool(bool p_value);
	void add_int32(int32_t p_value);
	void add_int64(int64_t p_value);
	void add_double(double p_value);
	void add_string(const String &p_value);
	void add_type_and_name(const String &p_type, const String &p_name);
	void add_raw_bytes(const PackedByteArray &p_value);
	void add_int32_array(const Vector<int32_t> &p_values);
	void add_double_array(const Vector<double> &p_values);

	// Adds and returns a Properties70 "P:" child node (name/type/subtype/flags props already
	// filled in) - the caller pushes the property's own value(s) onto it with the methods above,
	// e.g. `props.add_property("Lcl Translation", "Lcl Translation", "", "A").add_double(x)...`
	// isn't legal C++ chaining here since the setters return void; call add_double() on the
	// returned reference in separate statements instead.
	FbxNode &add_property(const String &p_name, const String &p_type, const String &p_subtype, const String &p_flags);
};
