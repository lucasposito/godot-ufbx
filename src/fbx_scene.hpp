#pragma once

#include <godot_cpp/classes/node3d.hpp>
#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/classes/skeleton3d.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/packed_string_array.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/transform3d.hpp>
#include <godot_cpp/templates/vector.hpp>

#include "fbx_mesh_entry.hpp"
#include "ufbx.h"

using namespace godot;

// A parsed FBX/OBJ scene (see FbxManager::load_scene). Owns the underlying ufbx_scene and one
// FbxMeshEntry per mesh, none of which have any Godot resources built yet. Inspect/selectively
// build meshes via get_mesh()/get_meshes(), then call import() to assemble a Node3D tree from
// whatever was actually loaded - nodes whose mesh never had load_geometry() called are omitted
// (their loaded descendants, if any, are reparented onto the nearest kept ancestor instead of
// being dropped too).
//
// Only ever meaningfully constructed by FbxManager::load_scene (via _init); a bare
// FbxScene.new() owns no scene and every method is a safe no-op / empty-result.
class FbxScene : public RefCounted {
	GDCLASS(FbxScene, RefCounted)

	ufbx_scene *scene = nullptr; // Owned; freed in the destructor.
	String fbx_dir;
	Vector<Ref<FbxMeshEntry>> mesh_entries; // Index-aligned with scene->meshes.

	// Built by load_skeleton(); owned by this object until import() parents it into the
	// returned tree, at which point the tree owns it. If never parented, freed in ~FbxScene().
	Skeleton3D *built_skeleton = nullptr;

	void _build_node(ufbx_node *p_node, Node3D *p_parent, Node3D *p_root, const Transform3D &p_pending, bool p_pending_valid) const;

protected:
	static void _bind_methods();

public:
	FbxScene() = default;
	~FbxScene();

	// Called once by FbxManager::load_scene right after instantiate(); not exposed to
	// GDScript. Takes ownership of p_scene (freed by this object's destructor).
	void _init(const String &p_fbx_dir, ufbx_scene *p_scene);

	const String &get_fbx_dir() const { return fbx_dir; }

	int get_mesh_count() const;
	PackedStringArray get_mesh_names() const;
	Ref<FbxMeshEntry> get_mesh(int p_index) const;
	Dictionary get_meshes() const;

	// Builds a single Skeleton3D covering every bone anywhere in the scene (not anchored to
	// any particular mesh). Safe to call again - discards and rebuilds. If the scene has no
	// bones at all, leaves has_skeleton() false rather than building an empty Skeleton3D.
	void load_skeleton();
	bool has_skeleton() const { return built_skeleton != nullptr; }
	Skeleton3D *get_skeleton() const { return built_skeleton; }
	// Not exposed to GDScript - internal plumbing for FbxMeshEntry::load_skin().
	int find_bone_index(const String &p_name) const { return built_skeleton ? built_skeleton->find_bone(p_name) : -1; }

	// Builds the Node3D tree from whichever meshes have had load_geometry() called. If
	// load_skeleton() was called first, the built Skeleton3D is parented under the returned
	// root (transferring ownership to it - calling import() again afterwards will fail to
	// reparent the same Skeleton3D, same as any Node can only belong to one tree at a time),
	// and bone nodes are omitted from the plain Node3D tree (they live inside the skeleton
	// instead). Each mesh becomes one of two kinds of MeshInstance3D:
	// - "Static mesh": no skin loaded (FbxMeshEntry::is_skin_loaded() false, whether because
	//   the mesh was never skinned or load_skin() couldn't resolve a skeleton). Placed at its
	//   normal spot in the hierarchy, untouched by any skeleton in the scene.
	// - "Skeletal mesh": is_skin_loaded() true. Its vertex positions are already fully defined
	//   by the skin binding relative to the skeleton, so instead of inheriting its (possibly
	//   bone-relative) FBX transform, it's attached directly under the returned root with an
	//   identity transform and MeshInstance3D::skeleton_path wired to the built Skeleton3D.
	// Ownership of the returned node is transferred to the caller.
	Node3D *import() const;
};
