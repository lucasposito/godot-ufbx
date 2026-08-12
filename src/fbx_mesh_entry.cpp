#include "fbx_mesh_entry.hpp"

#include "fbx_convert.hpp"
#include "fbx_scene.hpp"

#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

using namespace godot;
using namespace fbx_convert;

FbxMeshEntry::~FbxMeshEntry() {
}

void FbxMeshEntry::_init(const Ref<FbxScene> &p_owner_scene, ufbx_mesh *p_mesh, int p_index) {
	owner_scene = p_owner_scene;
	mesh = p_mesh;
	index = p_index;
}

String FbxMeshEntry::get_name() const {
	if (!mesh) {
		return String();
	}
	return String::utf8(mesh->name.data, (int)mesh->name.length);
}

int FbxMeshEntry::get_index() const {
	return index;
}

Dictionary FbxMeshEntry::geometry() const {
	if (!mesh) {
		return Dictionary();
	}
	return read_mesh_elements(mesh);
}

Dictionary FbxMeshEntry::material() const {
	if (!mesh) {
		return Dictionary();
	}
	ufbx_node *node = mesh->instances.count > 0 ? mesh->instances.data[0] : nullptr;
	return read_material_data(node, mesh);
}

Dictionary FbxMeshEntry::normal() const {
	if (!mesh) {
		return Dictionary();
	}
	return read_normal_data(mesh);
}

Dictionary FbxMeshEntry::uv() const {
	if (!mesh) {
		return Dictionary();
	}
	return read_uv_data(mesh);
}

Dictionary FbxMeshEntry::skin() const {
	if (!mesh) {
		return Dictionary();
	}
	return read_skin_data(mesh);
}

Dictionary FbxMeshEntry::skeleton() const {
	if (!mesh) {
		return Dictionary();
	}
	return read_skeleton_data(mesh);
}

void FbxMeshEntry::load_geometry() {
	_rebuild_mesh();
}

void FbxMeshEntry::load_skin() {
	if (!mesh || mesh->skin_deformers.count == 0) {
		return; // Not skinned - silently skip, callable unconditionally in a loop.
	}
	skin_requested = true;
	_rebuild_mesh();
}

void FbxMeshEntry::_rebuild_mesh() {
	if (!mesh) {
		return;
	}

	bool has_skin_deformer = mesh->skin_deformers.count > 0;
	bool has_built_skeleton = owner_scene.is_valid() && owner_scene->has_skeleton();
	bool skin_available = skin_requested && has_skin_deformer && has_built_skeleton;

	if (skin_requested && has_skin_deformer && !has_built_skeleton) {
		UtilityFunctions::print("the mesh '", get_name(), "' isn't skinned");
	}

	SkinBuildInput skin_input;
	if (skin_available) {
		skin_input.deformer = mesh->skin_deformers.data[0];
		skin_input.cluster_to_bone.resize((int)skin_input.deformer->clusters.count);
		for (size_t i = 0; i < skin_input.deformer->clusters.count; i++) {
			ufbx_node *bone_node = skin_input.deformer->clusters.data[i]->bone_node;
			int bone_index = -1;
			if (bone_node) {
				String bone_name = sanitize_bone_name(String::utf8(bone_node->name.data, (int)bone_node->name.length));
				bone_index = owner_scene->find_bone_index(bone_name);
			}
			skin_input.cluster_to_bone.write[(int)i] = bone_index;
		}
	}

	loaded_mesh = build_mesh_geometry(mesh, &surface_material_index, skin_available ? &skin_input : nullptr);
	skin_loaded = skin_available;
	_apply_materials();
}

void FbxMeshEntry::load_material() {
	if (!mesh || !owner_scene.is_valid()) {
		return;
	}

	loaded_materials.clear();
	if (mesh->materials.count > 0) {
		for (size_t i = 0; i < mesh->materials.count; i++) {
			loaded_materials.push_back(build_material(mesh->materials.data[i], owner_scene->get_fbx_dir()));
		}
	} else {
		Ref<StandardMaterial3D> fallback;
		fallback.instantiate();
		loaded_materials.push_back(fallback);
	}

	_apply_materials();
}

void FbxMeshEntry::_apply_materials() {
	if (loaded_mesh.is_null() || loaded_materials.is_empty()) {
		return;
	}

	for (int surface_i = 0; surface_i < surface_material_index.size(); surface_i++) {
		int material_i = surface_material_index[surface_i];
		if (material_i < 0 || material_i >= loaded_materials.size()) {
			continue;
		}
		loaded_mesh->surface_set_material(surface_i, loaded_materials[material_i]);
	}
}

bool FbxMeshEntry::is_geometry_loaded() const {
	return loaded_mesh.is_valid();
}

bool FbxMeshEntry::is_material_loaded() const {
	return !loaded_materials.is_empty();
}

Ref<ArrayMesh> FbxMeshEntry::get_loaded_mesh() const {
	return loaded_mesh;
}

PackedStringArray FbxMeshEntry::get_surface_material_names() const {
	PackedStringArray names;
	if (!mesh) {
		return names;
	}
	for (int i = 0; i < surface_material_index.size(); i++) {
		int material_i = surface_material_index[i];
		if (material_i >= 0 && (size_t)material_i < mesh->materials.count) {
			ufbx_string name = mesh->materials.data[material_i]->name;
			names.push_back(String::utf8(name.data, (int)name.length));
		} else {
			names.push_back(String());
		}
	}
	return names;
}

void FbxMeshEntry::_bind_methods() {
	ClassDB::bind_method(D_METHOD("get_name"), &FbxMeshEntry::get_name);
	ClassDB::bind_method(D_METHOD("get_index"), &FbxMeshEntry::get_index);
	ClassDB::bind_method(D_METHOD("geometry"), &FbxMeshEntry::geometry);
	ClassDB::bind_method(D_METHOD("material"), &FbxMeshEntry::material);
	ClassDB::bind_method(D_METHOD("normal"), &FbxMeshEntry::normal);
	ClassDB::bind_method(D_METHOD("uv"), &FbxMeshEntry::uv);
	ClassDB::bind_method(D_METHOD("skin"), &FbxMeshEntry::skin);
	ClassDB::bind_method(D_METHOD("skeleton"), &FbxMeshEntry::skeleton);
	ClassDB::bind_method(D_METHOD("load_geometry"), &FbxMeshEntry::load_geometry);
	ClassDB::bind_method(D_METHOD("load_material"), &FbxMeshEntry::load_material);
	ClassDB::bind_method(D_METHOD("load_skin"), &FbxMeshEntry::load_skin);
	ClassDB::bind_method(D_METHOD("is_geometry_loaded"), &FbxMeshEntry::is_geometry_loaded);
	ClassDB::bind_method(D_METHOD("is_material_loaded"), &FbxMeshEntry::is_material_loaded);
	ClassDB::bind_method(D_METHOD("is_skin_loaded"), &FbxMeshEntry::is_skin_loaded);
	ClassDB::bind_method(D_METHOD("get_loaded_mesh"), &FbxMeshEntry::get_loaded_mesh);
	ClassDB::bind_method(D_METHOD("get_surface_material_names"), &FbxMeshEntry::get_surface_material_names);
}
