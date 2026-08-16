#pragma once

#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/packed_int32_array.hpp>
#include <godot_cpp/variant/packed_string_array.hpp>
#include <godot_cpp/variant/packed_vector3_array.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/transform3d.hpp>
#include <godot_cpp/templates/vector.hpp>

using namespace godot;

// Builds an in-memory FBX scene from the same Dictionary shapes FbxMeshEntry's read-side getters
// return (geometry()/uv()/normal()/material()/skin(), FbxMeshEntry::skeleton()), then serializes
// it to a fresh ASCII FBX file via export_scene(). Only ever meaningfully constructed by
// FbxManager::new_scene() (a bare FbxSceneWriter.new() is just as usable - there's no parsed
// state to inherit, unlike FbxScene/FbxMeshEntry).
//
// set_mesh() is the only call required per mesh - export_scene() fills in sane defaults for
// anything the caller skipped: no UV channel, a single plain "DefaultMaterial", and flat-shaded
// normals computed from face winding. The scene holds at most one skeleton hierarchy
// (set_skeleton() replaces it wholesale, same "safe to call again" precedent as
// FbxScene::load_skeleton()), shared by every mesh that calls set_skin().
class FbxSceneWriter : public RefCounted {
	GDCLASS(FbxSceneWriter, RefCounted)

	struct WriterMesh {
		String name;
		PackedVector3Array vertices; // Control points.
		PackedInt32Array indices; // Per-corner -> control-point index.
		PackedInt32Array faces; // Polygon corner counts.
		Transform3D matrix; // World transform (identity if set_mesh's "matrix" key was absent).

		bool has_uv = false;
		Array uv_vertices; // [u, v] pairs, raw FBX convention (not V-flipped).
		Array uv_indices; // One sub-array per polygon, corner -> uv_vertices index.

		bool has_normal_override = false; // set_normal() was called for this mesh.
		Dictionary normals; // control-point index (int) -> Vector3, explicit or computed default.

		PackedStringArray materials; // Defaults to a single "DefaultMaterial".
		Dictionary material_paths; // material name -> {"DiffuseColor": path, "NormalMap": path}.
		Dictionary face_id_map; // material index (int) -> Array of polygon indices.

		Dictionary skin; // bone name -> per-control-point weight array. Empty if unskinned.
	};

	struct WriterBone {
		String name;
		int parent_index = -1; // Index into `bones`; -1 attaches directly under the scene root.
		Transform3D local; // Parent-relative (world, for a root bone - same as set_skeleton's input).
		Transform3D world; // Composed while flattening the Leaf tree in set_skeleton().
	};

	Vector<WriterMesh> meshes;
	Vector<WriterBone> bones; // Empty until set_skeleton() is called; single hierarchy only.

	// Recursively walks one Leaf (and its nested Leafs) from set_skeleton()'s Dictionary shape,
	// appending a WriterBone per Leaf. p_parent_world is identity for a top-level Leaf (whose
	// "matrix" is already world-space per the documented contract).
	void _collect_bone_leaf(const Dictionary &p_leaf, int p_parent_index, const Transform3D &p_parent_world);

protected:
	static void _bind_methods();

public:
	FbxSceneWriter() = default;

	// Creates a new mesh from the same shape FbxMeshEntry::geometry() returns: "name" (String),
	// "vertices" (PackedVector3Array), "indices" (PackedInt32Array, per-corner -> control-point),
	// "faces" (PackedInt32Array, polygon corner counts), and optionally "matrix" (Transform3D,
	// world - identity if absent). "normals"/"uvs" keys, if present, are ignored - feed those via
	// set_normal()/set_uv() instead. Returns the new mesh's index (for the other set_* calls) or
	// -1 (with a printerr) if vertices/indices/faces are missing, empty, or inconsistent (indices
	// count must equal the sum of faces).
	int set_mesh(Dictionary p_geometry);

	// Sets mesh p_mesh_index's UV channel from the same shape FbxMeshEntry::uv() returns:
	// "vertices" (Array of [u, v] pairs, raw FBX convention - not V-flipped) and "indices" (Array
	// of one sub-array per polygon, each holding that polygon's corners' indices into
	// "vertices"). Never calling this leaves the mesh with no UV channel at all.
	void set_uv(int p_mesh_index, Dictionary p_uv);

	// Sets mesh p_mesh_index's normals from the same shape FbxMeshEntry::normal() returns:
	// "normals" (Dictionary: control-point index -> Vector3). Each vector is normalized at
	// export time regardless of the input's length (unlike the read-side Dictionary, which is
	// inspection-only, this becomes a real rendered value). Never calling this makes
	// export_scene() compute flat-shaded normals from face winding instead.
	void set_normal(int p_mesh_index, Dictionary p_normal);

	// Sets mesh p_mesh_index's materials from the same shape FbxMeshEntry::material() returns:
	// "materials" (Array of names, in order), "paths" (Dictionary: material name ->
	// {"DiffuseColor": path, "NormalMap": path}, both optional), "face_id_map" (Dictionary:
	// material index -> Array of polygon indices using it). Replaces the single default material
	// set_mesh() seeded. Texture paths are only followed at export time to embed a
	// FileName/RelativeFilename reference - the files themselves are not copied.
	void set_material(int p_mesh_index, Dictionary p_material);

	// Replaces the scene's single skeleton hierarchy from the same Tree/Leaf shape
	// FbxMeshEntry::skeleton() returns: {"id": 0, "joints": [names...], <hashed_id>: Leaf, ...}
	// where each Leaf is {"id", "parent", "name", "matrix" (16-float Maya row-major flat array -
	// world-space for a top-level Leaf, parent-relative local space for every nested Leaf), plus
	// further nested Leafs for children}. Only "matrix"/"name"/nesting are used - "rotation"/
	// "orient"/"rotateOrder"/"radius"/"side"/"type" are accepted but ignored, per the read side's
	// own note that "matrix" is authoritative. Safe to call again - discards and rebuilds.
	void set_skeleton(Dictionary p_skeleton);

	// Binds mesh p_mesh_index to the scene's skeleton from the same shape FbxMeshEntry::skin()
	// returns: bone name -> Array/PackedFloat32Array of per-control-point weights (length must
	// equal that mesh's vertex count). Requires set_skeleton() to have been called first - no-op
	// with a printerr otherwise, mirroring FbxMeshEntry::load_skin()'s existing precedent. A bone
	// name that doesn't resolve against the current skeleton is skipped with its own printerr
	// rather than failing the whole call.
	void set_skin(int p_mesh_index, Dictionary p_skin);

	// Serializes everything accumulated so far into a fresh FBX 7.4 file at p_path (overwriting it
	// if it exists). p_binary selects binary FBX (the default - what Maya/3ds Max/Unity/Unreal/
	// Blender all expect) vs. human-readable ASCII FBX (still valid, handy for debugging/diffing,
	// but not every DCC tool's importer accepts it - Maya in particular is inconsistent about
	// ASCII support across versions). Purely a function of current state - safe to call again
	// after adding more meshes to re-write the file from scratch. Returns false (with a printerr)
	// if p_path can't be opened for writing.
	bool export_scene(String p_path, bool p_binary = true) const;
};
