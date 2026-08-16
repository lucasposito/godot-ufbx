#include "fbx_node.hpp"

FbxNode &FbxNode::add_child(const String &p_name) {
	FbxNode node;
	node.name = p_name;
	children.push_back(node);
	return children.write[children.size() - 1];
}

void FbxNode::add_bool(bool p_value) {
	FbxProp p;
	p.type = FbxPropType::BOOL;
	p.b = p_value;
	props.push_back(p);
}

void FbxNode::add_int32(int32_t p_value) {
	FbxProp p;
	p.type = FbxPropType::INT32;
	p.i = p_value;
	props.push_back(p);
}

void FbxNode::add_int64(int64_t p_value) {
	FbxProp p;
	p.type = FbxPropType::INT64;
	p.i = p_value;
	props.push_back(p);
}

void FbxNode::add_double(double p_value) {
	FbxProp p;
	p.type = FbxPropType::FLOAT64;
	p.d = p_value;
	props.push_back(p);
}

void FbxNode::add_string(const String &p_value) {
	FbxProp p;
	p.type = FbxPropType::STRING;
	p.s = p_value;
	props.push_back(p);
}

void FbxNode::add_type_and_name(const String &p_type, const String &p_name) {
	FbxProp p;
	p.type = FbxPropType::TYPE_AND_NAME;
	p.s = p_name;
	p.s2 = p_type;
	props.push_back(p);
}

void FbxNode::add_raw_bytes(const PackedByteArray &p_value) {
	FbxProp p;
	p.type = FbxPropType::RAW_BYTES;
	p.raw = p_value;
	props.push_back(p);
}

void FbxNode::add_int32_array(const Vector<int32_t> &p_values) {
	FbxProp p;
	p.type = FbxPropType::INT32_ARRAY;
	p.i32a = p_values;
	props.push_back(p);
}

void FbxNode::add_double_array(const Vector<double> &p_values) {
	FbxProp p;
	p.type = FbxPropType::FLOAT64_ARRAY;
	p.f64a = p_values;
	props.push_back(p);
}

FbxNode &FbxNode::add_property(const String &p_name, const String &p_type, const String &p_subtype, const String &p_flags) {
	FbxNode &p = add_child("P");
	p.add_string(p_name);
	p.add_string(p_type);
	p.add_string(p_subtype);
	p.add_string(p_flags);
	return p;
}
