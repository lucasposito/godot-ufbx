#pragma once

#include <godot_cpp/classes/node3d.hpp>
#include <godot_cpp/classes/ref.hpp>
#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/string.hpp>

#include "ufbx.h"

using namespace godot;

class FbxScene;
class FbxSceneWriter;

class FbxManager : public RefCounted {
	GDCLASS(FbxManager, RefCounted)

protected:
	static void _bind_methods();

public:
	// Quick way to test that a file can be parsed, without building any Godot nodes.
	// Returns a Dictionary with at least "success": bool.
	// On success also includes: node_count, mesh_count, material_count, animation_stack_count.
	// On failure also includes: "error" (String description).
	Dictionary get_scene_info(const String &p_path) const;

	// Loads an FBX/OBJ file and converts it into a Node3D tree (MeshInstance3D for nodes with geometry).
	// Returns nullptr on failure and prints the ufbx error via UtilityFunctions::printerr.
	// Ownership of the returned node is transferred to the caller (add it to the scene tree,
	// it is not automatically freed).
	Node3D *import_scene(const String &p_path) const;

	// Reads the raw elements that form a single mesh (vertices, per-corner indices, polygon
	// sizes, normals, UVs, and the transform of its first instance) without building any Godot
	// resources. p_mesh_index refers to the mesh's position in the file's mesh list (see
	// get_scene_info's "mesh_count"). Returns an empty Dictionary if the file fails to load or
	// the index is out of range.
	Dictionary get_mesh_data(const String &p_path, int p_mesh_index) const;

	// Reads material assignments for a single mesh, mirroring the logic and dictionary keys of
	// https://github.com/lucasposito/FbxData's material.py: "geometry" (owning node name),
	// "materials" (material names in order), "paths" (per-material DiffuseColor/NormalMap
	// texture paths), and "face_id_map" (material index -> polygon indices using it). Returns an
	// empty Dictionary if the file fails to load or the index is out of range.
	Dictionary get_material_data(const String &p_path, int p_mesh_index) const;

	// Parses an FBX/OBJ file and returns an FbxScene wrapping it, without building any Godot
	// meshes/materials/nodes yet. Use FbxScene's per-mesh entries to inspect and selectively
	// build geometry/materials, then call FbxScene::import() to assemble the final tree.
	// Returns a null Ref on failure (prints the ufbx error via UtilityFunctions::printerr).
	Ref<FbxScene> load_scene(const String &p_path) const;

	// Creates a fresh, empty in-memory FBX scene to build up via FbxSceneWriter's set_mesh() /
	// set_uv() / set_normal() / set_material() / set_skeleton() / set_skin(), then write out with
	// export_scene(). Pure in-memory construction - no file or ufbx parsing involved, unlike every
	// other method here.
	Ref<FbxSceneWriter> new_scene() const;

private:
	Node3D *_build_node(ufbx_node *p_node, const String &p_fbx_dir) const;
};
