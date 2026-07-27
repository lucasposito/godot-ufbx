#include "fbx_scene.hpp"

#include "fbx_convert.hpp"

#include <godot_cpp/classes/mesh_instance3d.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

using namespace godot;
using namespace fbx_convert;

FbxScene::~FbxScene() {
	if (scene) {
		ufbx_free_scene(scene);
	}
	if (built_skeleton && !built_skeleton->get_parent()) {
		memdelete(built_skeleton);
	}
}

void FbxScene::_init(const String &p_fbx_dir, ufbx_scene *p_scene) {
	fbx_dir = p_fbx_dir;
	scene = p_scene;

	mesh_entries.clear();
	for (size_t i = 0; i < scene->meshes.count; i++) {
		Ref<FbxMeshEntry> entry;
		entry.instantiate();
		entry->_init(Ref<FbxScene>(this), scene->meshes.data[i], (int)i);
		mesh_entries.push_back(entry);
	}
}

int FbxScene::get_mesh_count() const {
	return mesh_entries.size();
}

PackedStringArray FbxScene::get_mesh_names() const {
	PackedStringArray names;
	names.resize(mesh_entries.size());
	for (int i = 0; i < mesh_entries.size(); i++) {
		names[i] = mesh_entries[i]->get_name();
	}
	return names;
}

Ref<FbxMeshEntry> FbxScene::get_mesh(int p_index) const {
	if (p_index < 0 || p_index >= mesh_entries.size()) {
		UtilityFunctions::printerr("FbxScene: mesh index ", p_index, " out of range (", mesh_entries.size(), " meshes)");
		return Ref<FbxMeshEntry>();
	}
	return mesh_entries[p_index];
}

Dictionary FbxScene::get_meshes() const {
	Dictionary meshes;
	for (int i = 0; i < mesh_entries.size(); i++) {
		meshes[i] = mesh_entries[i];
	}
	return meshes;
}

void FbxScene::load_skeleton() {
	if (built_skeleton) {
		if (!built_skeleton->get_parent()) {
			memdelete(built_skeleton);
		}
		built_skeleton = nullptr;
	}

	if (!scene) {
		return;
	}

	Skeleton3D *skeleton = memnew(Skeleton3D);
	build_skeleton_bones(scene, skeleton);

	if (skeleton->get_bone_count() == 0) {
		memdelete(skeleton);
		SkinningSummary summary = summarize_skinning(scene);
		UtilityFunctions::print("FbxScene: load_skeleton() found no bones (", summary.mesh_count, " meshes scanned, ",
				summary.meshes_with_skin_deformers, " with a skin deformer, ", summary.total_clusters, " clusters, ",
				summary.clusters_with_bone_node, " with a resolvable bone_node)");
		return;
	}

	skeleton->set_name("Skeleton3D");
	// Force every bone's pose to match the rest we just set - shouldn't be necessary (pose
	// defaults to an identity delta over rest), but cheap insurance against any stale/default
	// pose state left over from building bones programmatically outside the usual import path.
	skeleton->reset_bone_poses();
	built_skeleton = skeleton;
}

Node3D *FbxScene::import() const {
	if (!scene) {
		return memnew(Node3D);
	}

	ufbx_node *root_node = scene->root_node;
	String name = String::utf8(root_node->name.data, (int)root_node->name.length);
	if (name.is_empty()) {
		name = "RootNode";
	}

	Node3D *root = memnew(Node3D);
	root->set_name(name);
	root->set_transform(ufbx_transform_to_godot(root_node->local_transform));

	if (built_skeleton) {
		// Parented before the recursive walk below so get_path_to() resolves correctly when
		// wiring up skinned MeshInstance3D nodes.
		root->add_child(built_skeleton);
	}

	for (size_t i = 0; i < root_node->children.count; i++) {
		_build_node(root_node->children.data[i], root, root, Transform3D(), false);
	}

	return root;
}

void FbxScene::_build_node(ufbx_node *p_node, Node3D *p_parent, Node3D *p_root, const Transform3D &p_pending, bool p_pending_valid) const {
	bool has_mesh = p_node->mesh != nullptr;
	bool is_bone = built_skeleton && p_node->bone != nullptr;

	Ref<FbxMeshEntry> entry;
	if (has_mesh) {
		int mesh_index = (int)p_node->mesh->typed_id;
		if (mesh_index >= 0 && mesh_index < mesh_entries.size()) {
			entry = mesh_entries[mesh_index];
		}
	}

	// Bone nodes are represented inside built_skeleton instead of as plain tree nodes; an
	// unloaded mesh is omitted the same way a mesh node with no loaded geometry always was.
	bool keep = !is_bone && (!has_mesh || (entry.is_valid() && entry->is_geometry_loaded()));
	bool is_skeletal_mesh = has_mesh && entry.is_valid() && entry->is_skin_loaded();

	Transform3D local = ufbx_transform_to_godot(p_node->local_transform);
	Transform3D effective = p_pending_valid ? (p_pending * local) : local;

	if (keep) {
		Node3D *node;
		MeshInstance3D *mesh_instance = nullptr;
		if (has_mesh) {
			mesh_instance = memnew(MeshInstance3D);
			mesh_instance->set_mesh(entry->get_loaded_mesh());
			node = mesh_instance;
		} else {
			node = memnew(Node3D);
		}

		String name = String::utf8(p_node->name.data, (int)p_node->name.length);
		if (name.is_empty()) {
			name = "Node"; // Root is handled separately in import(); never reached here.
		}
		node->set_name(name);

		if (is_skeletal_mesh) {
			// Skeletal mesh: its vertices are already fully positioned by the skin binding
			// relative to the skeleton, so it's attached flat under the scene root with an
			// identity transform instead of inheriting the FBX hierarchy's (possibly
			// bone-relative) placement - carrying that transform forward too would double up
			// whatever part of the pose the skin weights already bake in.
			node->set_transform(Transform3D());
			p_root->add_child(node);
		} else {
			// Static mesh (or a plain transform node): keeps its normal place in the
			// hierarchy, untouched by any skeleton in the scene.
			node->set_transform(effective);
			p_parent->add_child(node);
		}

		// get_path_to() needs both nodes already parented into the same tree, so this can only
		// happen after add_child() above (built_skeleton was parented under root earlier still).
		if (is_skeletal_mesh && built_skeleton) {
			mesh_instance->set_skeleton_path(mesh_instance->get_path_to(built_skeleton));
		}

		for (size_t i = 0; i < p_node->children.count; i++) {
			if (is_skeletal_mesh) {
				// Preserve where children would have spatially landed had this node kept its
				// natural transform, even though the node itself was flattened to identity.
				_build_node(p_node->children.data[i], node, p_root, effective, true);
			} else {
				_build_node(p_node->children.data[i], node, p_root, Transform3D(), false);
			}
		}
	} else {
		// Skipped node (bone, or mesh present but never load_geometry()'d): don't create a
		// placeholder, but still visit its children, composing this node's transform forward
		// so any loaded descendant keeps its correct world position under the nearest kept
		// ancestor.
		for (size_t i = 0; i < p_node->children.count; i++) {
			_build_node(p_node->children.data[i], p_parent, p_root, effective, true);
		}
	}
}

void FbxScene::_bind_methods() {
	ClassDB::bind_method(D_METHOD("get_mesh_count"), &FbxScene::get_mesh_count);
	ClassDB::bind_method(D_METHOD("get_mesh_names"), &FbxScene::get_mesh_names);
	ClassDB::bind_method(D_METHOD("get_mesh", "index"), &FbxScene::get_mesh);
	ClassDB::bind_method(D_METHOD("get_meshes"), &FbxScene::get_meshes);
	ClassDB::bind_method(D_METHOD("load_skeleton"), &FbxScene::load_skeleton);
	ClassDB::bind_method(D_METHOD("has_skeleton"), &FbxScene::has_skeleton);
	ClassDB::bind_method(D_METHOD("get_skeleton"), &FbxScene::get_skeleton);
	ClassDB::bind_method(D_METHOD("import"), &FbxScene::import);
}
