#include "fbx_scene.hpp"

#include "fbx_convert.hpp"

#include <godot_cpp/classes/mesh_instance3d.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/node_path.hpp>
#include <godot_cpp/variant/quaternion.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include <algorithm>
#include <cmath>

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

namespace {

void _append_bone_tree(Skeleton3D *p_skeleton, const Vector<Vector<int>> &p_children, int p_bone, int p_depth, String &p_out) {
	for (int d = 0; d < p_depth; d++) {
		p_out += "  ";
	}
	p_out += p_skeleton->get_bone_name(p_bone);
	p_out += "\n";
	const Vector<int> &kids = p_children[p_bone];
	for (int i = 0; i < kids.size(); i++) {
		_append_bone_tree(p_skeleton, p_children, kids[i], p_depth + 1, p_out);
	}
}

} //namespace

String FbxScene::debug_skeleton_hierarchy() const {
	if (!built_skeleton) {
		return String();
	}

	int count = built_skeleton->get_bone_count();
	Vector<Vector<int>> children;
	children.resize(count);
	Vector<int> roots;
	for (int i = 0; i < count; i++) {
		int parent = built_skeleton->get_bone_parent(i);
		if (parent < 0) {
			roots.push_back(i);
		} else {
			children.write[parent].push_back(i);
		}
	}

	String out;
	for (int i = 0; i < roots.size(); i++) {
		_append_bone_tree(built_skeleton, children, roots[i], 0, out);
	}
	return out;
}

int FbxScene::get_animation_count() const {
	return scene ? (int)scene->anim_stacks.count : 0;
}

PackedStringArray FbxScene::get_animation_names() const {
	PackedStringArray names;
	if (!scene) {
		return names;
	}
	names.resize((int)scene->anim_stacks.count);
	for (size_t i = 0; i < scene->anim_stacks.count; i++) {
		ufbx_anim_stack *stack = scene->anim_stacks.data[i];
		names[(int)i] = String::utf8(stack->name.data, (int)stack->name.length);
	}
	return names;
}

Ref<Animation> FbxScene::load_animation(int p_index, double p_fps, Skeleton3D *p_target_skeleton) const {
	Ref<Animation> anim;
	anim.instantiate();

	if (!scene || p_index < 0 || (size_t)p_index >= scene->anim_stacks.count) {
		UtilityFunctions::printerr("FbxScene: animation index ", p_index, " out of range (",
				scene ? (int)scene->anim_stacks.count : 0, " animation stacks)");
		return anim;
	}

	ufbx_anim_stack *stack = scene->anim_stacks.data[p_index];
	ufbx_anim *stack_anim = stack->anim;

	double fps = p_fps > 0.0 ? p_fps : scene->settings.frames_per_second;
	if (fps <= 0.0) {
		fps = 30.0;
	}

	double length = stack->time_end - stack->time_begin;
	if (length < 0.0) {
		length = 0.0;
	}
	anim->set_length(length > 0.0 ? length : (1.0 / fps));
	anim->set_step(1.0 / fps);

	// +1 so the last sample lands exactly on `length` (a plain length*fps count would stop just
	// short of the final frame, e.g. a 1s/30fps clip needs samples at frame 30 too, not just 0..29).
	int frame_count = length > 0.0 ? (int)std::ceil(length * fps) + 1 : 1;

	// "Is this node a real bone?" - must agree with the Skeleton3D the animation is played
	// against, so membership in p_target_skeleton (by sanitized name) is the authority whenever
	// one was given. ufbx's `bone` flag alone disagrees with build_skeleton_bones(), which also
	// takes any node referenced as a skin cluster's bone_node: some exporters skin against plain
	// transform nodes that never get an FBX "Skeleton" attribute, so those joints end up in the
	// Skeleton3D with a rest pose but would get no track here - frozen at rest while their
	// children's tracks were baked assuming they moved. Only with no target skeleton (an
	// animation-only export, e.g. Mixamo "without skin", which has no mesh/skin deformer to
	// derive bones from) do we fall back to the `bone` attribute alone.
	auto is_real_bone = [&](const ufbx_node *p_node) {
		if (p_target_skeleton == nullptr) {
			return p_node->bone != nullptr;
		}
		String name = sanitize_bone_name(String::utf8(p_node->name.data, (int)p_node->name.length));
		return p_target_skeleton->find_bone(name) >= 0;
	};

	for (size_t i = 0; i < scene->nodes.count; i++) {
		ufbx_node *node = scene->nodes.data[i];
		if (!is_real_bone(node)) {
			continue;
		}

		// Skeleton3D interprets a bone's pose track as relative to its *parent bone*, skipping
		// any non-bone nodes in between (matching how build_skeleton_bones() sets up
		// set_bone_rest()) - most bones have a real skeleton bone as their immediate parent so
		// this is empty. The walk stops at the nearest ancestor that is itself a real bone, using
		// the exact same test as the track-creation gate above - the two must agree, or a node can
		// be both skipped (no track of its own, so frozen at rest) and treated as a stopping point
		// (so its child's track is baked as if it moved), and the mismatch compounds down the
		// chain. Ancestors that aren't real bones - e.g. a "root" control node above the topmost
		// skinned bone that carries no skin weight and so never made it into
		// build_skeleton_bones()'s Skeleton3D - are composed through instead, matching how the
		// child's rest pose is expressed relative to the skeleton root.
		//
		// Deliberately never climbs past scene->root_node - that node carries ufbx's
		// UFBX_SPACE_CONVERSION_TRANSFORM_ROOT axis/unit conversion (see FbxManager::load_scene),
		// which build_skeleton_bones() also excludes from bone rest for the same reason
		// (fbx_convert.cpp's build_skeleton_bones() comment has the full explanation): that
		// conversion is applied exactly once, via the Node3D transform FbxScene::import() sets on
		// its returned root. Composing it a second time in here used to be invisible for a static
		// pose (skin deformation is pose * rest^-1, and a shared scale error in both cancels in
		// that ratio at bind pose) but broke skinning the instant a bone rotated away from rest,
		// since the mismatch no longer cancels once pose and rest diverge.
		Vector<ufbx_node *> pending_chain;
		for (ufbx_node *ancestor = node->parent; ancestor && ancestor != scene->root_node; ancestor = ancestor->parent) {
			if (is_real_bone(ancestor)) {
				break;
			}
			pending_chain.push_back(ancestor);
		}
		// Reverse to root-first order so the multiply below composes outermost-to-innermost.
		for (int lo = 0, hi = pending_chain.size() - 1; lo < hi; lo++, hi--) {
			ufbx_node *tmp = pending_chain[lo];
			pending_chain.write[lo] = pending_chain[hi];
			pending_chain.write[hi] = tmp;
		}

		String bone_name = sanitize_bone_name(String::utf8(node->name.data, (int)node->name.length));
		NodePath track_path(String("Skeleton3D:") + bone_name);

		int pos_track = anim->add_track(Animation::TYPE_POSITION_3D);
		anim->track_set_path(pos_track, track_path);
		int rot_track = anim->add_track(Animation::TYPE_ROTATION_3D);
		anim->track_set_path(rot_track, track_path);
		int scale_track = anim->add_track(Animation::TYPE_SCALE_3D);
		anim->track_set_path(scale_track, track_path);

		for (int f = 0; f < frame_count; f++) {
			double key_time = length > 0.0 ? std::min((double)f / fps, length) : 0.0;
			double sample_time = stack->time_begin + key_time;

			Transform3D pending;
			for (int c = 0; c < pending_chain.size(); c++) {
				ufbx_transform ancestor_xf = ufbx_evaluate_transform(stack_anim, pending_chain[c], sample_time);
				pending = pending * ufbx_transform_to_godot(ancestor_xf);
			}
			ufbx_transform xf = ufbx_evaluate_transform(stack_anim, node, sample_time);
			Transform3D effective = pending * ufbx_transform_to_godot(xf);

			anim->position_track_insert_key(pos_track, key_time, effective.origin);
			anim->rotation_track_insert_key(rot_track, key_time, effective.basis.get_rotation_quaternion());
			anim->scale_track_insert_key(scale_track, key_time, effective.basis.get_scale());
		}
	}

	return anim;
}

void FbxScene::_bind_methods() {
	ClassDB::bind_method(D_METHOD("get_mesh_count"), &FbxScene::get_mesh_count);
	ClassDB::bind_method(D_METHOD("get_mesh_names"), &FbxScene::get_mesh_names);
	ClassDB::bind_method(D_METHOD("get_mesh", "index"), &FbxScene::get_mesh);
	ClassDB::bind_method(D_METHOD("get_meshes"), &FbxScene::get_meshes);
	ClassDB::bind_method(D_METHOD("load_skeleton"), &FbxScene::load_skeleton);
	ClassDB::bind_method(D_METHOD("has_skeleton"), &FbxScene::has_skeleton);
	ClassDB::bind_method(D_METHOD("get_skeleton"), &FbxScene::get_skeleton);
	ClassDB::bind_method(D_METHOD("debug_skeleton_hierarchy"), &FbxScene::debug_skeleton_hierarchy);
	ClassDB::bind_method(D_METHOD("import"), &FbxScene::import);
	ClassDB::bind_method(D_METHOD("get_animation_count"), &FbxScene::get_animation_count);
	ClassDB::bind_method(D_METHOD("get_animation_names"), &FbxScene::get_animation_names);
	ClassDB::bind_method(D_METHOD("load_animation", "index", "fps", "target_skeleton"), &FbxScene::load_animation, DEFVAL(0.0), DEFVAL(Variant()));
}
