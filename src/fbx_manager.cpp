#include "fbx_manager.hpp"

#include "fbx_convert.hpp"
#include "fbx_mesh_entry.hpp"
#include "fbx_scene.hpp"

#include <godot_cpp/classes/mesh_instance3d.hpp>
#include <godot_cpp/classes/project_settings.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

using namespace godot;
using namespace fbx_convert;

Dictionary FbxManager::get_scene_info(const String &p_path) const {
	Dictionary result;

	String global_path = ProjectSettings::get_singleton()->globalize_path(p_path);

	ufbx_load_opts opts = {};
	ufbx_error error;
	ufbx_scene *scene = ufbx_load_file(global_path.utf8().get_data(), &opts, &error);

	if (!scene) {
		result["success"] = false;
		result["error"] = String::utf8(error.description.data, (int)error.description.length);
		return result;
	}

	result["success"] = true;
	result["node_count"] = (int64_t)scene->nodes.count;
	result["mesh_count"] = (int64_t)scene->meshes.count;
	result["material_count"] = (int64_t)scene->materials.count;
	result["animation_stack_count"] = (int64_t)scene->anim_stacks.count;

	ufbx_free_scene(scene);
	return result;
}

Node3D *FbxManager::import_scene(const String &p_path) const {
	ufbx_load_opts opts = {};
	// Normalize to Godot's coordinate system/units. Baking the conversion into the
	// root node's local_transform keeps every other node's local_transform a plain copy.
	opts.target_axes = ufbx_axes_right_handed_y_up;
	opts.target_unit_meters = 1.0f;
	opts.space_conversion = UFBX_SPACE_CONVERSION_TRANSFORM_ROOT;

	String global_path = ProjectSettings::get_singleton()->globalize_path(p_path);

	ufbx_error error;
	ufbx_scene *scene = ufbx_load_file(global_path.utf8().get_data(), &opts, &error);

	if (!scene) {
		UtilityFunctions::printerr("FbxManager: failed to load '", p_path, "': ",
				String::utf8(error.description.data, (int)error.description.length));
		return nullptr;
	}

	Node3D *root = _build_node(scene->root_node, global_path.get_base_dir());

	ufbx_free_scene(scene);
	return root;
}

Dictionary FbxManager::get_mesh_data(const String &p_path, int p_mesh_index) const {
	String global_path = ProjectSettings::get_singleton()->globalize_path(p_path);

	ufbx_load_opts opts = {};
	ufbx_error error;
	ufbx_scene *scene = ufbx_load_file(global_path.utf8().get_data(), &opts, &error);

	if (!scene) {
		UtilityFunctions::printerr("FbxManager: failed to load '", p_path, "': ",
				String::utf8(error.description.data, (int)error.description.length));
		return Dictionary();
	}

	if (p_mesh_index < 0 || (size_t)p_mesh_index >= scene->meshes.count) {
		UtilityFunctions::printerr("FbxManager: mesh index ", p_mesh_index, " out of range for '", p_path, "'");
		ufbx_free_scene(scene);
		return Dictionary();
	}

	Dictionary data = read_mesh_elements(scene->meshes.data[p_mesh_index]);

	ufbx_free_scene(scene);
	return data;
}

Dictionary FbxManager::get_material_data(const String &p_path, int p_mesh_index) const {
	String global_path = ProjectSettings::get_singleton()->globalize_path(p_path);

	ufbx_load_opts opts = {};
	ufbx_error error;
	ufbx_scene *scene = ufbx_load_file(global_path.utf8().get_data(), &opts, &error);

	if (!scene) {
		UtilityFunctions::printerr("FbxManager: failed to load '", p_path, "': ",
				String::utf8(error.description.data, (int)error.description.length));
		return Dictionary();
	}

	if (p_mesh_index < 0 || (size_t)p_mesh_index >= scene->meshes.count) {
		UtilityFunctions::printerr("FbxManager: mesh index ", p_mesh_index, " out of range for '", p_path, "'");
		ufbx_free_scene(scene);
		return Dictionary();
	}

	ufbx_mesh *mesh = scene->meshes.data[p_mesh_index];
	ufbx_node *node = mesh->instances.count > 0 ? mesh->instances.data[0] : nullptr;
	Dictionary data = read_material_data(node, mesh);

	ufbx_free_scene(scene);
	return data;
}

Ref<FbxScene> FbxManager::load_scene(const String &p_path) const {
	ufbx_load_opts opts = {};
	// Normalize to Godot's coordinate system/units. Baking the conversion into the
	// root node's local_transform keeps every other node's local_transform a plain copy.
	opts.target_axes = ufbx_axes_right_handed_y_up;
	opts.target_unit_meters = 1.0f;
	opts.space_conversion = UFBX_SPACE_CONVERSION_TRANSFORM_ROOT;

	String global_path = ProjectSettings::get_singleton()->globalize_path(p_path);

	ufbx_error error;
	ufbx_scene *scene = ufbx_load_file(global_path.utf8().get_data(), &opts, &error);

	if (!scene) {
		UtilityFunctions::printerr("FbxManager: failed to load '", p_path, "': ",
				String::utf8(error.description.data, (int)error.description.length));
		return Ref<FbxScene>();
	}

	Ref<FbxScene> fbx_scene;
	fbx_scene.instantiate();
	fbx_scene->_init(global_path.get_base_dir(), scene);
	return fbx_scene;
}

Node3D *FbxManager::_build_node(ufbx_node *p_node, const String &p_fbx_dir) const {
	Node3D *node = nullptr;

	if (p_node->mesh) {
		MeshInstance3D *mesh_instance = memnew(MeshInstance3D);
		mesh_instance->set_mesh(build_mesh(p_node->mesh, p_fbx_dir));
		node = mesh_instance;
	} else {
		node = memnew(Node3D);
	}

	String name = String::utf8(p_node->name.data, (int)p_node->name.length);
	if (name.is_empty()) {
		// ufbx nodes (notably the scene root) are frequently unnamed; Node::set_name()
		// rejects an empty string outright, so fall back to a generic name.
		name = p_node->is_root ? "RootNode" : "Node";
	}
	node->set_name(name);
	node->set_transform(ufbx_transform_to_godot(p_node->local_transform));

	for (size_t i = 0; i < p_node->children.count; i++) {
		Node3D *child = _build_node(p_node->children.data[i], p_fbx_dir);
		node->add_child(child);
	}

	return node;
}

void FbxManager::_bind_methods() {
	ClassDB::bind_method(D_METHOD("get_scene_info", "path"), &FbxManager::get_scene_info);
	ClassDB::bind_method(D_METHOD("import_scene", "path"), &FbxManager::import_scene);
	ClassDB::bind_method(D_METHOD("get_mesh_data", "path", "mesh_index"), &FbxManager::get_mesh_data);
	ClassDB::bind_method(D_METHOD("get_material_data", "path", "mesh_index"), &FbxManager::get_material_data);
	ClassDB::bind_method(D_METHOD("load_scene", "path"), &FbxManager::load_scene);
}
