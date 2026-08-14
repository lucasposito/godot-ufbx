#pragma once

// Internal (unregistered) conversion helpers shared by FbxManager, FbxScene, and
// FbxMeshEntry. Nothing here is exposed to GDScript.

#include <godot_cpp/classes/array_mesh.hpp>
#include <godot_cpp/classes/image_texture.hpp>
#include <godot_cpp/classes/skeleton3d.hpp>
#include <godot_cpp/classes/standard_material3d.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/transform3d.hpp>
#include <godot_cpp/templates/vector.hpp>

#include "ufbx.h"

using namespace godot;

namespace fbx_convert {

// Per-cluster bone resolution for build_mesh_geometry()'s optional skin pass: cluster_to_bone
// is index-aligned with deformer->clusters, giving each cluster's bone's index in the
// Skeleton3D that will end up skinning the mesh (-1 if that cluster's bone didn't make it in).
struct SkinBuildInput {
	ufbx_skin_deformer *deformer = nullptr;
	Vector<int> cluster_to_bone;
};

// ufbx already normalizes the scene to Godot's right-handed, Y-up axes via ufbx_load_opts
// (see FbxManager::import_scene / FbxManager::load_scene), so these are direct field copies.
Transform3D ufbx_transform_to_godot(const ufbx_transform &p_transform);
Transform3D ufbx_matrix_to_godot(const ufbx_matrix &p_matrix);

// Reads the elements that form a mesh (vertices, per-corner indices, polygon sizes, normals,
// UVs) straight from ufbx's own vertex-attribute model. Dictionary keys are: name, vertices,
// indices, faces, matrix, normals, normal_indices, uvs, uv_indices.
Dictionary read_mesh_elements(ufbx_mesh *p_mesh);

// Mirrors the material-extraction logic and dictionary keys of FbxData's material.py
// (https://github.com/lucasposito/FbxData/blob/main/FbxData/lib/material.py): "geometry" is
// the owning node's name, "materials" lists material names in order, "paths" gives each
// material's DiffuseColor/NormalMap texture path, and "face_id_map" gives, for each material
// index, which polygons use it.
Dictionary read_material_data(ufbx_node *p_node, ufbx_mesh *p_mesh);

// Mirrors FbxData's normal.py: "geometry" is the mesh's name, "normals" maps a control-point
// (logical vertex) index to its normal, and "faces" is left empty - normal.py declares it on
// NormalData but its get() never actually populates it, so neither do we. normal.py derives
// each control point's normal by summing every per-corner normal whose corner maps back to
// that control point (an FBX-SDK re-derivation of a possibly per-corner/split normal down to
// per-control-point, without averaging); we do the same sum, just walking ufbx's
// vertex_indices (corner -> control point) instead of manually searching an index array.
// A control point is omitted if its summed normal is zero-length, same as the reference.
Dictionary read_normal_data(ufbx_mesh *p_mesh);

// Mirrors FbxData's uv.py: "geometry" is the mesh's name, "vertices" is the flat list of unique
// UV pairs, and "indices" is one sub-list per face giving that face's corners' indices into
// "vertices", in order. Raw ufbx UV values (V not flipped) - unlike build_mesh_geometry's UVs,
// this is an inspection-only export meant to be comparable with the reference tool's output.
Dictionary read_uv_data(ufbx_mesh *p_mesh);

// Mirrors FbxData's skin.py: no "geometry" key (the reference returns a plain dict), just one
// entry per bone (skin cluster) that actually influences this mesh, mapping the bone's name to
// a control-point-count-long weight array (0.0 where the bone has no influence). Only the
// mesh's first skin deformer is read, same as skin.py's `GetDeformer(0, eSkin)`. Returns an
// empty Dictionary if the mesh has no skin deformer.
Dictionary read_skin_data(ufbx_mesh *p_mesh);

// Mirrors the tree shape and per-bone keys of MayaData's skeleton.py
// (https://github.com/lucasposito/MayaData/blob/main/MayaData/data/skeleton.py): top level is
// {"id": 0, "joints": [<names, breadth-first>], <root bone's hashed id>: <Leaf>, ...}. Each
// Leaf is {"id", "parent", "name", "matrix", "orient", "rotation", "radius", "rotateOrder",
// "side", "type"} plus one further nested Leaf per child bone, keyed by that child's hashed id
// - exactly like the reference's Tree/Leaf. Anchors on the mesh's own influencing bones (same
// universe as read_skin_data), walking up to the scene root from each to find where to start,
// same as skeleton.py's default from_root=True.
//
// Deliberate deviations from the reference, all noted where implemented in the .cpp:
// - "matrix" is a flat 16-float Maya-style row-major list (not a Transform3D): world-space for
//   the root bone found per anchor (matching skeleton.py's from_root special case for the very
//   first joint), parent-relative local space for every other bone (matching
//   MFnTransform.transformationMatrix() for the rest) - everything else derives from it exactly
//   as in Maya.
// - Tree/Leaf keys are hashed per-name with a 64-bit FNV-1a, not Python's
//   int(name.encode('utf-8').hex(), 16): that scheme is an arbitrary-precision integer that
//   cannot fit a 64-bit Dictionary key for any name over 8 UTF-8 bytes, so exact numeric parity
//   with the Python tool was never possible here regardless.
// - Only nodes actually referenced as a skin cluster's bone get a Leaf/tree level (not merely
//   nodes ufbx classifies via ufbx_node::bone - some exporters skin against plain transform
//   nodes with no FBX "Skeleton" NodeAttribute at all, leaving that field null); intermediate
//   non-bone transforms are skipped over (a bone nests under its nearest bone ancestor) rather
//   than each becoming its own level, unlike skeleton.py's per-DAG-path-segment hashing (an
//   artifact of how Maya builds path strings, not something meaningful to reproduce against an
//   FBX file).
// - "side"/"type" are Maya-only joint labeling attributes with no FBX equivalent; both default
//   to 0.0, matching skeleton.py's own DEFAULT_DATA fallback.
Dictionary read_skeleton_data(ufbx_mesh *p_mesh);

// Decodes an in-memory image buffer, dispatching to the matching Image loader by file
// extension. Returns a null Ref if the extension is unrecognized or decoding fails.
Ref<ImageTexture> load_image_from_buffer(const PackedByteArray &p_buffer, const String &p_extension);

// Loads a texture from a ufbx_texture, preferring its embedded content blob (common for
// "embed media" exports, eg. Mixamo) since the file paths ufbx reports are recorded at export
// time and are frequently absolute paths from a different machine than the one running the
// importer. Falls back to disk, trying the texture's filename next to the source FBX (the
// ".fbm" media-folder convention) before the recorded path verbatim.
Ref<ImageTexture> load_texture(ufbx_texture *p_texture, const String &p_fbx_dir);

// Builds a StandardMaterial3D from a material's base color and DiffuseColor/NormalMap textures.
Ref<StandardMaterial3D> build_material(ufbx_material *p_material, const String &p_fbx_dir);

// Triangulates every face and builds a flat (non-indexed) triangle soup, split into one
// ArrayMesh surface per material (or a single unmaterialed surface if the mesh has none), with
// no materials attached. Simplest correct approach - not vertex-cache optimal, good enough as a
// starting point. If p_out_surface_material_index is non-null, it is filled with one entry per
// Godot surface actually created (empty material buckets are skipped), giving that surface's
// index into p_mesh->materials[] - lets callers attach materials independently and/or later.
// If p_skin is non-null, ARRAY_BONES/ARRAY_WEIGHTS are also filled per corner (8-wide, with
// ARRAY_FLAG_USE_8_BONE_WEIGHTS set on the surface - plain 4-wide silently reads this data
// wrong, not just truncated): each corner's control point is looked up in
// p_skin->deformer->vertices[] (already weight-sorted descending by ufbx), the first 8 weights
// are resolved through p_skin->cluster_to_bone (dropping any that resolve to -1) and
// renormalized to sum to 1. 8 rather than Godot's default 4 because corrective-bone rigs (e.g.
// UE-style upperarm_fwd/bck/in/out/twist_01/twist_02 helper bones) routinely blend a single
// vertex across 5+ bones - capping at 4 silently drops whichever influences sort lowest,
// which for a vertex whose weight is split between a few dominant torso bones and several
// smaller corrective limb bones means the limb bones vanish entirely, leaving that vertex
// rigid to the torso while its neighbors correctly follow the limb.
Ref<ArrayMesh> build_mesh_geometry(ufbx_mesh *p_mesh, Vector<int> *p_out_surface_material_index, const SkinBuildInput *p_skin = nullptr);

// Legacy convenience used by FbxManager::import_scene: build_mesh_geometry() plus an immediate
// build_material()/surface_set_material() pass over every surface. Never includes skin data.
Ref<ArrayMesh> build_mesh(ufbx_mesh *p_mesh, const String &p_fbx_dir);

// Builds one real Skeleton3D bone (add_bone/set_bone_parent/set_bone_rest) for every node
// referenced as some mesh's skin cluster bone, scene-wide (not anchored to any one mesh, since
// this lives on FbxScene rather than a specific FbxMeshEntry) - deliberately not gated on
// ufbx_node::bone, since some exporters skin against plain transform nodes with no FBX
// "Skeleton" NodeAttribute at all, leaving that field null even though the node is
// unambiguously a joint. Mirrors read_skeleton_data's "skip non-bone transforms, nest under the
// nearest bone ancestor" rule, but - unlike that Dictionary version - properly composes the
// rest transform through any skipped intermediate nodes, since this pose has to be
// geometrically correct for real skinning rather than just informative.
void build_skeleton_bones(ufbx_scene *p_scene, Skeleton3D *p_skeleton);

// Skeleton3D::add_bone() outright rejects names containing ':' or '/' (common in namespaced
// rigs, eg. Mixamo's "mixamorig:Hips") rather than sanitizing them like Node::set_name() does.
// Both build_skeleton_bones() (when adding) and FbxMeshEntry::load_skin() (when resolving a
// skin cluster's bone back to its Skeleton3D index via FbxScene::find_bone_index()) must run a
// bone's name through this same function, or the two won't agree on what the bone is called.
String sanitize_bone_name(const String &p_name);

// Diagnostic counts used by FbxScene::load_skeleton() to explain why it found zero bones,
// without needing the caller to write any inspection code of their own.
struct SkinningSummary {
	int mesh_count = 0;
	int meshes_with_skin_deformers = 0;
	int total_clusters = 0;
	int clusters_with_bone_node = 0;
};
SkinningSummary summarize_skinning(ufbx_scene *p_scene);

} //namespace fbx_convert
