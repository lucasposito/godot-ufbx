#include "fbx_ascii_writer.hpp"

namespace {

String quote_and_escape(const String &p_value) {
	String escaped = p_value.replace("\\", "\\\\").replace("\"", "\\\"");
	return "\"" + escaped + "\"";
}

// Precision generous enough that a round-tripped double loses nothing a viewer would notice
// (double has ~15-17 significant digits; this format is the debug/interop fallback now that
// export_scene() defaults to binary, not the primary compatibility target).
String format_double(double p_value) {
	return String::num(p_value, 9);
}

String format_scalar_prop(const FbxProp &p_prop) {
	switch (p_prop.type) {
		case FbxPropType::BOOL:
			return p_prop.b ? "1" : "0";
		case FbxPropType::INT16:
		case FbxPropType::INT32:
		case FbxPropType::INT64:
			return String::num_int64(p_prop.i);
		case FbxPropType::FLOAT32:
		case FbxPropType::FLOAT64:
			return format_double(p_prop.d);
		case FbxPropType::STRING:
			return quote_and_escape(p_prop.s);
		case FbxPropType::TYPE_AND_NAME:
			return quote_and_escape(p_prop.s2 + "::" + p_prop.s);
		case FbxPropType::RAW_BYTES: {
			String hex;
			for (int i = 0; i < p_prop.raw.size(); i++) {
				hex += String::num_uint64((uint64_t)p_prop.raw[i], 16).lpad(2, "0");
			}
			return quote_and_escape(hex);
		}
		default:
			return String(); // Array types are never rendered inline - see is_sole_array_prop().
	}
}

// True if p_node's properties are exactly one array-typed prop - the only shape our own builder
// ever produces for array-typed nodes (e.g. "Vertices", "PolygonVertexIndex", "Weights"), and the
// one that gets the special "Name: *N { a: v1,v2,... }" block rendering instead of an inline list.
bool is_sole_array_prop(const FbxNode &p_node) {
	if (p_node.props.size() != 1) {
		return false;
	}
	FbxPropType t = p_node.props[0].type;
	return t == FbxPropType::INT32_ARRAY || t == FbxPropType::INT64_ARRAY ||
			t == FbxPropType::FLOAT32_ARRAY || t == FbxPropType::FLOAT64_ARRAY;
}

String join_array_values(const FbxProp &p_prop) {
	String out;
	switch (p_prop.type) {
		case FbxPropType::INT32_ARRAY:
			for (int i = 0; i < p_prop.i32a.size(); i++) {
				if (i > 0) {
					out += ",";
				}
				out += String::num_int64(p_prop.i32a[i]);
			}
			break;
		case FbxPropType::INT64_ARRAY:
			for (int i = 0; i < p_prop.i64a.size(); i++) {
				if (i > 0) {
					out += ",";
				}
				out += String::num_int64(p_prop.i64a[i]);
			}
			break;
		case FbxPropType::FLOAT32_ARRAY:
			for (int i = 0; i < p_prop.f32a.size(); i++) {
				if (i > 0) {
					out += ",";
				}
				out += format_double(p_prop.f32a[i]);
			}
			break;
		case FbxPropType::FLOAT64_ARRAY:
			for (int i = 0; i < p_prop.f64a.size(); i++) {
				if (i > 0) {
					out += ",";
				}
				out += format_double(p_prop.f64a[i]);
			}
			break;
		default:
			break;
	}
	return out;
}

int64_t array_count(const FbxProp &p_prop) {
	switch (p_prop.type) {
		case FbxPropType::INT32_ARRAY:
			return p_prop.i32a.size();
		case FbxPropType::INT64_ARRAY:
			return p_prop.i64a.size();
		case FbxPropType::FLOAT32_ARRAY:
			return p_prop.f32a.size();
		case FbxPropType::FLOAT64_ARRAY:
			return p_prop.f64a.size();
		default:
			return 0;
	}
}

String serialize_node(const FbxNode &p_node, int p_indent) {
	String indent;
	for (int i = 0; i < p_indent; i++) {
		indent += "\t";
	}

	if (is_sole_array_prop(p_node)) {
		const FbxProp &prop = p_node.props[0];
		String out = indent + p_node.name + ": *" + String::num_int64(array_count(prop)) + " {\n";
		out += indent + "\ta: " + join_array_values(prop) + "\n";
		out += indent + "}\n";
		return out;
	}

	String line = indent + p_node.name + ":";
	if (p_node.props.size() > 0) {
		line += " ";
		for (int i = 0; i < p_node.props.size(); i++) {
			if (i > 0) {
				line += ", ";
			}
			line += format_scalar_prop(p_node.props[i]);
		}
	}

	if (p_node.children.size() == 0 && !p_node.force_braces) {
		return line + "\n";
	}

	String out = line + " {\n";
	for (int i = 0; i < p_node.children.size(); i++) {
		out += serialize_node(p_node.children[i], p_indent + 1);
	}
	out += indent + "}\n";
	return out;
}

} //namespace

String fbx_ascii_serialize_children(const FbxNode &p_root) {
	String out;
	for (int i = 0; i < p_root.children.size(); i++) {
		out += serialize_node(p_root.children[i], 0);
	}
	return out;
}
