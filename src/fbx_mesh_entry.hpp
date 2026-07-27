#pragma once

#include <godot_cpp/classes/array_mesh.hpp>
#include <godot_cpp/classes/ref.hpp>
#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/classes/standard_material3d.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/templates/vector.hpp>

#include "ufbx.h"

using namespace godot;

class FbxScene;

// A single mesh from a parsed FBX scene (see FbxScene::get_mesh / get_meshes). Lets you inspect
// a mesh's raw geometry/material data without building any Godot resources, then selectively
// build (load_geometry / load_material) only what you actually want in the final scene - see
// FbxScene::import(), which only includes nodes whose mesh had load_geometry() called.
//
// Only ever meaningfully constructed by FbxScene (via _init); a bare FbxMeshEntry.new() has no
// underlying mesh and every method is a safe no-op / empty-result.
class FbxMeshEntry : public RefCounted {
	GDCLASS(FbxMeshEntry, RefCounted)

	Ref<FbxScene> owner_scene; // Keeps the parsed ufbx_scene (and thus `mesh`) alive.
	ufbx_mesh *mesh = nullptr; // Borrowed; valid as long as owner_scene is held.
	int index = -1;

	Ref<ArrayMesh> loaded_mesh;
	Vector<int> surface_material_index; // Godot surface index -> mesh->materials[] index.
	Vector<Ref<StandardMaterial3D>> loaded_materials;

	bool skin_requested = false; // Was load_skin() ever called.
	bool skin_loaded = false; // Did the most recent rebuild actually have skin data available.

	void _apply_materials();
	// Actually builds loaded_mesh (ArrayMesh surfaces are immutable once created, so this is
	// re-run in full by both load_geometry() and load_skin() - whichever runs last picks up
	// the other's state, same order-independence load_geometry()/load_material() already have).
	void _rebuild_mesh();

protected:
	static void _bind_methods();

public:
	FbxMeshEntry() = default;
	~FbxMeshEntry();

	// Called once by FbxScene right after instantiate(); not exposed to GDScript.
	void _init(const Ref<FbxScene> &p_owner_scene, ufbx_mesh *p_mesh, int p_index);

	String get_name() const;
	int get_index() const;

	// Raw geometry/material dictionaries, same shape as FbxManager::get_mesh_data /
	// get_material_data - reading these never builds any Godot resource.
	Dictionary geometry() const;
	Dictionary material() const;

	// Raw normal/UV/skin-weight dictionaries, mirroring the shape of normal.py/uv.py/skin.py
	// from https://github.com/lucasposito/FbxData/tree/main/FbxData/lib. Also inspection-only.
	Dictionary normal() const;
	Dictionary uv() const;
	Dictionary skin() const;

	// Raw skeleton-hierarchy dictionary, mirroring the tree shape of MayaData's skeleton.py:
	// https://github.com/lucasposito/MayaData/blob/main/MayaData/data/skeleton.py. Anchors on
	// this mesh's own influencing bones (same universe as skin()). Inspection-only.
	Dictionary skeleton() const;

	// Builds this mesh's ArrayMesh (no materials attached yet - see load_material()).
	void load_geometry();
	// Builds this mesh's StandardMaterial3D(s) and attaches them to the ArrayMesh's surfaces
	// if load_geometry() has already run (otherwise applied automatically once it does).
	void load_material();
	// Writes real ARRAY_BONES/ARRAY_WEIGHTS onto the mesh's surfaces, resolving each skin
	// cluster's bone through owner_scene's built Skeleton3D (see FbxScene::load_skeleton(),
	// which must be called first - if it hasn't been, this is a no-op with a printerr). If the
	// mesh has no skin deformer at all, this is a silent no-op, callable unconditionally in a
	// loop over every mesh. Implies load_geometry() if it hasn't run yet (skin weights are
	// meaningless without vertices), and can be called before or after it in either order.
	void load_skin();

	bool is_geometry_loaded() const;
	bool is_material_loaded() const;
	bool is_skin_loaded() const { return skin_loaded; }

	Ref<ArrayMesh> get_loaded_mesh() const;
};
