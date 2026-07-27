#include "fbx_convert.hpp"

#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/classes/image.hpp>
#include <godot_cpp/core/math_defs.hpp>
#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/basis.hpp>
#include <godot_cpp/variant/color.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/packed_float32_array.hpp>
#include <godot_cpp/variant/packed_int32_array.hpp>
#include <godot_cpp/variant/packed_vector2_array.hpp>
#include <godot_cpp/variant/packed_vector3_array.hpp>
#include <godot_cpp/variant/quaternion.hpp>

#include <cstdint>
#include <cstring>

namespace fbx_convert {

String sanitize_bone_name(const String &p_name) {
	String name = p_name.replace(":", "_").replace("/", "_");
	if (name.is_empty()) {
		name = "Bone";
	}
	return name;
}

namespace {

// tree.py/hash.py key each tree level by int(name.encode('utf-8').hex(), 16) - an
// arbitrary-precision integer that doesn't fit a 64-bit Dictionary key for any name over 8
// UTF-8 bytes. This 64-bit FNV-1a keeps the same *role* (a stable, name-derived integer key)
// without pretending to reproduce Python's unbounded-integer values.
int64_t hash_name(const String &p_name) {
	CharString utf8 = p_name.utf8();
	uint64_t h = 1469598103934665603ull; // FNV offset basis
	for (int64_t i = 0; i < utf8.length(); i++) {
		h ^= (uint8_t)utf8[i];
		h *= 1099511628211ull; // FNV prime
	}
	return (int64_t)(h & 0x7FFFFFFFFFFFFFFFull);
}

// Maya-style row-major flat list: [Xx,Xy,Xz,0, Yx,Yy,Yz,0, Zx,Zy,Zz,0, Tx,Ty,Tz,1], matching
// list(MMatrix). ufbx_matrix stores the same basis/translation columns as m0*/m1*/m2*/m3*.
Array matrix_to_maya_array(const ufbx_matrix &p_matrix) {
	Array a;
	a.push_back((double)p_matrix.m00);
	a.push_back((double)p_matrix.m10);
	a.push_back((double)p_matrix.m20);
	a.push_back(0.0);
	a.push_back((double)p_matrix.m01);
	a.push_back((double)p_matrix.m11);
	a.push_back((double)p_matrix.m21);
	a.push_back(0.0);
	a.push_back((double)p_matrix.m02);
	a.push_back((double)p_matrix.m12);
	a.push_back((double)p_matrix.m22);
	a.push_back(0.0);
	a.push_back((double)p_matrix.m03);
	a.push_back((double)p_matrix.m13);
	a.push_back((double)p_matrix.m23);
	a.push_back(1.0);
	return a;
}

// skeleton.py reads angles via MPlug::asDouble() on angle-typed attributes, which Maya always
// returns in radians; ufbx stores Euler angles/PreRotation in degrees, so convert.
Array vec3_deg_to_rad_array(const ufbx_vec3 &p_v) {
	const double d2r = Math_PI / 180.0;
	Array a;
	a.push_back(p_v.x * d2r);
	a.push_back(p_v.y * d2r);
	a.push_back(p_v.z * d2r);
	return a;
}

// ufbx_rotation_order and Maya's MEulerRotation::RotationOrder (the enum backing the joint
// "rotateOrder" attribute) don't share numeric values (only XYZ/ZYX happen to line up), so
// translate explicitly instead of casting.
double rotation_order_to_maya(ufbx_rotation_order p_order) {
	switch (p_order) {
		case UFBX_ROTATION_ORDER_XYZ:
			return 0.0;
		case UFBX_ROTATION_ORDER_YZX:
			return 1.0;
		case UFBX_ROTATION_ORDER_ZXY:
			return 2.0;
		case UFBX_ROTATION_ORDER_XZY:
			return 3.0;
		case UFBX_ROTATION_ORDER_YXZ:
			return 4.0;
		case UFBX_ROTATION_ORDER_ZYX:
			return 5.0;
		default:
			return 0.0; // UFBX_ROTATION_ORDER_SPHERIC has no Maya equivalent.
	}
}

// A node "is a bone" if it's referenced as some skin cluster's bone_node - not merely because
// ufbx classified its node attribute as ufbx_bone. Some exporters (this codebase has seen it
// from a Mixamo-derived pipeline) skin against plain, attribute-less transform nodes with no
// FBX "Skeleton" NodeAttribute at all, leaving `ufbx_node.bone` null even though the node is
// unambiguously a joint as far as the mesh's actual skinning is concerned. Callers gather the
// real bone-node set from skin cluster references (see gather_bone_nodes/read_skeleton_data)
// and test membership here instead of trusting `node->bone`.
bool contains_node(const Vector<ufbx_node *> &p_bone_nodes, ufbx_node *p_node) {
	for (int i = 0; i < p_bone_nodes.size(); i++) {
		if (p_bone_nodes[i] == p_node) {
			return true;
		}
	}
	return false;
}

// Scene-wide equivalent of walking one mesh's skin_deformers[0]->clusters[] for bone_node
// (see read_skeleton_data) - every skin cluster's bone, across every mesh in the scene.
Vector<ufbx_node *> gather_bone_nodes(ufbx_scene *p_scene) {
	Vector<ufbx_node *> bone_nodes;
	for (size_t mesh_i = 0; mesh_i < p_scene->meshes.count; mesh_i++) {
		ufbx_mesh *mesh = p_scene->meshes.data[mesh_i];
		if (mesh->skin_deformers.count == 0) {
			continue;
		}
		ufbx_skin_deformer *deformer = mesh->skin_deformers.data[0];
		for (size_t i = 0; i < deformer->clusters.count; i++) {
			ufbx_node *bone_node = deformer->clusters.data[i]->bone_node;
			if (bone_node && !contains_node(bone_nodes, bone_node)) {
				bone_nodes.push_back(bone_node);
			}
		}
	}
	return bone_nodes;
}

// Builds one Leaf-equivalent dict for a bone node. p_world_space selects whether "matrix" is
// this node's world transform (root bone found per anchor, mirroring skeleton.py's from_root
// special case for the very first joint) or its parent-relative local transform (every other
// bone, mirroring MFnTransform.transformationMatrix()).
Dictionary build_bone_leaf(ufbx_node *p_node, int64_t p_id, int64_t p_parent_id, bool p_world_space) {
	Dictionary leaf;
	leaf["id"] = p_id;
	leaf["parent"] = p_parent_id;
	leaf["name"] = String::utf8(p_node->name.data, (int)p_node->name.length);
	leaf["matrix"] = matrix_to_maya_array(p_world_space ? p_node->node_to_world : p_node->node_to_parent);
	leaf["rotation"] = vec3_deg_to_rad_array(p_node->euler_rotation);

	ufbx_prop *pre_rotation = ufbx_find_prop(&p_node->props, UFBX_PreRotation);
	ufbx_vec3 orient = pre_rotation ? pre_rotation->value_vec3 : ufbx_vec3();
	leaf["orient"] = vec3_deg_to_rad_array(orient);

	leaf["radius"] = p_node->bone ? (double)p_node->bone->radius : 0.0;
	leaf["rotateOrder"] = rotation_order_to_maya(p_node->rotation_order);
	// No FBX equivalent for Maya's joint "side"/"type" labeling attributes.
	leaf["side"] = 0.0;
	leaf["type"] = 0.0;

	return leaf;
}

// Recurses through every descendant regardless of type (mirroring MItDag visiting the whole
// DAG), but only creates - and only nests further bones under - a Leaf for nodes in
// p_bone_nodes; intervening non-bone transforms are skipped over rather than given their own
// tree level (see the deviations noted on read_skeleton_data in fbx_convert.hpp).
// p_parent_dict/p_parent_id/p_parent_is_bone describe the nearest kept ancestor so far.
void collect_bones(ufbx_node *p_node, const Vector<ufbx_node *> &p_bone_nodes, Dictionary p_parent_dict, int64_t p_parent_id, bool p_parent_is_bone, Array &p_joint_names) {
	Dictionary next_dict = p_parent_dict;
	int64_t next_id = p_parent_id;
	bool next_is_bone = p_parent_is_bone;

	if (contains_node(p_bone_nodes, p_node)) {
		String name = String::utf8(p_node->name.data, (int)p_node->name.length);
		int64_t id = hash_name(name);
		Dictionary leaf = build_bone_leaf(p_node, id, p_parent_is_bone ? p_parent_id : (int64_t)0, !p_parent_is_bone);
		p_parent_dict[id] = leaf;
		p_joint_names.push_back(name);

		next_dict = leaf;
		next_id = id;
		next_is_bone = true;
	}

	for (size_t i = 0; i < p_node->children.count; i++) {
		collect_bones(p_node->children.data[i], p_bone_nodes, next_dict, next_id, next_is_bone, p_joint_names);
	}
}

// Same "skip non-bone transforms, nest under the nearest bone ancestor" shape as collect_bones,
// but builds real Skeleton3D bones instead of a Dictionary, and - since a rest pose has to be
// geometrically correct rather than just informative - properly composes the local transform
// through any skipped intermediate nodes via p_pending/p_pending_valid (same pattern
// FbxScene::_build_node uses for skipped unloaded-mesh nodes).
void collect_skeleton_bones(ufbx_node *p_node, const Vector<ufbx_node *> &p_bone_nodes, Skeleton3D *p_skeleton, int p_parent_bone, bool p_parent_is_bone, const Transform3D &p_pending, bool p_pending_valid) {
	Transform3D local = ufbx_transform_to_godot(p_node->local_transform);
	Transform3D effective = p_pending_valid ? (p_pending * local) : local;

	int next_parent_bone = p_parent_bone;
	bool next_is_bone = p_parent_is_bone;
	Transform3D next_pending = effective;
	bool next_pending_valid = true;

	if (contains_node(p_bone_nodes, p_node)) {
		String name = sanitize_bone_name(String::utf8(p_node->name.data, (int)p_node->name.length));
		int bone_idx = p_skeleton->add_bone(name);
		p_skeleton->set_bone_parent(bone_idx, p_parent_is_bone ? p_parent_bone : -1);
		p_skeleton->set_bone_rest(bone_idx, effective);

		next_parent_bone = bone_idx;
		next_is_bone = true;
		next_pending = Transform3D();
		next_pending_valid = false;
	}

	for (size_t i = 0; i < p_node->children.count; i++) {
		collect_skeleton_bones(p_node->children.data[i], p_bone_nodes, p_skeleton, next_parent_bone, next_is_bone, next_pending, next_pending_valid);
	}
}

} //namespace

Transform3D ufbx_transform_to_godot(const ufbx_transform &p_transform) {
	Basis basis = Basis(Quaternion(p_transform.rotation.x, p_transform.rotation.y, p_transform.rotation.z, p_transform.rotation.w));
	basis.scale(Vector3(p_transform.scale.x, p_transform.scale.y, p_transform.scale.z));
	Vector3 origin(p_transform.translation.x, p_transform.translation.y, p_transform.translation.z);
	return Transform3D(basis, origin);
}

Transform3D ufbx_matrix_to_godot(const ufbx_matrix &p_matrix) {
	return ufbx_transform_to_godot(ufbx_matrix_to_transform(&p_matrix));
}

Dictionary read_mesh_elements(ufbx_mesh *p_mesh) {
	Dictionary data;

	data["name"] = String::utf8(p_mesh->name.data, (int)p_mesh->name.length);

	PackedVector3Array vertices;
	vertices.resize((int)p_mesh->vertex_position.values.count);
	for (size_t i = 0; i < p_mesh->vertex_position.values.count; i++) {
		ufbx_vec3 v = p_mesh->vertex_position.values.data[i];
		vertices[(int)i] = Vector3(v.x, v.y, v.z);
	}
	data["vertices"] = vertices;

	PackedInt32Array indices;
	indices.resize((int)p_mesh->vertex_position.indices.count);
	for (size_t i = 0; i < p_mesh->vertex_position.indices.count; i++) {
		indices[(int)i] = (int32_t)p_mesh->vertex_position.indices.data[i];
	}
	data["indices"] = indices;

	PackedInt32Array faces;
	faces.resize((int)p_mesh->faces.count);
	for (size_t i = 0; i < p_mesh->faces.count; i++) {
		faces[(int)i] = (int32_t)p_mesh->faces.data[i].num_indices;
	}
	data["faces"] = faces;

	Transform3D matrix;
	if (p_mesh->instances.count > 0) {
		matrix = ufbx_matrix_to_godot(p_mesh->instances.data[0]->node_to_world);
	}
	data["matrix"] = matrix;

	if (p_mesh->vertex_normal.exists) {
		PackedVector3Array normals;
		normals.resize((int)p_mesh->vertex_normal.values.count);
		for (size_t i = 0; i < p_mesh->vertex_normal.values.count; i++) {
			ufbx_vec3 n = p_mesh->vertex_normal.values.data[i];
			normals[(int)i] = Vector3(n.x, n.y, n.z);
		}
		data["normals"] = normals;

		PackedInt32Array normal_indices;
		normal_indices.resize((int)p_mesh->vertex_normal.indices.count);
		for (size_t i = 0; i < p_mesh->vertex_normal.indices.count; i++) {
			normal_indices[(int)i] = (int32_t)p_mesh->vertex_normal.indices.data[i];
		}
		data["normal_indices"] = normal_indices;
	}

	if (p_mesh->vertex_uv.exists) {
		PackedVector2Array uvs;
		uvs.resize((int)p_mesh->vertex_uv.values.count);
		for (size_t i = 0; i < p_mesh->vertex_uv.values.count; i++) {
			ufbx_vec2 uv = p_mesh->vertex_uv.values.data[i];
			uvs[(int)i] = Vector2(uv.x, 1.0f - uv.y); // Flip V: FBX/OpenGL vs Godot UV origin
		}
		data["uvs"] = uvs;

		PackedInt32Array uv_indices;
		uv_indices.resize((int)p_mesh->vertex_uv.indices.count);
		for (size_t i = 0; i < p_mesh->vertex_uv.indices.count; i++) {
			uv_indices[(int)i] = (int32_t)p_mesh->vertex_uv.indices.data[i];
		}
		data["uv_indices"] = uv_indices;
	}

	return data;
}

Ref<ImageTexture> load_image_from_buffer(const PackedByteArray &p_buffer, const String &p_extension) {
	Ref<Image> image;
	image.instantiate();

	String ext = p_extension.to_lower();
	Error err = FAILED;
	if (ext == "png") {
		err = image->load_png_from_buffer(p_buffer);
	} else if (ext == "jpg" || ext == "jpeg") {
		err = image->load_jpg_from_buffer(p_buffer);
	} else if (ext == "webp") {
		err = image->load_webp_from_buffer(p_buffer);
	} else if (ext == "tga") {
		err = image->load_tga_from_buffer(p_buffer);
	} else if (ext == "bmp") {
		err = image->load_bmp_from_buffer(p_buffer);
	} else if (ext == "ktx") {
		err = image->load_ktx_from_buffer(p_buffer);
	}

	if (err != OK || image->is_empty()) {
		return Ref<ImageTexture>();
	}
	return ImageTexture::create_from_image(image);
}

Ref<ImageTexture> load_texture(ufbx_texture *p_texture, const String &p_fbx_dir) {
	if (!p_texture) {
		return Ref<ImageTexture>();
	}

	String filename = String::utf8(p_texture->filename.data, (int)p_texture->filename.length);
	String relative_filename = String::utf8(p_texture->relative_filename.data, (int)p_texture->relative_filename.length);
	String absolute_filename = String::utf8(p_texture->absolute_filename.data, (int)p_texture->absolute_filename.length);
	String name_hint = !filename.is_empty() ? filename : (!relative_filename.is_empty() ? relative_filename : absolute_filename);

	if (p_texture->content.size > 0) {
		PackedByteArray buffer;
		buffer.resize((int)p_texture->content.size);
		memcpy(buffer.ptrw(), p_texture->content.data, p_texture->content.size);

		Ref<ImageTexture> texture = load_image_from_buffer(buffer, name_hint.get_extension());
		if (texture.is_valid()) {
			return texture;
		}
	}

	if (name_hint.is_empty()) {
		return Ref<ImageTexture>();
	}

	Vector<String> candidates;
	if (!p_fbx_dir.is_empty()) {
		candidates.push_back(p_fbx_dir.path_join(name_hint.get_file()));
	}
	candidates.push_back(filename);
	candidates.push_back(absolute_filename);

	for (int i = 0; i < candidates.size(); i++) {
		const String &path = candidates[i];
		if (path.is_empty() || !FileAccess::file_exists(path)) {
			continue;
		}
		Ref<Image> image = Image::load_from_file(path);
		if (image.is_valid() && !image->is_empty()) {
			return ImageTexture::create_from_image(image);
		}
	}

	return Ref<ImageTexture>();
}

Ref<StandardMaterial3D> build_material(ufbx_material *p_material, const String &p_fbx_dir) {
	Ref<StandardMaterial3D> material;
	material.instantiate();

	const ufbx_material_map &diffuse_color = p_material->fbx.diffuse_color;
	if (diffuse_color.has_value && diffuse_color.value_components >= 3) {
		material->set_albedo(Color(diffuse_color.value_vec3.x, diffuse_color.value_vec3.y, diffuse_color.value_vec3.z));
	}
	if (diffuse_color.texture) {
		Ref<ImageTexture> texture = load_texture(diffuse_color.texture, p_fbx_dir);
		if (texture.is_valid()) {
			material->set_texture(BaseMaterial3D::TEXTURE_ALBEDO, texture);
		}
	}

	if (p_material->fbx.normal_map.texture) {
		Ref<ImageTexture> texture = load_texture(p_material->fbx.normal_map.texture, p_fbx_dir);
		if (texture.is_valid()) {
			material->set_texture(BaseMaterial3D::TEXTURE_NORMAL, texture);
			material->set_feature(BaseMaterial3D::FEATURE_NORMAL_MAPPING, true);
		}
	}

	return material;
}

Ref<ArrayMesh> build_mesh_geometry(ufbx_mesh *p_mesh, Vector<int> *p_out_surface_material_index, const SkinBuildInput *p_skin) {
	const bool has_normals = p_mesh->vertex_normal.exists;
	const bool has_uvs = p_mesh->vertex_uv.exists;
	const size_t num_materials = p_mesh->materials.count;
	const size_t num_surfaces = num_materials > 0 ? num_materials : 1;

	Vector<PackedVector3Array> positions_per_surface;
	Vector<PackedVector3Array> normals_per_surface;
	Vector<PackedVector2Array> uvs_per_surface;
	Vector<PackedInt32Array> bones_per_surface;
	Vector<PackedFloat32Array> weights_per_surface;
	positions_per_surface.resize((int)num_surfaces);
	normals_per_surface.resize((int)num_surfaces);
	uvs_per_surface.resize((int)num_surfaces);
	if (p_skin) {
		bones_per_surface.resize((int)num_surfaces);
		weights_per_surface.resize((int)num_surfaces);
	}

	Vector<uint32_t> tri_indices;
	tri_indices.resize((int)p_mesh->max_face_triangles * 3);

	for (size_t face_i = 0; face_i < p_mesh->faces.count; face_i++) {
		ufbx_face face = p_mesh->faces.data[face_i];
		uint32_t num_triangles = ufbx_triangulate_face(tri_indices.ptrw(), tri_indices.size(), p_mesh, face);

		size_t surface_i = 0;
		if (num_materials > 0 && face_i < p_mesh->face_material.count) {
			surface_i = p_mesh->face_material.data[face_i];
		}

		PackedVector3Array &positions = positions_per_surface.write[(int)surface_i];
		PackedVector3Array &normals = normals_per_surface.write[(int)surface_i];
		PackedVector2Array &uvs = uvs_per_surface.write[(int)surface_i];

		for (uint32_t i = 0; i < num_triangles * 3; i++) {
			uint32_t index = tri_indices[i];

			ufbx_vec3 pos = ufbx_get_vertex_vec3(&p_mesh->vertex_position, index);
			positions.push_back(Vector3(pos.x, pos.y, pos.z));

			if (has_normals) {
				ufbx_vec3 n = ufbx_get_vertex_vec3(&p_mesh->vertex_normal, index);
				normals.push_back(Vector3(n.x, n.y, n.z));
			}

			if (has_uvs) {
				ufbx_vec2 uv = ufbx_get_vertex_vec2(&p_mesh->vertex_uv, index);
				uvs.push_back(Vector2(uv.x, 1.0f - uv.y)); // Flip V: FBX/OpenGL vs Godot UV origin
			}

			if (p_skin) {
				PackedInt32Array &bones = bones_per_surface.write[(int)surface_i];
				PackedFloat32Array &weights = weights_per_surface.write[(int)surface_i];

				int32_t bone_ids[4] = { 0, 0, 0, 0 };
				float bone_weights[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
				int written = 0;
				float total = 0.0f;

				// Same corner->control-point mapping already used to resolve position/normal/uv
				// above (vertex_position.indices), not the separate mesh->vertex_indices array -
				// they're documented as an "alternative" access path and aren't guaranteed to
				// agree index-for-index, which was scrambling which vertex got which weights.
				uint32_t control_point = p_mesh->vertex_position.indices.data[index];
				ufbx_skin_vertex sv = p_skin->deformer->vertices.data[control_point];
				for (uint32_t w = 0; w < sv.num_weights && written < 4; w++) {
					ufbx_skin_weight sw = p_skin->deformer->weights.data[sv.weight_begin + w];
					if (sw.cluster_index >= (uint32_t)p_skin->cluster_to_bone.size()) {
						continue;
					}
					int bone_index = p_skin->cluster_to_bone[(int)sw.cluster_index];
					if (bone_index < 0) {
						continue;
					}
					bone_ids[written] = bone_index;
					bone_weights[written] = (float)sw.weight;
					total += (float)sw.weight;
					written++;
				}
				if (total > 0.0f) {
					for (int k = 0; k < written; k++) {
						bone_weights[k] /= total;
					}
				}
				for (int k = 0; k < 4; k++) {
					bones.push_back(bone_ids[k]);
					weights.push_back(bone_weights[k]);
				}
			}
		}
	}

	if (p_out_surface_material_index) {
		p_out_surface_material_index->clear();
	}

	Ref<ArrayMesh> array_mesh;
	for (size_t surface_i = 0; surface_i < num_surfaces; surface_i++) {
		const PackedVector3Array &positions = positions_per_surface[(int)surface_i];
		if (positions.is_empty()) {
			continue;
		}

		Array surface_arrays;
		surface_arrays.resize(ArrayMesh::ARRAY_MAX);
		surface_arrays[ArrayMesh::ARRAY_VERTEX] = positions;
		if (has_normals) {
			surface_arrays[ArrayMesh::ARRAY_NORMAL] = normals_per_surface[(int)surface_i];
		}
		if (has_uvs) {
			surface_arrays[ArrayMesh::ARRAY_TEX_UV] = uvs_per_surface[(int)surface_i];
		}
		if (p_skin) {
			surface_arrays[ArrayMesh::ARRAY_BONES] = bones_per_surface[(int)surface_i];
			surface_arrays[ArrayMesh::ARRAY_WEIGHTS] = weights_per_surface[(int)surface_i];
		}

		if (array_mesh.is_null()) {
			array_mesh.instantiate();
		}
		array_mesh->add_surface_from_arrays(Mesh::PRIMITIVE_TRIANGLES, surface_arrays);

		if (p_out_surface_material_index) {
			p_out_surface_material_index->push_back((int)surface_i);
		}
	}

	return array_mesh;
}

Ref<ArrayMesh> build_mesh(ufbx_mesh *p_mesh, const String &p_fbx_dir) {
	Vector<int> surface_material_index;
	Ref<ArrayMesh> array_mesh = build_mesh_geometry(p_mesh, &surface_material_index, nullptr);

	if (array_mesh.is_valid() && p_mesh->materials.count > 0) {
		for (int godot_surface_i = 0; godot_surface_i < surface_material_index.size(); godot_surface_i++) {
			int material_i = surface_material_index[godot_surface_i];
			array_mesh->surface_set_material(godot_surface_i, build_material(p_mesh->materials.data[material_i], p_fbx_dir));
		}
	}

	return array_mesh;
}

Dictionary read_material_data(ufbx_node *p_node, ufbx_mesh *p_mesh) {
	Dictionary data;

	data["geometry"] = p_node ? String::utf8(p_node->name.data, (int)p_node->name.length)
							   : String::utf8(p_mesh->name.data, (int)p_mesh->name.length);

	// Per-instance material list (falls back to the mesh's own list if it has no instances).
	ufbx_material_list materials = p_node ? p_node->materials : p_mesh->materials;

	Array material_names;
	for (size_t i = 0; i < materials.count; i++) {
		ufbx_string name = materials.data[i]->name;
		material_names.push_back(String::utf8(name.data, (int)name.length));
	}
	data["materials"] = material_names;

	Dictionary paths;
	for (size_t i = 0; i < materials.count; i++) {
		ufbx_material *material = materials.data[i];
		String material_name = String::utf8(material->name.data, (int)material->name.length);

		Dictionary attr_paths;
		if (material->fbx.diffuse_color.texture) {
			ufbx_string fn = material->fbx.diffuse_color.texture->filename;
			attr_paths["DiffuseColor"] = String::utf8(fn.data, (int)fn.length);
		}
		if (material->fbx.normal_map.texture) {
			ufbx_string fn = material->fbx.normal_map.texture->filename;
			attr_paths["NormalMap"] = String::utf8(fn.data, (int)fn.length);
		}
		if (!attr_paths.is_empty()) {
			paths[material_name] = attr_paths;
		}
	}
	data["paths"] = paths;

	Dictionary face_id_map;
	for (size_t face_i = 0; face_i < p_mesh->face_material.count; face_i++) {
		int32_t material_index = (int32_t)p_mesh->face_material.data[face_i];
		Array faces = face_id_map.has(material_index) ? (Array)face_id_map[material_index] : Array();
		faces.push_back((int32_t)face_i);
		face_id_map[material_index] = faces;
	}
	data["face_id_map"] = face_id_map;

	return data;
}

Dictionary read_normal_data(ufbx_mesh *p_mesh) {
	Dictionary data;
	data["geometry"] = String::utf8(p_mesh->name.data, (int)p_mesh->name.length);

	Dictionary normals;
	if (p_mesh->vertex_normal.exists) {
		Vector<Vector3> accum;
		accum.resize((int)p_mesh->num_vertices);
		for (int i = 0; i < accum.size(); i++) {
			accum.write[i] = Vector3();
		}

		for (size_t corner = 0; corner < p_mesh->num_indices; corner++) {
			// Same array ufbx_get_vertex_vec3 uses internally for vertex_position - see the
			// note on this same substitution in build_mesh_geometry's skin pass.
			uint32_t control_point = p_mesh->vertex_position.indices.data[corner];
			ufbx_vec3 n = ufbx_get_vertex_vec3(&p_mesh->vertex_normal, corner);
			accum.write[(int)control_point] += Vector3(n.x, n.y, n.z);
		}

		for (int i = 0; i < accum.size(); i++) {
			if (accum[i].length_squared() > 0.0) {
				normals[i] = accum[i];
			}
		}
	}
	data["normals"] = normals;
	data["faces"] = Array();

	return data;
}

Dictionary read_uv_data(ufbx_mesh *p_mesh) {
	Dictionary data;
	data["geometry"] = String::utf8(p_mesh->name.data, (int)p_mesh->name.length);

	Array indices;
	Array vertices;

	if (p_mesh->vertex_uv.exists) {
		for (size_t i = 0; i < p_mesh->vertex_uv.values.count; i++) {
			ufbx_vec2 uv = p_mesh->vertex_uv.values.data[i];
			Array pair;
			pair.push_back(uv.x);
			pair.push_back(uv.y);
			vertices.push_back(pair);
		}

		for (size_t face_i = 0; face_i < p_mesh->faces.count; face_i++) {
			ufbx_face face = p_mesh->faces.data[face_i];
			Array face_indices;
			for (uint32_t c = 0; c < face.num_indices; c++) {
				uint32_t corner = face.index_begin + c;
				face_indices.push_back((int32_t)p_mesh->vertex_uv.indices.data[corner]);
			}
			indices.push_back(face_indices);
		}
	}

	data["indices"] = indices;
	data["vertices"] = vertices;

	return data;
}

Dictionary read_skin_data(ufbx_mesh *p_mesh) {
	Dictionary data;

	if (p_mesh->skin_deformers.count == 0) {
		return data;
	}

	ufbx_skin_deformer *deformer = p_mesh->skin_deformers.data[0];
	const int vertex_count = (int)p_mesh->num_vertices;

	for (size_t i = 0; i < deformer->clusters.count; i++) {
		ufbx_skin_cluster *cluster = deformer->clusters.data[i];
		if (cluster->num_weights == 0 || !cluster->bone_node) {
			// Matches skin.py's `if len(indices) == 0: continue` (unused influences skipped);
			// the bone_node null-check is a defensive extra, ufbx marks it nullable.
			continue;
		}

		Array all_weights;
		all_weights.resize(vertex_count);
		for (int v = 0; v < vertex_count; v++) {
			all_weights[v] = 0.0;
		}
		for (size_t w = 0; w < cluster->num_weights; w++) {
			uint32_t vertex_index = cluster->vertices.data[w];
			if ((int)vertex_index < vertex_count) {
				all_weights[(int)vertex_index] = cluster->weights.data[w];
			}
		}

		String bone_name = String::utf8(cluster->bone_node->name.data, (int)cluster->bone_node->name.length);
		data[bone_name] = all_weights;
	}

	return data;
}

Dictionary read_skeleton_data(ufbx_mesh *p_mesh) {
	Dictionary data;
	data["id"] = (int64_t)0;
	data["joints"] = Array();

	if (p_mesh->skin_deformers.count == 0) {
		return data;
	}

	// Anchor on this mesh's own influencing bones (same universe as read_skin_data), then walk
	// up to the scene root from each - mirrors skeleton.py's from_root=True _traverse_to_root.
	// Usually every anchor converges on the same single root; dedupe in case they don't.
	ufbx_skin_deformer *deformer = p_mesh->skin_deformers.data[0];
	Vector<ufbx_node *> bone_nodes;
	Vector<ufbx_node *> roots;
	for (size_t i = 0; i < deformer->clusters.count; i++) {
		ufbx_node *bone_node = deformer->clusters.data[i]->bone_node;
		if (!bone_node) {
			continue;
		}
		bone_nodes.push_back(bone_node);

		ufbx_node *node = bone_node;
		while (node->parent && !node->parent->is_root) {
			node = node->parent;
		}

		if (!contains_node(roots, node)) {
			roots.push_back(node);
		}
	}

	Array joint_names;
	for (int i = 0; i < roots.size(); i++) {
		collect_bones(roots[i], bone_nodes, data, 0, false, joint_names);
	}
	data["joints"] = joint_names;

	return data;
}

void build_skeleton_bones(ufbx_scene *p_scene, Skeleton3D *p_skeleton) {
	Vector<ufbx_node *> bone_nodes = gather_bone_nodes(p_scene);
	collect_skeleton_bones(p_scene->root_node, bone_nodes, p_skeleton, -1, false, Transform3D(), false);
}

SkinningSummary summarize_skinning(ufbx_scene *p_scene) {
	SkinningSummary summary;
	summary.mesh_count = (int)p_scene->meshes.count;

	for (size_t mesh_i = 0; mesh_i < p_scene->meshes.count; mesh_i++) {
		ufbx_mesh *mesh = p_scene->meshes.data[mesh_i];
		if (mesh->skin_deformers.count == 0) {
			continue;
		}
		summary.meshes_with_skin_deformers++;

		ufbx_skin_deformer *deformer = mesh->skin_deformers.data[0];
		summary.total_clusters += (int)deformer->clusters.count;
		for (size_t i = 0; i < deformer->clusters.count; i++) {
			if (deformer->clusters.data[i]->bone_node) {
				summary.clusters_with_bone_node++;
			}
		}
	}

	return summary;
}

} //namespace fbx_convert
