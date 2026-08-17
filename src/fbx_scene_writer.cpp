#include "fbx_scene_writer.hpp"

#include "fbx_ascii_writer.hpp"
#include "fbx_binary_writer.hpp"

#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/classes/project_settings.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/core/math_defs.hpp>
#include <godot_cpp/variant/basis.hpp>
#include <godot_cpp/variant/packed_float32_array.hpp>
#include <godot_cpp/variant/packed_float64_array.hpp>
#include <godot_cpp/variant/packed_vector2_array.hpp>
#include <godot_cpp/variant/utility_functions.hpp>
#include <godot_cpp/variant/vector2.hpp>

using namespace godot;

namespace {

PackedVector3Array to_vector3_array(const Variant &p_value) {
	if (p_value.get_type() == Variant::PACKED_VECTOR3_ARRAY) {
		return p_value;
	}
	PackedVector3Array out;
	if (p_value.get_type() == Variant::ARRAY) {
		Array a = p_value;
		out.resize(a.size());
		for (int i = 0; i < a.size(); i++) {
			out[i] = (Vector3)a[i];
		}
	}
	return out;
}

PackedInt32Array to_int32_array(const Variant &p_value) {
	if (p_value.get_type() == Variant::PACKED_INT32_ARRAY) {
		return p_value;
	}
	PackedInt32Array out;
	if (p_value.get_type() == Variant::ARRAY) {
		Array a = p_value;
		out.resize(a.size());
		for (int i = 0; i < a.size(); i++) {
			out[i] = (int32_t)(int64_t)a[i];
		}
	}
	return out;
}

PackedFloat32Array to_float32_array(const Variant &p_value) {
	if (p_value.get_type() == Variant::PACKED_FLOAT32_ARRAY) {
		return p_value;
	}
	PackedFloat32Array out;
	if (p_value.get_type() == Variant::PACKED_FLOAT64_ARRAY) {
		PackedFloat64Array a = p_value;
		out.resize(a.size());
		for (int i = 0; i < a.size(); i++) {
			out[i] = (float)a[i];
		}
	} else if (p_value.get_type() == Variant::ARRAY) {
		Array a = p_value;
		out.resize(a.size());
		for (int i = 0; i < a.size(); i++) {
			out[i] = (float)(double)a[i];
		}
	}
	return out;
}

// Inverse of matrix_to_maya_array() in fbx_convert.cpp: a Maya-style row-major flat 16-float
// list [Xx,Xy,Xz,0, Yx,Yy,Yz,0, Zx,Zy,Zz,0, Tx,Ty,Tz,1] - also exactly the flat-array convention
// FBX itself uses for Transform/TransformLink/PoseNode Matrix arrays, so this same shape is both
// "how we read a set_skeleton() Leaf's matrix" and "how we write one back out".
Transform3D flat16_to_transform3d(const Vector<double> &p_m) {
	if (p_m.size() < 16) {
		return Transform3D();
	}
	Vector3 x_axis(p_m[0], p_m[1], p_m[2]);
	Vector3 y_axis(p_m[4], p_m[5], p_m[6]);
	Vector3 z_axis(p_m[8], p_m[9], p_m[10]);
	Vector3 origin(p_m[12], p_m[13], p_m[14]);
	return Transform3D(Basis(x_axis, y_axis, z_axis), origin);
}

Vector<double> transform3d_to_flat16(const Transform3D &p_xf) {
	Vector3 x = p_xf.basis.get_column(0);
	Vector3 y = p_xf.basis.get_column(1);
	Vector3 z = p_xf.basis.get_column(2);
	Vector3 o = p_xf.origin;
	Vector<double> a;
	a.resize(16);
	a.write[0] = x.x;
	a.write[1] = x.y;
	a.write[2] = x.z;
	a.write[3] = 0.0;
	a.write[4] = y.x;
	a.write[5] = y.y;
	a.write[6] = y.z;
	a.write[7] = 0.0;
	a.write[8] = z.x;
	a.write[9] = z.y;
	a.write[10] = z.z;
	a.write[11] = 0.0;
	a.write[12] = o.x;
	a.write[13] = o.y;
	a.write[14] = o.z;
	a.write[15] = 1.0;
	return a;
}

// A set_skeleton() Leaf's "matrix" arrives as a Godot Array of 16 numbers (mirroring
// matrix_to_maya_array's own Array output on the read side), not a packed array.
Vector<double> array_to_double_vector(const Array &p_a) {
	Vector<double> out;
	out.resize(p_a.size());
	for (int i = 0; i < p_a.size(); i++) {
		out.write[i] = (double)p_a[i];
	}
	return out;
}

// The FBX Model rotation order this writer always declares (no explicit "RotationOrder" property,
// so readers fall back to the PropertyTemplate default, enum 0 = eEulerXYZ) reconstructs Lcl
// Rotation as M = Rx * Ry * Rz applied to a row vector. Godot's Basis::get_euler(), despite the
// confusingly-similar name, composes EULER_ORDER_XYZ the opposite way (M = Rz * Ry * Rx) -
// EULER_ORDER_ZYX is the one whose output angles reconstruct via Rx * Ry * Rz, matching FBX's
// eEulerXYZ. Using EULER_ORDER_XYZ here produces a real-but-wrong rotation for any non-identity
// orientation (a valid rotation comes out, just not the original one).
Vector3 transform3d_to_euler_degrees(const Transform3D &p_xf) {
	return p_xf.basis.get_euler(EulerOrder::EULER_ORDER_ZYX) * (180.0 / Math_PI);
}

// Helpers for building a Properties70 "P:" entry ("template" props, hence tprop) - values are
// transcribed verbatim from a real Maya-exported reference file (test_project/
// maya_export_reference.py / maya_export_reference2.py / fbx_binary_validate.py) wherever the
// exact encoding matters, since ufbx tolerates far looser property encoding than Maya's own FBX
// SDK reader does.
void add_tprop0(FbxNode &p_props, const char *p_name, const char *p_type, const char *p_subtype, const char *p_flags) {
	p_props.add_property(p_name, p_type, p_subtype, p_flags);
}
void add_tprop1(FbxNode &p_props, const char *p_name, const char *p_type, const char *p_subtype, const char *p_flags, double p_v) {
	p_props.add_property(p_name, p_type, p_subtype, p_flags).add_double(p_v);
}
void add_tprop3(FbxNode &p_props, const char *p_name, const char *p_type, const char *p_subtype, const char *p_flags, double p_x, double p_y, double p_z) {
	FbxNode &p = p_props.add_property(p_name, p_type, p_subtype, p_flags);
	p.add_double(p_x);
	p.add_double(p_y);
	p.add_double(p_z);
}
void add_tprop_str(FbxNode &p_props, const char *p_name, const char *p_type, const char *p_subtype, const char *p_flags, const char *p_v) {
	p_props.add_property(p_name, p_type, p_subtype, p_flags).add_string(p_v);
}
// KTime-typed properties (AnimationStack's Local/ReferenceStart/Stop) are real 'L'-encoded int64
// values in a real Maya export, not a generic double like every other P: value here - unlike the
// "NUMBER"-subtyped properties this file otherwise collapses to double regardless of width (see
// this function group's own doc comment above), KTime isn't a NUMBER subtype, so that leniency
// isn't assumed to extend to it.
void add_tprop1_i64(FbxNode &p_props, const char *p_name, const char *p_type, const char *p_subtype, const char *p_flags, int64_t p_v) {
	p_props.add_property(p_name, p_type, p_subtype, p_flags).add_int64(p_v);
}
// A "bool"-typed property needs different encoding depending on where it lives, both found by
// diffing real Maya-exported files against this writer's own output:
//  - On a genuine per-OBJECT Properties70 (an actual Model/NodeAttribute instance), it needs a
//    real 'C'-encoded boolean, not add_tprop1's generic double - Maya's FBX SDK reads "Show"
//    strictly as a bool and treats a double-encoded 1.0 as false, so every imported mesh's shape
//    node came in with visibility off. Use add_tprop1_bool() for these.
//  - Inside a PropertyTemplate's own Properties70 (Definitions/ObjectType/PropertyTemplate), it's
//    the opposite: "bool"-typed template properties need int32 ('I') encoding, not 'C'. Getting
//    this backwards left every imported joint/mesh with translate/rotate/scale limits enabled at
//    min==max==0 (frozen in place, selectable but not editable, right after an otherwise-correct
//    import) - Maya silently ignores a type-mismatched template property and falls back to its
//    own "limit enabled" default instead of the template's declared "disabled". Use
//    add_tprop1_int32(..., "bool", ...) for these (the same helper "int"/"enum" properties use).
void add_tprop1_bool(FbxNode &p_props, const char *p_name, bool p_v) {
	p_props.add_property(p_name, "bool", "", "").add_bool(p_v);
}
// "int"/"enum"-typed properties (DefaultAttributeIndex, InheritType, RotationOrder, UpAxis, ...)
// need a real 'I'-encoded int32 value, not add_tprop1's generic double - every property declared
// "int"/"Integer" or "enum" in a real Maya file is int32-encoded on disk, never a double, unlike
// "double"/"Number"-subtyped properties. Also used for PropertyTemplate "bool" properties - see
// add_tprop1_bool()'s doc comment above.
void add_tprop1_int32(FbxNode &p_props, const char *p_name, const char *p_type, const char *p_subtype, const char *p_flags, int32_t p_v) {
	p_props.add_property(p_name, p_type, p_subtype, p_flags).add_int32(p_v);
}

// The full FbxNode PropertyTemplate default set for Definitions/ObjectType[Model], transcribed
// verbatim from a real Maya-exported reference file (test_project/maya_export_reference.py) -
// used ONLY here, never per-object (see add_transform_properties() below for what a real Model
// instance actually writes). Most values below are inert defaults (limits disabled, identity
// geometric transform, visible); the only ones worth calling out are the intentionally
// non-obvious int32 encodings (see add_tprop1_bool()'s doc comment) and DefaultAttributeIndex's
// -1 ("no attribute") default, which every real Model overrides to 0 itself.
void add_model_property_template(FbxNode &p_object_type) {
	FbxNode &tmpl = p_object_type.add_child("PropertyTemplate");
	tmpl.force_braces = true;
	tmpl.add_string("FbxNode");
	FbxNode &p = tmpl.add_child("Properties70");
	p.force_braces = true;
	add_tprop1_int32(p, "QuaternionInterpolate", "enum", "", "", 0);
	add_tprop3(p, "RotationOffset", "Vector3D", "Vector", "", 0.0, 0.0, 0.0);
	add_tprop3(p, "RotationPivot", "Vector3D", "Vector", "", 0.0, 0.0, 0.0);
	add_tprop3(p, "ScalingOffset", "Vector3D", "Vector", "", 0.0, 0.0, 0.0);
	add_tprop3(p, "ScalingPivot", "Vector3D", "Vector", "", 0.0, 0.0, 0.0);
	add_tprop1_int32(p, "TranslationActive", "bool", "", "", 0);
	add_tprop3(p, "TranslationMin", "Vector3D", "Vector", "", 0.0, 0.0, 0.0);
	add_tprop3(p, "TranslationMax", "Vector3D", "Vector", "", 0.0, 0.0, 0.0);
	add_tprop1_int32(p, "TranslationMinX", "bool", "", "", 0);
	add_tprop1_int32(p, "TranslationMinY", "bool", "", "", 0);
	add_tprop1_int32(p, "TranslationMinZ", "bool", "", "", 0);
	add_tprop1_int32(p, "TranslationMaxX", "bool", "", "", 0);
	add_tprop1_int32(p, "TranslationMaxY", "bool", "", "", 0);
	add_tprop1_int32(p, "TranslationMaxZ", "bool", "", "", 0);
	add_tprop1_int32(p, "RotationOrder", "enum", "", "", 0);
	add_tprop1_int32(p, "RotationSpaceForLimitOnly", "bool", "", "", 0);
	add_tprop1(p, "RotationStiffnessX", "double", "Number", "", 0.0);
	add_tprop1(p, "RotationStiffnessY", "double", "Number", "", 0.0);
	add_tprop1(p, "RotationStiffnessZ", "double", "Number", "", 0.0);
	add_tprop1(p, "AxisLen", "double", "Number", "", 10.0);
	add_tprop3(p, "PreRotation", "Vector3D", "Vector", "", 0.0, 0.0, 0.0);
	add_tprop3(p, "PostRotation", "Vector3D", "Vector", "", 0.0, 0.0, 0.0);
	add_tprop1_int32(p, "RotationActive", "bool", "", "", 0);
	add_tprop3(p, "RotationMin", "Vector3D", "Vector", "", 0.0, 0.0, 0.0);
	add_tprop3(p, "RotationMax", "Vector3D", "Vector", "", 0.0, 0.0, 0.0);
	add_tprop1_int32(p, "RotationMinX", "bool", "", "", 0);
	add_tprop1_int32(p, "RotationMinY", "bool", "", "", 0);
	add_tprop1_int32(p, "RotationMinZ", "bool", "", "", 0);
	add_tprop1_int32(p, "RotationMaxX", "bool", "", "", 0);
	add_tprop1_int32(p, "RotationMaxY", "bool", "", "", 0);
	add_tprop1_int32(p, "RotationMaxZ", "bool", "", "", 0);
	add_tprop1_int32(p, "InheritType", "enum", "", "", 0);
	add_tprop1_int32(p, "ScalingActive", "bool", "", "", 0);
	add_tprop3(p, "ScalingMin", "Vector3D", "Vector", "", 0.0, 0.0, 0.0);
	add_tprop3(p, "ScalingMax", "Vector3D", "Vector", "", 1.0, 1.0, 1.0);
	add_tprop1_int32(p, "ScalingMinX", "bool", "", "", 0);
	add_tprop1_int32(p, "ScalingMinY", "bool", "", "", 0);
	add_tprop1_int32(p, "ScalingMinZ", "bool", "", "", 0);
	add_tprop1_int32(p, "ScalingMaxX", "bool", "", "", 0);
	add_tprop1_int32(p, "ScalingMaxY", "bool", "", "", 0);
	add_tprop1_int32(p, "ScalingMaxZ", "bool", "", "", 0);
	add_tprop3(p, "GeometricTranslation", "Vector3D", "Vector", "", 0.0, 0.0, 0.0);
	add_tprop3(p, "GeometricRotation", "Vector3D", "Vector", "", 0.0, 0.0, 0.0);
	add_tprop3(p, "GeometricScaling", "Vector3D", "Vector", "", 1.0, 1.0, 1.0);
	add_tprop1(p, "MinDampRangeX", "double", "Number", "", 0.0);
	add_tprop1(p, "MinDampRangeY", "double", "Number", "", 0.0);
	add_tprop1(p, "MinDampRangeZ", "double", "Number", "", 0.0);
	add_tprop1(p, "MaxDampRangeX", "double", "Number", "", 0.0);
	add_tprop1(p, "MaxDampRangeY", "double", "Number", "", 0.0);
	add_tprop1(p, "MaxDampRangeZ", "double", "Number", "", 0.0);
	add_tprop1(p, "MinDampStrengthX", "double", "Number", "", 0.0);
	add_tprop1(p, "MinDampStrengthY", "double", "Number", "", 0.0);
	add_tprop1(p, "MinDampStrengthZ", "double", "Number", "", 0.0);
	add_tprop1(p, "MaxDampStrengthX", "double", "Number", "", 0.0);
	add_tprop1(p, "MaxDampStrengthY", "double", "Number", "", 0.0);
	add_tprop1(p, "MaxDampStrengthZ", "double", "Number", "", 0.0);
	add_tprop1(p, "PreferedAngleX", "double", "Number", "", 0.0);
	add_tprop1(p, "PreferedAngleY", "double", "Number", "", 0.0);
	add_tprop1(p, "PreferedAngleZ", "double", "Number", "", 0.0);
	add_tprop0(p, "LookAtProperty", "object", "", "");
	add_tprop0(p, "UpVectorProperty", "object", "", "");
	add_tprop1_int32(p, "Show", "bool", "", "", 1);
	add_tprop1_int32(p, "NegativePercentShapeSupport", "bool", "", "", 1);
	add_tprop1_int32(p, "DefaultAttributeIndex", "int", "Integer", "", -1);
	add_tprop1_int32(p, "Freeze", "bool", "", "", 0);
	add_tprop1_int32(p, "LODBox", "bool", "", "", 0);
	add_tprop3(p, "Lcl Translation", "Lcl Translation", "", "A", 0.0, 0.0, 0.0);
	add_tprop3(p, "Lcl Rotation", "Lcl Rotation", "", "A", 0.0, 0.0, 0.0);
	add_tprop3(p, "Lcl Scaling", "Lcl Scaling", "", "A", 1.0, 1.0, 1.0);
	add_tprop1(p, "Visibility", "Visibility", "", "A", 1.0);
	add_tprop1_int32(p, "Visibility Inheritance", "Visibility Inheritance", "", "", 1);
}

// The actual per-object Properties70 a real Maya-authored Model writes - only the handful of
// properties whose value differs from add_model_property_template()'s class defaults above.
// Deliberately does NOT write the Translation/Rotation/Scaling limit properties, Geometric*, or
// Show/Visibility/Freeze/LODBox/etc. - all left at their PropertyTemplate default, matching real
// Maya's own file structure. ScalingMax is a literal (0,0,0), not this object's actual scale -
// that's what real Maya itself writes there too (ScalingActive is off, so it's inert either way).
void add_transform_properties(FbxNode &p_model, const Transform3D &p_xf) {
	FbxNode &props = p_model.add_child("Properties70");
	props.force_braces = true;
	Vector3 t = p_xf.origin;
	Vector3 r_deg = transform3d_to_euler_degrees(p_xf);
	Vector3 s = p_xf.basis.get_scale();
	add_tprop1_bool(props, "RotationActive", 1);
	add_tprop1_int32(props, "InheritType", "enum", "", "", 1);
	add_tprop3(props, "ScalingMax", "Vector3D", "Vector", "", 0.0, 0.0, 0.0);
	add_tprop1_int32(props, "DefaultAttributeIndex", "int", "Integer", "", 0);
	add_tprop3(props, "Lcl Translation", "Lcl Translation", "", "A", t.x, t.y, t.z);
	add_tprop3(props, "Lcl Rotation", "Lcl Rotation", "", "A", r_deg.x, r_deg.y, r_deg.z);
	add_tprop3(props, "Lcl Scaling", "Lcl Scaling", "", "A", s.x, s.y, s.z);
}

void add_geometry_property_template(FbxNode &p_object_type) {
	FbxNode &tmpl = p_object_type.add_child("PropertyTemplate");
	tmpl.force_braces = true;
	tmpl.add_string("FbxMesh");
	FbxNode &p = tmpl.add_child("Properties70");
	p.force_braces = true;
	add_tprop3(p, "Color", "ColorRGB", "Color", "", 0.8, 0.8, 0.8);
	add_tprop3(p, "BBoxMin", "Vector3D", "Vector", "", 0.0, 0.0, 0.0);
	add_tprop3(p, "BBoxMax", "Vector3D", "Vector", "", 0.0, 0.0, 0.0);
	add_tprop1_int32(p, "Primary Visibility", "bool", "", "", 1);
	add_tprop1_int32(p, "Casts Shadows", "bool", "", "", 1);
	add_tprop1_int32(p, "Receive Shadows", "bool", "", "", 1);
}

void add_material_property_template(FbxNode &p_object_type) {
	FbxNode &tmpl = p_object_type.add_child("PropertyTemplate");
	tmpl.force_braces = true;
	tmpl.add_string("FbxSurfaceMaterial");
	FbxNode &p = tmpl.add_child("Properties70");
	p.force_braces = true;
	add_tprop_str(p, "ShadingModel", "KString", "", "", "Unknown");
	add_tprop1_int32(p, "MultiLayer", "bool", "", "", 0);
}

void add_skeleton_property_template(FbxNode &p_object_type) {
	FbxNode &tmpl = p_object_type.add_child("PropertyTemplate");
	tmpl.force_braces = true;
	tmpl.add_string("FbxSkeleton");
	FbxNode &p = tmpl.add_child("Properties70");
	p.force_braces = true;
	add_tprop3(p, "Color", "ColorRGB", "Color", "", 0.8, 0.8, 0.8);
	add_tprop1(p, "Size", "double", "Number", "", 100.0);
	add_tprop1(p, "LimbLength", "double", "Number", "H", 1.0);
}

// Flat per-face normal (cross of the first two edges from the polygon's first corner), summed
// into every one of its corners' control points - same accumulate-per-control-point shape
// fbx_convert.cpp's read_normal_data() already uses for the opposite (reading) direction.
Dictionary compute_default_normals(const PackedVector3Array &p_vertices, const PackedInt32Array &p_indices, const PackedInt32Array &p_faces) {
	Vector<Vector3> accum;
	accum.resize(p_vertices.size());
	for (int i = 0; i < accum.size(); i++) {
		accum.write[i] = Vector3();
	}

	int corner = 0;
	for (int f = 0; f < p_faces.size(); f++) {
		int count = p_faces[f];
		if (count >= 3) {
			int i0 = p_indices[corner];
			int i1 = p_indices[corner + 1];
			int i2 = p_indices[corner + 2];
			if (i0 >= 0 && i0 < p_vertices.size() && i1 >= 0 && i1 < p_vertices.size() && i2 >= 0 && i2 < p_vertices.size()) {
				Vector3 face_normal = (p_vertices[i1] - p_vertices[i0]).cross(p_vertices[i2] - p_vertices[i0]);
				for (int c = 0; c < count; c++) {
					int vi = p_indices[corner + c];
					if (vi >= 0 && vi < accum.size()) {
						accum.write[vi] += face_normal;
					}
				}
			}
		}
		corner += count;
	}

	Dictionary normals;
	for (int i = 0; i < accum.size(); i++) {
		Vector3 n = accum[i];
		normals[i] = n.length_squared() > 0.0 ? n.normalized() : Vector3(0, 1, 0);
	}
	return normals;
}

} //namespace

int FbxSceneWriter::set_mesh(Dictionary p_geometry) {
	PackedVector3Array vertices = to_vector3_array(p_geometry.get("vertices", Variant()));
	PackedInt32Array indices = to_int32_array(p_geometry.get("indices", Variant()));
	PackedInt32Array faces = to_int32_array(p_geometry.get("faces", Variant()));

	if (vertices.is_empty() || indices.is_empty() || faces.is_empty()) {
		UtilityFunctions::printerr("FbxSceneWriter: set_mesh() requires non-empty \"vertices\", \"indices\" and \"faces\"");
		return -1;
	}

	int expected_indices = 0;
	for (int i = 0; i < faces.size(); i++) {
		expected_indices += faces[i];
	}
	if (expected_indices != indices.size()) {
		UtilityFunctions::printerr("FbxSceneWriter: set_mesh() \"indices\" has ", indices.size(),
				" entries but \"faces\" sums to ", expected_indices, " - they must match");
		return -1;
	}

	WriterMesh mesh;
	mesh.name = p_geometry.get("name", String("Mesh"));
	mesh.vertices = vertices;
	mesh.indices = indices;
	mesh.faces = faces;
	Variant matrix_v = p_geometry.get("matrix", Variant());
	if (matrix_v.get_type() == Variant::TRANSFORM3D) {
		mesh.matrix = matrix_v;
	}

	// Default fallbacks set_uv()/set_normal()/set_material() may replace.
	mesh.materials.push_back("DefaultMaterial");
	Array all_faces;
	for (int i = 0; i < faces.size(); i++) {
		all_faces.push_back(i);
	}
	mesh.face_id_map[0] = all_faces;

	meshes.push_back(mesh);
	return meshes.size() - 1;
}

void FbxSceneWriter::set_uv(int p_mesh_index, Dictionary p_uv) {
	if (p_mesh_index < 0 || p_mesh_index >= meshes.size()) {
		UtilityFunctions::printerr("FbxSceneWriter: set_uv() mesh index ", p_mesh_index, " out of range (", meshes.size(), " meshes)");
		return;
	}
	WriterMesh &mesh = meshes.write[p_mesh_index];

	Array indices = p_uv.get("indices", Array());
	if (indices.size() != mesh.faces.size()) {
		UtilityFunctions::printerr("FbxSceneWriter: set_uv() \"indices\" has ", indices.size(),
				" polygons but mesh ", p_mesh_index, " has ", mesh.faces.size(), " - they must match");
		return;
	}

	mesh.has_uv = true;
	mesh.uv_vertices = p_uv.get("vertices", Array());
	mesh.uv_indices = indices;
}

void FbxSceneWriter::set_normal(int p_mesh_index, Dictionary p_normal) {
	if (p_mesh_index < 0 || p_mesh_index >= meshes.size()) {
		UtilityFunctions::printerr("FbxSceneWriter: set_normal() mesh index ", p_mesh_index, " out of range (", meshes.size(), " meshes)");
		return;
	}
	WriterMesh &mesh = meshes.write[p_mesh_index];
	mesh.has_normal_override = true;
	mesh.normals = p_normal.get("normals", Dictionary());
}

void FbxSceneWriter::set_material(int p_mesh_index, Dictionary p_material) {
	if (p_mesh_index < 0 || p_mesh_index >= meshes.size()) {
		UtilityFunctions::printerr("FbxSceneWriter: set_material() mesh index ", p_mesh_index, " out of range (", meshes.size(), " meshes)");
		return;
	}
	WriterMesh &mesh = meshes.write[p_mesh_index];

	Array names = p_material.get("materials", Array());
	if (names.is_empty()) {
		UtilityFunctions::printerr("FbxSceneWriter: set_material() requires a non-empty \"materials\" list");
		return;
	}

	PackedStringArray materials;
	materials.resize(names.size());
	for (int i = 0; i < names.size(); i++) {
		materials[i] = names[i];
	}

	mesh.materials = materials;
	mesh.material_paths = p_material.get("paths", Dictionary());
	mesh.face_id_map = p_material.get("face_id_map", Dictionary());
}

namespace {
// Leaf metadata field names (see FbxMeshEntry::skeleton()'s doc comment) - every other key on a
// Leaf/skeleton Dictionary is a nested Leaf. Matched by name rather than by Variant key type
// because a Dictionary literal built in GDScript keys a nested Leaf with a real int (matching
// read_skeleton_data's own int64 hash keys), but the same shape round-tripped through JSON
// (json.parse_string() on a file written by the external FbxData/MayaData tools this format
// mirrors) turns every key - metadata and nested-Leaf hash alike - into a String, since JSON
// object keys are always strings. Some of those hash keys also exceed 64-bit range (the
// reference tool's own Python arbitrary-precision hash), so they couldn't be normalized to int
// keys even if we wanted to.
bool is_leaf_metadata_key(const Variant &p_key) {
	String k = p_key;
	return k == "id" || k == "parent" || k == "name" || k == "matrix" || k == "orient" ||
			k == "rotation" || k == "radius" || k == "rotateOrder" || k == "side" || k == "type";
}
} //namespace

void FbxSceneWriter::_collect_bone_leaf(const Dictionary &p_leaf, int p_parent_index, const Transform3D &p_parent_world) {
	WriterBone bone;
	bone.name = p_leaf.get("name", String());
	bone.parent_index = p_parent_index;

	Array matrix_array = p_leaf.get("matrix", Array());
	bone.local = flat16_to_transform3d(array_to_double_vector(matrix_array));
	bone.world = p_parent_world * bone.local;

	bones.push_back(bone);
	int this_index = bones.size() - 1;

	Array keys = p_leaf.keys();
	for (int i = 0; i < keys.size(); i++) {
		Variant key = keys[i];
		if (is_leaf_metadata_key(key)) {
			continue;
		}
		Variant child_v = p_leaf[key];
		if (child_v.get_type() == Variant::DICTIONARY) {
			_collect_bone_leaf(child_v, this_index, bone.world);
		}
	}
}

void FbxSceneWriter::set_skeleton(Dictionary p_skeleton) {
	bones.clear();

	Array keys = p_skeleton.keys();
	for (int i = 0; i < keys.size(); i++) {
		Variant key = keys[i];
		String k = key;
		if (k == "id" || k == "joints") {
			continue;
		}
		Variant leaf_v = p_skeleton[key];
		if (leaf_v.get_type() == Variant::DICTIONARY) {
			// A top-level Leaf's "matrix" is already world-space (see set_skeleton's doc
			// comment), so it composes against an identity parent transform.
			_collect_bone_leaf(leaf_v, -1, Transform3D());
		}
	}
}

void FbxSceneWriter::set_skin(int p_mesh_index, Dictionary p_skin) {
	if (bones.is_empty()) {
		UtilityFunctions::printerr("FbxSceneWriter: set_skin() called before set_skeleton() - skin data needs a skeleton to resolve bone names against");
		return;
	}
	if (p_mesh_index < 0 || p_mesh_index >= meshes.size()) {
		UtilityFunctions::printerr("FbxSceneWriter: set_skin() mesh index ", p_mesh_index, " out of range (", meshes.size(), " meshes)");
		return;
	}
	WriterMesh &mesh = meshes.write[p_mesh_index];

	Array bone_names = p_skin.keys();
	Dictionary resolved;
	for (int i = 0; i < bone_names.size(); i++) {
		String bone_name = bone_names[i];
		bool found = false;
		for (int b = 0; b < bones.size(); b++) {
			if (bones[b].name == bone_name) {
				found = true;
				break;
			}
		}
		if (!found) {
			UtilityFunctions::printerr("FbxSceneWriter: set_skin() bone \"", bone_name, "\" not found in the current skeleton - skipped");
			continue;
		}
		resolved[bone_name] = to_float32_array(p_skin[bone_name]);
	}
	mesh.skin = resolved;
}

namespace {

struct PendingConn {
	String type; // "OO" or "OP".
	int64_t child = 0;
	int64_t parent = 0;
	String prop; // Only used for "OP".
};

void connect_oo(Vector<PendingConn> &r_conns, int64_t p_child, int64_t p_parent) {
	PendingConn c;
	c.type = "OO";
	c.child = p_child;
	c.parent = p_parent;
	r_conns.push_back(c);
}

void connect_op(Vector<PendingConn> &r_conns, int64_t p_child, int64_t p_parent, const String &p_prop) {
	PendingConn c;
	c.type = "OP";
	c.child = p_child;
	c.parent = p_parent;
	c.prop = p_prop;
	r_conns.push_back(c);
}

} //namespace

bool FbxSceneWriter::export_scene(String p_path, bool p_binary) const {
	String global_path = ProjectSettings::get_singleton()->globalize_path(p_path);
	Ref<FileAccess> file = FileAccess::open(global_path, FileAccess::WRITE);
	if (file.is_null()) {
		UtilityFunctions::printerr("FbxSceneWriter: export_scene() failed to open \"", p_path, "\" for writing");
		return false;
	}

	int64_t next_id = 1000000;
	Vector<PendingConn> conns;
	FbxNode file_root;

	{
		// FileId/CreationTime/CreationTimeStamp below are NOT computed per export - they're a
		// fixed, verbatim-copied triple from one real Maya-exported reference file
		// (test_project/maya_export_reference.py), and this is deliberate, not a placeholder to
		// improve later: Maya's own FBX SDK reader silently imports zero objects from any file
		// whose top-level FileId isn't a genuinely SDK-generated value paired with its own
		// originally-exported CreationTime/CreationTimeStamp - a synthesized value is rejected just
		// the same as a missing one. That pairing does NOT need to correspond to this file's actual
		// content though - swapping this exact triple onto completely different, independently-
		// generated Objects/Definitions/Connections still imports correctly, so one real triple,
		// captured once, unlocks every export. The one visible side effect: every file this writer
		// produces reports the same "creation" timestamp.
		FbxNode &header = file_root.add_child("FBXHeaderExtension");
		header.force_braces = true;
		header.add_child("FBXHeaderVersion").add_int32(1003);
		header.add_child("FBXVersion").add_int32(7400);
		header.add_child("EncryptionType").add_int32(0);
		{
			FbxNode &cts = header.add_child("CreationTimeStamp");
			cts.force_braces = true;
			cts.add_child("Version").add_int32(1000);
			cts.add_child("Year").add_int32(2026);
			cts.add_child("Month").add_int32(8);
			cts.add_child("Day").add_int32(15);
			cts.add_child("Hour").add_int32(18);
			cts.add_child("Minute").add_int32(45);
			cts.add_child("Second").add_int32(3);
			cts.add_child("Millisecond").add_int32(812);
		}
		header.add_child("Creator").add_string("godot-template FbxSceneWriter");

		// Real Maya-authored files always carry this SceneInfo block declaring
		// Original|ApplicationVendor=Autodesk/ApplicationName=Maya (test_project/
		// maya_export_reference.py's dump) - matched for structural fidelity, though not itself
		// required for a correct/editable import (see add_tprop1_bool()'s doc comment for what was).
		FbxNode &scene_info = header.add_child("SceneInfo");
		scene_info.force_braces = true;
		scene_info.add_type_and_name("SceneInfo", "GlobalInfo");
		scene_info.add_string("UserData");
		scene_info.add_child("Type").add_string("UserData");
		scene_info.add_child("Version").add_int32(100);
		{
			FbxNode &meta = scene_info.add_child("MetaData");
			meta.force_braces = true;
			meta.add_child("Version").add_int32(100);
			meta.add_child("Title").add_string("");
			meta.add_child("Subject").add_string("");
			meta.add_child("Author").add_string("");
			meta.add_child("Keywords").add_string("");
			meta.add_child("Revision").add_string("");
			meta.add_child("Comment").add_string("");
		}
		{
			FbxNode &props = scene_info.add_child("Properties70");
			props.force_braces = true;
			add_tprop0(props, "Original", "Compound", "", "");
			add_tprop_str(props, "Original|ApplicationVendor", "KString", "", "", "Autodesk");
			add_tprop_str(props, "Original|ApplicationName", "KString", "", "", "Maya");
			add_tprop_str(props, "Original|ApplicationVersion", "KString", "", "", "2026");
			add_tprop0(props, "LastSaved", "Compound", "", "");
			add_tprop_str(props, "LastSaved|ApplicationVendor", "KString", "", "", "Autodesk");
			add_tprop_str(props, "LastSaved|ApplicationName", "KString", "", "", "Maya");
			add_tprop_str(props, "LastSaved|ApplicationVersion", "KString", "", "", "2026");
		}
	}

	{
		static const uint8_t seed_file_id[16] = { 0x28, 0xb0, 0x29, 0xe1, 0xbc, 0x26, 0xcf, 0xc5, 0xb0, 0xc6, 0xbd, 0x2e, 0xa8, 0x2c, 0xff, 0xf7 };
		PackedByteArray file_id;
		file_id.resize(16);
		for (int i = 0; i < 16; i++) {
			file_id[i] = seed_file_id[i];
		}
		file_root.add_child("FileId").add_raw_bytes(file_id);
	}
	file_root.add_child("CreationTime").add_string("2026-08-15 18:45:03:812");
	file_root.add_child("Creator").add_string("godot-template FbxSceneWriter");

	{
		FbxNode &gs = file_root.add_child("GlobalSettings");
		gs.force_braces = true;
		gs.add_child("Version").add_int32(1000);
		FbxNode &props = gs.add_child("Properties70");
		props.force_braces = true;
		// Right-handed Y-up, +Z front - matches ufbx_axes_right_handed_y_up exactly (see
		// fbx_manager.cpp's opts.target_axes), so re-loading this file through this same plugin
		// applies no axis-conversion rotation at all.
		props.add_property("UpAxis", "int", "Integer", "").add_int32(1);
		props.add_property("UpAxisSign", "int", "Integer", "").add_int32(1);
		props.add_property("FrontAxis", "int", "Integer", "").add_int32(2);
		props.add_property("FrontAxisSign", "int", "Integer", "").add_int32(1);
		props.add_property("CoordAxis", "int", "Integer", "").add_int32(0);
		props.add_property("CoordAxisSign", "int", "Integer", "").add_int32(1);
		props.add_property("OriginalUpAxis", "int", "Integer", "").add_int32(1);
		props.add_property("OriginalUpAxisSign", "int", "Integer", "").add_int32(1);
		// UnitScaleFactor is "centimeters per native unit"; 1.0 (FBX's own native default) means
		// "these numbers are already in the file's native unit, apply no conversion". Deliberately
		// unit-agnostic: this writer never rescales the caller's own numbers (set_mesh() vertices,
		// set_skeleton() Leaf matrices, ...), so whatever unit the caller's data is already in is
		// what ends up in the file. A non-1.0 factor would also hit a real Maya FBX SDK asymmetry -
		// Maya applies it as an actual scale on *Mesh*-type Model transforms but not on LimbNode
		// ones, so a skinned character would import with its mesh scaled relative to its own
		// skeleton. Declaring 1.0 sidesteps that entirely: no conversion ratio, nothing to disagree.
		props.add_property("UnitScaleFactor", "double", "Number", "").add_double(1.0);
		props.add_property("OriginalUnitScaleFactor", "double", "Number", "").add_double(1.0);
		// Without a declared TimeMode, Maya's FBX SDK importer treats the file as a frame-rate
		// mismatch against the current scene ("Warning: Frame rate mismatch: The imported scene
		// frame rate 'ntsc' differs from the existing frame rate 'film'" on every import) - absent
		// when importing a real Maya-exported file. Matched here byte-for-byte against a real
		// Maya session's own GlobalSettings.
		add_tprop3(props, "AmbientColor", "ColorRGB", "Color", "", 0.0, 0.0, 0.0);
		add_tprop_str(props, "DefaultCamera", "KString", "", "", "Producer Perspective");
		add_tprop1_int32(props, "TimeMode", "enum", "", "", 11);
		add_tprop1_int32(props, "TimeProtocol", "enum", "", "", 2);
		add_tprop1_int32(props, "SnapOnFrameMode", "enum", "", "", 0);
		add_tprop1_i64(props, "TimeSpanStart", "KTime", "Time", "", 1924423250);
		add_tprop1_i64(props, "TimeSpanStop", "KTime", "Time", "", 384884650000);
		add_tprop1(props, "CustomFrameRate", "double", "Number", "", -1.0);
		add_tprop0(props, "TimeMarker", "Compound", "", "");
		add_tprop1_int32(props, "CurrentTimeMarker", "int", "Integer", "", -1);
	}

	{
		// ufbx (see ufbxi_read_document() in ufbx.c) only learns the root node's object id from
		// this block's "RootNode" property - without it, uc->root_id silently stays whatever it
		// defaulted to rather than the "0" every Connections entry below assumes, so nothing ends
		// up parented under the actual root_node the read side walks.
		FbxNode &docs = file_root.add_child("Documents");
		docs.force_braces = true;
		docs.add_child("Count").add_int32(1);
		FbxNode &doc = docs.add_child("Document");
		doc.force_braces = true;
		doc.add_int64(next_id++);
		doc.add_string("");
		doc.add_string("Scene");
		doc.add_child("RootNode").add_int64(0);
	}
	{ FbxNode &n = file_root.add_child("References"); n.force_braces = true; }

	{
		// Counts are upper bounds, not byte-exact (e.g. deformer_count doesn't account for a bone
		// contributing zero non-zero-weighted vertices and being skipped later) - fine, since ufbx
		// and our own structural validator ignore Definitions counts entirely, and what Maya's own
		// reader actually needs is ObjectType/PropertyTemplate presence, not exact counts.
		int material_count = 0;
		for (int i = 0; i < meshes.size(); i++) {
			material_count += meshes[i].materials.size();
		}
		int model_count = meshes.size() + bones.size();
		int deformer_count = 0;
		int anim_curve_node_count = 0;
		bool has_skin = false;
		for (int i = 0; i < meshes.size(); i++) {
			if (!meshes[i].skin.is_empty()) {
				has_skin = true;
				deformer_count += 1 + meshes[i].skin.size();
				anim_curve_node_count += meshes[i].skin.size();
			}
		}

		FbxNode &defs = file_root.add_child("Definitions");
		defs.force_braces = true;
		defs.add_child("Version").add_int32(100);
		defs.add_child("Count").add_int32(1 + meshes.size() + model_count + material_count + deformer_count + (has_skin ? 1 : 0) + bones.size() + (has_skin ? 2 + anim_curve_node_count : 0));

		FbxNode &ot_gs = defs.add_child("ObjectType");
		ot_gs.force_braces = true;
		ot_gs.add_string("GlobalSettings");
		ot_gs.add_child("Count").add_int32(1);

		if (has_skin) {
			// A real Maya FBX export - even one with no actual keyframed animation - always
			// includes an AnimationStack/AnimationLayer pair plus a "lockInfluenceWeights"
			// AnimationCurveNode per skinCluster influence (test_project/
			// maya_export_reference2.py's own re-export dump) - matched for structural fidelity.
			// See export_scene()'s Deformer/SubDeformer-writing loop for the matching
			// AnimationCurveNode objects/connections.
			FbxNode &ot_stack = defs.add_child("ObjectType");
			ot_stack.force_braces = true;
			ot_stack.add_string("AnimationStack");
			ot_stack.add_child("Count").add_int32(1);
			{
				FbxNode &tmpl = ot_stack.add_child("PropertyTemplate");
				tmpl.force_braces = true;
				tmpl.add_string("FbxAnimStack");
				FbxNode &p = tmpl.add_child("Properties70");
				p.force_braces = true;
				add_tprop_str(p, "Description", "KString", "", "", "");
				add_tprop1_i64(p, "LocalStart", "KTime", "Time", "", 0);
				add_tprop1_i64(p, "LocalStop", "KTime", "Time", "", 0);
				add_tprop1_i64(p, "ReferenceStart", "KTime", "Time", "", 0);
				add_tprop1_i64(p, "ReferenceStop", "KTime", "Time", "", 0);
			}

			FbxNode &ot_layer = defs.add_child("ObjectType");
			ot_layer.force_braces = true;
			ot_layer.add_string("AnimationLayer");
			ot_layer.add_child("Count").add_int32(1);
			{
				FbxNode &tmpl = ot_layer.add_child("PropertyTemplate");
				tmpl.force_braces = true;
				tmpl.add_string("FbxAnimLayer");
				FbxNode &p = tmpl.add_child("Properties70");
				p.force_braces = true;
				add_tprop1(p, "Weight", "Number", "", "A", 100.0);
				add_tprop1_int32(p, "Mute", "bool", "", "", 0);
				add_tprop1_int32(p, "Solo", "bool", "", "", 0);
				add_tprop1_int32(p, "Lock", "bool", "", "", 0);
				FbxNode &color = p.add_property("Color", "ColorRGB", "Color", "");
				color.add_double(0.8);
				color.add_double(0.8);
				color.add_double(0.8);
				add_tprop1_int32(p, "BlendMode", "enum", "", "", 0);
				add_tprop1_int32(p, "RotationAccumulationMode", "enum", "", "", 0);
				add_tprop1_int32(p, "ScaleAccumulationMode", "enum", "", "", 0);
				add_tprop1(p, "BlendModeBypass", "ULongLong", "", "", 0);
			}

			FbxNode &ot_curve = defs.add_child("ObjectType");
			ot_curve.force_braces = true;
			ot_curve.add_string("AnimationCurveNode");
			ot_curve.add_child("Count").add_int32(anim_curve_node_count);
			{
				FbxNode &tmpl = ot_curve.add_child("PropertyTemplate");
				tmpl.force_braces = true;
				tmpl.add_string("FbxAnimCurveNode");
				FbxNode &p = tmpl.add_child("Properties70");
				p.force_braces = true;
				add_tprop0(p, "d", "Compound", "", "");
			}
		}

		if (bones.size() > 0) {
			FbxNode &ot_attr = defs.add_child("ObjectType");
			ot_attr.force_braces = true;
			ot_attr.add_string("NodeAttribute");
			ot_attr.add_child("Count").add_int32(bones.size());
			add_skeleton_property_template(ot_attr);
		}

		FbxNode &ot_model = defs.add_child("ObjectType");
		ot_model.force_braces = true;
		ot_model.add_string("Model");
		ot_model.add_child("Count").add_int32(model_count);
		add_model_property_template(ot_model);

		FbxNode &ot_geom = defs.add_child("ObjectType");
		ot_geom.force_braces = true;
		ot_geom.add_string("Geometry");
		ot_geom.add_child("Count").add_int32(meshes.size());
		add_geometry_property_template(ot_geom);

		if (material_count > 0) {
			FbxNode &ot_mat = defs.add_child("ObjectType");
			ot_mat.force_braces = true;
			ot_mat.add_string("Material");
			ot_mat.add_child("Count").add_int32(material_count);
			add_material_property_template(ot_mat);
		}

		// Pose declared before Deformer here, matching the same before-Deformer/SubDeformer
		// ordering export_scene() already uses when writing the actual Objects (see that loop's own
		// comment) - Maya's FBX SDK reader evidently expects Definitions' ObjectType order to track
		// Objects' own order, not just be present.
		if (has_skin) {
			FbxNode &ot_pose = defs.add_child("ObjectType");
			ot_pose.force_braces = true;
			ot_pose.add_string("Pose");
			ot_pose.add_child("Count").add_int32(1);
		}

		if (deformer_count > 0) {
			FbxNode &ot_def = defs.add_child("ObjectType");
			ot_def.force_braces = true;
			ot_def.add_string("Deformer");
			ot_def.add_child("Count").add_int32(deformer_count);
		}
	}

	Vector<int64_t> geometry_ids;
	Vector<int64_t> model_ids;
	geometry_ids.resize(meshes.size());
	model_ids.resize(meshes.size());
	for (int i = 0; i < meshes.size(); i++) {
		geometry_ids.write[i] = next_id++;
		model_ids.write[i] = next_id++;
	}
	Vector<int64_t> bone_model_ids;
	Vector<int64_t> bone_attr_ids;
	bone_model_ids.resize(bones.size());
	bone_attr_ids.resize(bones.size());
	for (int i = 0; i < bones.size(); i++) {
		bone_model_ids.write[i] = next_id++;
		bone_attr_ids.write[i] = next_id++;
	}

	{
		FbxNode &objects = file_root.add_child("Objects");
		objects.force_braces = true;

		// Bones written before meshes, matching real Maya-authored files' own Objects ordering
		// (joints before geometry, mirroring their own scene-creation order) - structural fidelity.
		for (int b = 0; b < bones.size(); b++) {
			{
				// Every bone gets its own NodeAttribute regardless of whether a skinCluster actually
				// references it (relying only on Definitions' FbxSkeleton PropertyTemplate default
				// makes Maya's importer treat unskinned/helper joints as plain transforms instead of
				// joint nodes, splitting one skeleton across two Maya node types). "Size" plus the
				// trailing TypeFlags("Skeleton") - not Color/LimbLength - matches a real Maya-exported
				// joint's NodeAttribute byte-for-byte (test_project/maya_export_reference2.py's dump);
				// TypeFlags in particular is how the FBX SDK recognizes this as a skeleton joint
				// rather than a generic null/locator.
				FbxNode &attr = objects.add_child("NodeAttribute");
				attr.force_braces = true;
				attr.add_int64(bone_attr_ids[b]);
				attr.add_type_and_name("NodeAttribute", "");
				attr.add_string("LimbNode");
				FbxNode &attr_props = attr.add_child("Properties70");
				attr_props.force_braces = true;
				attr_props.add_property("Size", "double", "Number", "").add_double(100.0);
				attr.add_child("TypeFlags").add_string("Skeleton");
			}
			{
				FbxNode &model = objects.add_child("Model");
				model.force_braces = true;
				model.add_int64(bone_model_ids[b]);
				model.add_type_and_name("Model", bones[b].name);
				model.add_string("LimbNode");
				model.add_child("Version").add_int32(232);
				add_transform_properties(model, bones[b].local);
				model.add_child("Shading").add_bool(true);
				model.add_child("Culling").add_string("CullingOff");
			}
			connect_oo(conns, bone_attr_ids[b], bone_model_ids[b]);
			int64_t parent_id = bones[b].parent_index >= 0 ? bone_model_ids[bones[b].parent_index] : 0;
			connect_oo(conns, bone_model_ids[b], parent_id);
		}

		for (int i = 0; i < meshes.size(); i++) {
			const WriterMesh &mesh = meshes[i];

			{
				FbxNode &geom = objects.add_child("Geometry");
				geom.force_braces = true;
				geom.add_int64(geometry_ids[i]);
				geom.add_type_and_name("Geometry", mesh.name);
				geom.add_string("Mesh");

				Vector<double> verts_flat;
				verts_flat.resize(mesh.vertices.size() * 3);
				for (int v = 0; v < mesh.vertices.size(); v++) {
					Vector3 p = mesh.vertices[v];
					verts_flat.write[v * 3] = p.x;
					verts_flat.write[v * 3 + 1] = p.y;
					verts_flat.write[v * 3 + 2] = p.z;
				}
				geom.add_child("Vertices").add_double_array(verts_flat);

				Vector<int32_t> poly_idx;
				poly_idx.resize(mesh.indices.size());
				int corner = 0;
				for (int f = 0; f < mesh.faces.size(); f++) {
					int count = mesh.faces[f];
					for (int c = 0; c < count; c++) {
						int32_t vi = mesh.indices[corner + c];
						poly_idx.write[corner + c] = (c == count - 1) ? ~vi : vi;
					}
					corner += count;
				}
				geom.add_child("PolygonVertexIndex").add_int32_array(poly_idx);

				geom.add_child("GeometryVersion").add_int32(124);

				// Per-corner (ByPolygonVertex), not per-control-point (ByVertice): both are spec-
				// legal and ufbx reads either, but every real exporter (Maya included) always
				// emits per-corner normals - ByVertice is a rarer mapping mode real-world readers
				// exercise far less.
				Dictionary default_normals = compute_default_normals(mesh.vertices, mesh.indices, mesh.faces);
				Vector<double> normal_flat;
				normal_flat.resize(mesh.indices.size() * 3);
				// mesh.normals[v] is an Array of one normal per corner that touched control point v
				// at read time (see read_normal_data() in fbx_convert.cpp), in the order those
				// corners were encountered walking the source mesh's faces. mesh.indices/mesh.faces
				// below were captured from that exact same corner traversal (ufbx shares one index
				// space across every vertex attribute), so re-walking them here in order and taking
				// the Nth occurrence of v as the Nth entry of mesh.normals[v] reproduces the
				// original per-corner (hard-edge/split-normal) assignment without needing to match
				// on face id.
				Dictionary point_occurrence;
				for (int corner = 0; corner < mesh.indices.size(); corner++) {
					int v = mesh.indices[corner];
					Vector3 n;
					bool used_override = false;
					if (mesh.has_normal_override && mesh.normals.has(v)) {
						Array corner_normals = mesh.normals[v];
						int occurrence = point_occurrence.has(v) ? (int)point_occurrence[v] : 0;
						point_occurrence[v] = occurrence + 1;
						if (occurrence < corner_normals.size()) {
							Vector3 given = corner_normals[occurrence];
							n = given.length_squared() > 0.0 ? given.normalized() : Vector3(0, 1, 0);
							used_override = true;
						}
					}
					if (!used_override && v >= 0 && v < mesh.vertices.size()) {
						n = default_normals[v];
					}
					normal_flat.write[corner * 3] = n.x;
					normal_flat.write[corner * 3 + 1] = n.y;
					normal_flat.write[corner * 3 + 2] = n.z;
				}
				{
					FbxNode &le = geom.add_child("LayerElementNormal");
					le.force_braces = true;
					le.add_int32(0);
					le.add_child("Version").add_int32(102);
					le.add_child("Name").add_string("");
					le.add_child("MappingInformationType").add_string("ByPolygonVertex");
					le.add_child("ReferenceInformationType").add_string("Direct");
					le.add_child("Normals").add_double_array(normal_flat);
				}

				if (mesh.has_uv) {
					Array uv_verts = mesh.uv_vertices;
					Vector<double> uv_flat;
					uv_flat.resize(uv_verts.size() * 2);
					for (int k = 0; k < uv_verts.size(); k++) {
						Variant item = uv_verts[k];
						double u = 0.0, v = 0.0;
						if (item.get_type() == Variant::VECTOR2) {
							Vector2 p = item;
							u = p.x;
							v = p.y;
						} else if (item.get_type() == Variant::ARRAY) {
							Array pair = item;
							if (pair.size() > 0) {
								u = (double)pair[0];
							}
							if (pair.size() > 1) {
								v = (double)pair[1];
							}
						}
						uv_flat.write[k * 2] = u;
						uv_flat.write[k * 2 + 1] = v;
					}

					Array uv_indices = mesh.uv_indices;
					Vector<int32_t> uv_idx;
					for (int f = 0; f < uv_indices.size(); f++) {
						Array face_idx = uv_indices[f];
						for (int c = 0; c < face_idx.size(); c++) {
							uv_idx.push_back((int32_t)(int64_t)face_idx[c]);
						}
					}

					FbxNode &le = geom.add_child("LayerElementUV");
					le.force_braces = true;
					le.add_int32(0);
					le.add_child("Version").add_int32(101);
					le.add_child("Name").add_string("");
					le.add_child("MappingInformationType").add_string("ByPolygonVertex");
					le.add_child("ReferenceInformationType").add_string("IndexToDirect");
					le.add_child("UV").add_double_array(uv_flat);
					le.add_child("UVIndex").add_int32_array(uv_idx);
				}

				{
					Vector<int32_t> poly_material;
					poly_material.resize(mesh.faces.size());
					for (int f = 0; f < poly_material.size(); f++) {
						poly_material.write[f] = 0;
					}
					Array fmap_keys = mesh.face_id_map.keys();
					for (int k = 0; k < fmap_keys.size(); k++) {
						int mat_idx = (int)(int64_t)fmap_keys[k];
						Array polys = mesh.face_id_map[fmap_keys[k]];
						for (int p = 0; p < polys.size(); p++) {
							int face_i = (int)(int64_t)polys[p];
							if (face_i >= 0 && face_i < poly_material.size()) {
								poly_material.write[face_i] = mat_idx;
							}
						}
					}

					FbxNode &le = geom.add_child("LayerElementMaterial");
					le.force_braces = true;
					le.add_int32(0);
					le.add_child("Version").add_int32(101);
					le.add_child("Name").add_string("");
					le.add_child("MappingInformationType").add_string("ByPolygon");
					le.add_child("ReferenceInformationType").add_string("IndexToDirect");
					le.add_child("Materials").add_int32_array(poly_material);
				}

				{
					FbxNode &layer = geom.add_child("Layer");
					layer.force_braces = true;
					layer.add_int32(0);
					layer.add_child("Version").add_int32(100);
					{
						FbxNode &le = layer.add_child("LayerElement");
						le.force_braces = true;
						le.add_child("Type").add_string("LayerElementNormal");
						le.add_child("TypedIndex").add_int32(0);
					}
					if (mesh.has_uv) {
						FbxNode &le = layer.add_child("LayerElement");
						le.force_braces = true;
						le.add_child("Type").add_string("LayerElementUV");
						le.add_child("TypedIndex").add_int32(0);
					}
					{
						FbxNode &le = layer.add_child("LayerElement");
						le.force_braces = true;
						le.add_child("Type").add_string("LayerElementMaterial");
						le.add_child("TypedIndex").add_int32(0);
					}
				}
			}

			{
				FbxNode &model = objects.add_child("Model");
				model.force_braces = true;
				model.add_int64(model_ids[i]);
				model.add_type_and_name("Model", mesh.name);
				model.add_string("Mesh");
				model.add_child("Version").add_int32(232);
				add_transform_properties(model, mesh.matrix);
				model.add_child("Shading").add_bool(true);
				model.add_child("Culling").add_string("CullingOff");
			}
			connect_oo(conns, model_ids[i], 0);
			connect_oo(conns, geometry_ids[i], model_ids[i]);

			for (int m = 0; m < mesh.materials.size(); m++) {
				String mat_name = mesh.materials[m];
				int64_t material_id = next_id++;
				{
					FbxNode &mat = objects.add_child("Material");
					mat.force_braces = true;
					mat.add_int64(material_id);
					mat.add_type_and_name("Material", mat_name);
					mat.add_string("");
					mat.add_child("Version").add_int32(102);
					mat.add_child("ShadingModel").add_string("phong");
					mat.add_child("MultiLayer").add_int32(0);
					FbxNode &props = mat.add_child("Properties70");
					props.force_braces = true;
					FbxNode &dc = props.add_property("DiffuseColor", "Color", "", "A");
					dc.add_double(1.0);
					dc.add_double(1.0);
					dc.add_double(1.0);
				}
				connect_oo(conns, material_id, model_ids[i]);

				Dictionary paths = mesh.material_paths.has(mat_name) ? (Dictionary)mesh.material_paths[mat_name] : Dictionary();
				Array path_keys = paths.keys();
				for (int pk = 0; pk < path_keys.size(); pk++) {
					String prop_name = path_keys[pk];
					String path = paths[path_keys[pk]];
					int64_t texture_id = next_id++;
					{
						FbxNode &tex = objects.add_child("Texture");
						tex.force_braces = true;
						tex.add_int64(texture_id);
						tex.add_type_and_name("Texture", mat_name + String("_") + prop_name);
						tex.add_string("");
						tex.add_child("Type").add_string("TextureVideoClip");
						tex.add_child("Version").add_int32(202);
						tex.add_child("TextureName").add_string("Texture::" + mat_name + String("_") + prop_name);
						tex.add_child("FileName").add_string(path);
						tex.add_child("RelativeFilename").add_string(path);
					}
					connect_oo(conns, texture_id, material_id);
					connect_op(conns, texture_id, material_id, prop_name);
				}
			}
		}

		bool any_skin = false;
		Vector<int> pose_mesh_indices;
		Vector<int> pose_bone_indices;

		// Precompute which meshes/bones actually contribute a skin (i.e. have at least one
		// non-zero weight - same rule the write pass below applies) so the Pose (BindPose) block
		// can be written before any Deformer/SubDeformer object, matching real Maya-authored files'
		// own Objects/Definitions ordering - structural fidelity only.
		for (int i = 0; i < meshes.size(); i++) {
			const WriterMesh &mesh = meshes[i];
			if (mesh.skin.is_empty()) {
				continue;
			}
			bool mesh_contributes = false;
			Array bone_names = mesh.skin.keys();
			for (int bn = 0; bn < bone_names.size(); bn++) {
				String bone_name = bone_names[bn];
				int bone_index = -1;
				for (int b = 0; b < bones.size(); b++) {
					if (bones[b].name == bone_name) {
						bone_index = b;
						break;
					}
				}
				if (bone_index < 0) {
					continue;
				}
				PackedFloat32Array weights = mesh.skin[bone_name];
				int vcount = weights.size() < mesh.vertices.size() ? weights.size() : mesh.vertices.size();
				bool has_weight = false;
				for (int v = 0; v < vcount; v++) {
					if (weights[v] != 0.0f) {
						has_weight = true;
						break;
					}
				}
				if (!has_weight) {
					continue;
				}
				mesh_contributes = true;
				if (!pose_bone_indices.has(bone_index)) {
					pose_bone_indices.push_back(bone_index);
				}
			}
			if (mesh_contributes) {
				any_skin = true;
				pose_mesh_indices.push_back(i);
			}
		}

		if (any_skin) {
			FbxNode &pose = objects.add_child("Pose");
			pose.force_braces = true;
			pose.add_int64(next_id++);
			pose.add_type_and_name("Pose", "BindPose");
			pose.add_string("BindPose");
			pose.add_child("Type").add_string("BindPose");
			pose.add_child("Version").add_int32(100);
			pose.add_child("NbPoseNodes").add_int32(pose_mesh_indices.size() + pose_bone_indices.size());
			for (int k = 0; k < pose_mesh_indices.size(); k++) {
				int mi = pose_mesh_indices[k];
				FbxNode &pn = pose.add_child("PoseNode");
				pn.force_braces = true;
				pn.add_child("Node").add_int64(model_ids[mi]);
				pn.add_child("Matrix").add_double_array(transform3d_to_flat16(meshes[mi].matrix));
			}
			for (int k = 0; k < pose_bone_indices.size(); k++) {
				int bi = pose_bone_indices[k];
				FbxNode &pn = pose.add_child("PoseNode");
				pn.force_braces = true;
				pn.add_child("Node").add_int64(bone_model_ids[bi]);
				pn.add_child("Matrix").add_double_array(transform3d_to_flat16(bones[bi].world));
			}
		}

		// Otherwise-pure-boilerplate AnimationStack/AnimationLayer/AnimationCurveNode scaffolding
		// (no real keyframes) matching what a real Maya skinCluster export always includes - see
		// the matching Definitions/ObjectType entries above.
		if (any_skin) {
			int64_t stack_id = next_id++;
			{
				FbxNode &stack = objects.add_child("AnimationStack");
				stack.force_braces = true;
				stack.add_int64(stack_id);
				stack.add_type_and_name("AnimStack", "Take 001");
				stack.add_string("");
				FbxNode &props = stack.add_child("Properties70");
				props.force_braces = true;
				add_tprop_str(props, "Description", "KString", "", "", "");
				add_tprop1_i64(props, "LocalStart", "KTime", "Time", "", 0);
				add_tprop1_i64(props, "LocalStop", "KTime", "Time", "", 0);
				add_tprop1_i64(props, "ReferenceStart", "KTime", "Time", "", 0);
				add_tprop1_i64(props, "ReferenceStop", "KTime", "Time", "", 0);
			}

			int64_t layer_id = next_id++;
			{
				FbxNode &layer = objects.add_child("AnimationLayer");
				layer.force_braces = true;
				layer.add_int64(layer_id);
				layer.add_type_and_name("AnimLayer", "BaseLayer");
				layer.add_string("");
			}
			connect_oo(conns, layer_id, stack_id);

			for (int k = 0; k < pose_bone_indices.size(); k++) {
				int bi = pose_bone_indices[k];
				int64_t curve_node_id = next_id++;
				{
					FbxNode &curve = objects.add_child("AnimationCurveNode");
					curve.force_braces = true;
					curve.add_int64(curve_node_id);
					curve.add_type_and_name("AnimCurveNode", "lockInfluenceWeights");
					curve.add_string("");
					FbxNode &props = curve.add_child("Properties70");
					props.force_braces = true;
					props.add_property("d|lockInfluenceWeights", "Bool", "", "A").add_bool(false);
				}
				connect_oo(conns, curve_node_id, layer_id);
				connect_op(conns, curve_node_id, bone_model_ids[bi], "lockInfluenceWeights");
			}
		}

		for (int i = 0; i < meshes.size(); i++) {
			const WriterMesh &mesh = meshes[i];
			if (mesh.skin.is_empty()) {
				continue;
			}

			int64_t deformer_id = next_id++;
			{
				FbxNode &def = objects.add_child("Deformer");
				def.force_braces = true;
				def.add_int64(deformer_id);
				def.add_type_and_name("Deformer", "");
				def.add_string("Skin");
				def.add_child("Version").add_int32(101);
				def.add_child("Link_DeformAcuracy").add_double(50.0);
				// Present on every real Maya-authored Deformer:Skin block (test_project/
				// maya_export_reference2.py's own output) - matched for structural fidelity.
				def.add_child("SkinningType").add_string("Linear");
			}
			connect_oo(conns, deformer_id, geometry_ids[i]);

			Array bone_names = mesh.skin.keys();
			for (int bn = 0; bn < bone_names.size(); bn++) {
				String bone_name = bone_names[bn];
				int bone_index = -1;
				for (int b = 0; b < bones.size(); b++) {
					if (bones[b].name == bone_name) {
						bone_index = b;
						break;
					}
				}
				if (bone_index < 0) {
					UtilityFunctions::printerr("FbxSceneWriter: export_scene() skin bone \"", bone_name, "\" no longer resolves against the current skeleton - skipped");
					continue;
				}

				PackedFloat32Array weights = mesh.skin[bone_name];
				int vcount = weights.size() < mesh.vertices.size() ? weights.size() : mesh.vertices.size();
				Vector<int32_t> idxs;
				Vector<double> wts;
				for (int v = 0; v < vcount; v++) {
					float w = weights[v];
					if (w != 0.0f) {
						idxs.push_back(v);
						wts.push_back((double)w);
					}
				}
				if (idxs.is_empty()) {
					continue;
				}

				int64_t subdeformer_id = next_id++;
				{
					// ufbx dispatches purely on the block keyword + sub-type string (see the
					// `name == ufbxi_Deformer` check in ufbx.c's object reader): a Cluster is
					// still a "Deformer:" block, just with sub-type "Cluster" instead of "Skin" -
					// there is no separate "SubDeformer:" keyword despite the object's own
					// "SubDeformer::" display name below.
					FbxNode &sub = objects.add_child("Deformer");
					sub.force_braces = true;
					sub.add_int64(subdeformer_id);
					sub.add_type_and_name("SubDeformer", "");
					sub.add_string("Cluster");
					sub.add_child("Version").add_int32(100);
					{
						FbxNode &ud = sub.add_child("UserData");
						ud.add_string("");
						ud.add_string("");
					}
					sub.add_child("Indexes").add_int32_array(idxs);
					sub.add_child("Weights").add_double_array(wts);
					// Transform is joint-relative, not mesh-global: the mesh's bind-time world
					// transform expressed relative to *this specific influence's* own bind-time
					// world transform, i.e. inverse(TransformLink) * mesh-world - and so differs per
					// influence (confirmed by decoding a real Maya-exported two-influence skinCluster:
					// its root joint's Transform was the mesh's own world offset, but its child
					// joint's Transform - sitting exactly at the mesh's position - was identity).
					// Writing the mesh's world matrix verbatim here (the same value for every
					// influence) is what was making Maya's own FBX SDK reader reconstruct every
					// influence's skinCluster.bindPreMatrix as identity instead of TransformLink's
					// inverse - the mesh deforms incorrectly the instant the skinCluster is enabled,
					// but looks completely fine static/unskinned, since bindPreMatrix is only
					// consulted during skin evaluation. ufbx-based readers (this project's own
					// FbxScene/FbxMeshEntry, and therefore the Godot import path) never derive
					// anything from Transform in the first place, so they never showed this.
					sub.add_child("Transform").add_double_array(transform3d_to_flat16(bones[bone_index].world.affine_inverse() * mesh.matrix));
					sub.add_child("TransformLink").add_double_array(transform3d_to_flat16(bones[bone_index].world));
				}
				connect_oo(conns, subdeformer_id, deformer_id);
				connect_oo(conns, bone_model_ids[bone_index], subdeformer_id);
			}
		}
	}

	{
		FbxNode &connections = file_root.add_child("Connections");
		connections.force_braces = true;
		for (int i = 0; i < conns.size(); i++) {
			const PendingConn &pc = conns[i];
			FbxNode &c = connections.add_child("C");
			c.add_string(pc.type);
			c.add_int64(pc.child);
			c.add_int64(pc.parent);
			if (pc.type == "OP") {
				c.add_string(pc.prop);
			}
		}
	}

	{
		FbxNode &takes = file_root.add_child("Takes");
		takes.force_braces = true;
		takes.add_child("Current").add_string("Take 001");
		FbxNode &take = takes.add_child("Take");
		take.force_braces = true;
		take.add_string("Take 001");
		take.add_child("FileName").add_string("Take_001.tak");
		{
			FbxNode &lt = take.add_child("LocalTime");
			lt.add_int64(0);
			lt.add_int64(0);
		}
		{
			FbxNode &rt = take.add_child("ReferenceTime");
			rt.add_int64(0);
			rt.add_int64(0);
		}
	}

	if (p_binary) {
		file->store_buffer(fbx_binary_serialize(file_root, 7400));
	} else {
		file->store_string(fbx_ascii_serialize_children(file_root));
	}
	return true;
}

void FbxSceneWriter::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_mesh", "geometry"), &FbxSceneWriter::set_mesh);
	ClassDB::bind_method(D_METHOD("set_uv", "mesh_index", "uv"), &FbxSceneWriter::set_uv);
	ClassDB::bind_method(D_METHOD("set_normal", "mesh_index", "normal"), &FbxSceneWriter::set_normal);
	ClassDB::bind_method(D_METHOD("set_material", "mesh_index", "material"), &FbxSceneWriter::set_material);
	ClassDB::bind_method(D_METHOD("set_skeleton", "skeleton"), &FbxSceneWriter::set_skeleton);
	ClassDB::bind_method(D_METHOD("set_skin", "mesh_index", "skin"), &FbxSceneWriter::set_skin);
	ClassDB::bind_method(D_METHOD("export_scene", "path", "binary"), &FbxSceneWriter::export_scene, DEFVAL(true));
}
