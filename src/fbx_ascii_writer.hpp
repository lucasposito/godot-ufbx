#pragma once

// ASCII-FBX text serializer for the format-agnostic FbxNode tree (see fbx_node.hpp). Not
// registered with Godot - a plain C++ helper, same role as the free functions in fbx_convert.hpp.

#include "fbx_node.hpp"

// Serializes every top-level child of p_root back-to-back (p_root itself is just a container, not
// emitted) - used to turn the whole built tree into the final ASCII file text.
String fbx_ascii_serialize_children(const FbxNode &p_root);
