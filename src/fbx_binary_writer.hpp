#pragma once

// Binary-FBX serializer for the format-agnostic FbxNode tree (see fbx_node.hpp). Node record
// layout, property type codes, and array encoding are all verified directly against the vendored
// ufbx.c's binary *reader* (ufbxi_binary_parse_node() and friends) rather than from memory, since
// this needs to match what real tools (Maya included) actually expect on disk - see the comments
// in fbx_binary_writer.cpp for exact source line references.

#include "fbx_node.hpp"

#include <godot_cpp/variant/packed_byte_array.hpp>

using namespace godot;

// p_version must be < 7500 (this writer only implements the 32-bit-offset node header layout,
// i.e. FBX 7.0-7.4x - plenty modern for every current DCC tool/engine, and what the rest of this
// plugin already targets via FbxManager's ufbx_load_opts).
PackedByteArray fbx_binary_serialize(const FbxNode &p_root, int32_t p_version = 7400);
