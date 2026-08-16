extends SceneTree

# Exercises FbxSceneWriter against real production data instead of the small hand-built quads in
# main.gd's _test_export_roundtrip(): reads the same JSON shapes FbxMeshEntry::geometry() /
# skeleton() / skin() already produce (see fbx_convert.hpp's read_mesh_elements /
# read_skeleton_data / read_skin_data) from Joe's exported data directory, feeds them straight
# into set_mesh()/set_skeleton()/set_skin(), writes a fresh .fbx into that same directory, then
# reloads it through the normal read path to confirm the round-trip.
#
# Run with: godot --headless --script res://export_joe_data_test.gd

const DATA_DIR := "D://Monjolo Project//Source//Characters//Joe//Data"
const GEOMETRY_PATH := DATA_DIR + "//Geometry//SKM_BV_Joe_01_Jacket.json"
const SKELETON_PATH := DATA_DIR + "//Skeleton//SKM_BV_Joe.json"
const SKIN_PATH := DATA_DIR + "//Skins//SKM_BV_Joe_01_Jacket.json"
const OUTPUT_PATH := DATA_DIR + "//SKM_BV_Joe_01_Jacket_export_test.fbx"

func _initialize() -> void:
	var geometry_json = _load_json(GEOMETRY_PATH)
	var skeleton_json = _load_json(SKELETON_PATH)
	var skin_json = _load_json(SKIN_PATH)
	if geometry_json == null or skeleton_json == null or skin_json == null:
		quit(1)
		return

	# geometry.py's "vertices" is an array of [x, y, z] triples and "matrix" a flat 16-float
	# Maya-style array (see matrix_to_maya_array() in fbx_convert.cpp) - both need converting to
	# the PackedVector3Array/Transform3D types set_mesh() expects; "indices"/"faces" are already
	# flat number arrays, which set_mesh() accepts as plain Arrays directly.
	var geometry := {
		"name": geometry_json.get("name", "Mesh"),
		"vertices": _to_vector3_array(geometry_json["vertices"]),
		"indices": geometry_json["indices"],
		"faces": geometry_json["faces"],
		"matrix": _maya_array_to_transform(geometry_json["matrix"]),
	}

	var writer := FbxManager.new().new_scene()
	var mesh_index: int = writer.set_mesh(geometry)
	if mesh_index < 0:
		print("FAILED: set_mesh() rejected the geometry data")
		quit(1)
		return
	print("set_mesh: vertices=", geometry.vertices.size(), " faces=", (geometry.faces as Array).size())

	# skeleton.json's Leaf shape matches FbxMeshEntry::skeleton() exactly, including nested Leafs
	# keyed by a hashed id - set_skeleton() accepts it as-is (it matches nested Leafs by field
	# name, not key type, specifically so this works whether the Dictionary came from GDScript
	# with real int keys or from JSON, where every key - metadata and nested-Leaf hash alike - is
	# a String).
	writer.set_skeleton(skeleton_json)
	print("set_skeleton: source joints listed=", (skeleton_json.get("joints", []) as Array).size())

	# skin.json is already bone_name -> per-vertex weight array, exactly skin() 's shape.
	writer.set_skin(mesh_index, skin_json)
	print("set_skin: bones=", (skin_json as Dictionary).size())

	var ok: bool = writer.export_scene(OUTPUT_PATH)
	print("export_scene(\"", OUTPUT_PATH, "\") ok=", ok)
	if not ok:
		quit(1)
		return

	# Round-trip: reload the file we just wrote through the normal read path and confirm the
	# counts line up, same sanity check _test_export_roundtrip() does in main.gd.
	var reloaded := FbxManager.new().load_scene(OUTPUT_PATH)
	if not reloaded:
		print("FAILED to reload the exported file")
		quit(1)
		return

	print("reloaded mesh_count=", reloaded.get_mesh_count())
	var entry := reloaded.get_mesh(0)
	var geo := entry.geometry()
	print("reloaded vertex count=", (geo.get("vertices", []) as PackedVector3Array).size(),
			" (expected ", geometry.vertices.size(), ")")
	print("reloaded face count=", (geo.get("faces", []) as PackedInt32Array).size(),
			" (expected ", (geometry.faces as Array).size(), ")")

	reloaded.load_skeleton()
	# Only bones this mesh's skin actually weights against round-trip as Skeleton3D bones - same
	# "referenced by a skin cluster, or has its own NodeAttribute" rule FbxScene::load_skeleton()
	# already applies when reading a real FBX (see gather_bone_nodes() in fbx_convert.cpp), which
	# is why this is <= the full source rig's joint count, not equal to it: helper/control joints
	# with no vertices weighted to them (of which this 135-joint rig has plenty) are expected to
	# be composed through rather than becoming their own bone.
	print("reloaded has_skeleton=", reloaded.has_skeleton(),
			" bone_count=", reloaded.get_skeleton().get_bone_count() if reloaded.has_skeleton() else -1,
			" (source rig has ", (skeleton_json.get("joints", []) as Array).size(), " joints total, ",
			(skin_json as Dictionary).size(), " of which this mesh's skin weights against)")

	entry.load_geometry()
	entry.load_skin()
	print("reloaded is_skin_loaded=", entry.is_skin_loaded())
	var skin := entry.skin()
	print("reloaded skin bone count=", skin.size(), " (expected <= ", (skin_json as Dictionary).size(), ")")

	quit(0)


func _load_json(path: String) -> Variant:
	var f := FileAccess.open(path, FileAccess.READ)
	if not f:
		print("FAILED to open ", path, ": ", FileAccess.get_open_error())
		return null
	var text := f.get_as_text()
	var data = JSON.parse_string(text)
	if data == null:
		print("FAILED to parse JSON: ", path)
		return null
	return data


func _to_vector3_array(raw: Array) -> PackedVector3Array:
	var out := PackedVector3Array()
	out.resize(raw.size())
	for i in raw.size():
		var v: Array = raw[i]
		out[i] = Vector3(v[0], v[1], v[2])
	return out


# Inverse of main.gd's _transform_to_maya_array(): a flat 16-float Maya-style row-major array
# [Xx,Xy,Xz,0, Yx,Yy,Yz,0, Zx,Zy,Zz,0, Tx,Ty,Tz,1] back into a Transform3D.
func _maya_array_to_transform(arr: Array) -> Transform3D:
	var x := Vector3(arr[0], arr[1], arr[2])
	var y := Vector3(arr[4], arr[5], arr[6])
	var z := Vector3(arr[8], arr[9], arr[10])
	var o := Vector3(arr[12], arr[13], arr[14])
	return Transform3D(Basis(x, y, z), o)
