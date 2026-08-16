extends Node2D

const JOE_MESH_PATH := "D://Monjolo Project//Source//Characters//Joe//Meshes//SKM_BV_Joe_01.fbx"
const JOE_RUN_ANIM_PATH := "D://Monjolo Project//Source//Characters//Joe//Animations//Joe_Run_01.fbx"

func _ready() -> void:
	##### TEST ZONE #####

	_test_export_roundtrip()

	var fbx := FbxManager.new()
	var scene := fbx.load_scene(JOE_MESH_PATH)
	if not scene:
		return

	print("meshes: ", scene.get_mesh_names())

	# load_skeleton() must run before any load_skin() calls below, since load_skin() resolves
	# each cluster's bone through the Skeleton3D this builds.
	scene.load_skeleton()
	print("has_skeleton: ", scene.has_skeleton())
	if scene.has_skeleton():
		print("bone_count: ", scene.get_skeleton().get_bone_count())

	# Select which meshes to actually build - here, everything. Skip entries in this
	# loop (or only call load_geometry() without load_material(), etc.) to import a subset.
	for mesh_entry in scene.get_meshes().values():
		mesh_entry.load_geometry()
		mesh_entry.load_material()
		_fix_material_culling(mesh_entry)
		mesh_entry.load_skin() # no-op if the mesh isn't skinned

	var character := scene.import()
	add_child(character)

	_load_and_play_animation(scene, character, JOE_RUN_ANIM_PATH)


# Joe's animations ship as separate animation-only FBX files (Mixamo-style "without skin"
# export) rather than baked into the mesh file, so this loads a second FbxScene just for the
# bone tracks and plays them against the Skeleton3D built above - the two files only need to
# agree on bone names, not live in the same FBX. `mesh_scene`'s Skeleton3D is passed into
# load_animation() so it can tell a real skinned bone from an unused rig control bone (e.g. the
# "root" bone some rigs put above the topmost skinned bone) when baking track transforms.
func _load_and_play_animation(mesh_scene: FbxScene, character: Node3D, anim_path: String) -> void:
	var fbx := FbxManager.new()
	var anim_scene := fbx.load_scene(anim_path)
	if not anim_scene:
		print("failed to load animation: ", anim_path)
		return

	print("animations: ", anim_scene.get_animation_names())
	if anim_scene.get_animation_count() == 0:
		return

	var anim := anim_scene.load_animation(0, 0.0, mesh_scene.get_skeleton())
	anim.loop_mode = Animation.LOOP_LINEAR

	var library := AnimationLibrary.new()
	library.add_animation("Run", anim)

	var player := AnimationPlayer.new()
	player.name = "AnimationPlayer"
	# Added under `character` (the Skeleton3D's parent) so the animation's "Skeleton3D:<bone>"
	# track paths resolve against player.get_node("..") like FbxScene::import() built them for.
	character.add_child(player)
	player.add_animation_library("", library)
	player.play("Run")
#
#
# This FBX's winding order comes in reversed relative to what Godot's default
# back-face culling expects, so surfaces render inside-out unless culling is
# disabled on every material load_material() produces.
func _fix_material_culling(mesh_entry: FbxMeshEntry) -> void:
	var mesh := mesh_entry.get_loaded_mesh()
	if not mesh:
		return
	for surface_idx in mesh.get_surface_count():
		var mat := mesh.surface_get_material(surface_idx)
		if mat is BaseMaterial3D:
			mat.cull_mode = BaseMaterial3D.CULL_DISABLED


func _apply_texture(node: Node, texture_path: String) -> void:
	var image := Image.load_from_file(texture_path)
	if not image:
		print("failed to load texture: ", texture_path)
		return
	var texture := ImageTexture.create_from_image(image)
	_apply_texture_recursive(node, texture)


func _apply_texture_recursive(node: Node, texture: Texture2D) -> void:
	if node is MeshInstance3D:
		var mesh_instance := node as MeshInstance3D
		var mesh := mesh_instance.mesh
		if mesh:
			for surface_idx in mesh.get_surface_count():
				var mat := StandardMaterial3D.new()
				mat.albedo_texture = texture
				mat.cull_mode = BaseMaterial3D.CULL_DISABLED
				mat.roughness = 0.55
				mat.metallic = 0.0
				mat.metallic_specular = 0.4
				mesh_instance.set_surface_override_material(surface_idx, mat)
	for child in node.get_children():
		_apply_texture_recursive(child, texture)


# Smoke test for FbxSceneWriter (new_scene/set_mesh/set_uv/set_normal/set_material/
# set_skeleton/set_skin/export_scene): builds a scene entirely in memory, writes it to a fresh
# .fbx file, then loads that same file back through FbxManager.load_scene() (the normal read
# path) and prints what round-tripped so mismatches are obvious by eye in the log.
func _test_export_roundtrip() -> void:
	print("EXPORT_TEST: ---- pass 1: minimal set_mesh only ----")
	var out_path := "res://tmp_export_test.fbx"

	var writer := FbxManager.new().new_scene()
	var quad_vertices := PackedVector3Array([
		Vector3(0, 0, 0), Vector3(1, 0, 0), Vector3(1, 1, 0), Vector3(0, 1, 0),
	])
	var mesh_index := writer.set_mesh({
		"name": "Quad",
		"vertices": quad_vertices,
		"indices": PackedInt32Array([0, 1, 2, 3]),
		"faces": PackedInt32Array([4]),
	})
	print("EXPORT_TEST: set_mesh returned index ", mesh_index)
	var ok := writer.export_scene(out_path)
	print("EXPORT_TEST: export_scene ok=", ok)

	var reloaded := FbxManager.new().load_scene(out_path)
	if not reloaded:
		print("EXPORT_TEST: FAILED to reload exported file")
		return
	print("EXPORT_TEST: reloaded mesh_count=", reloaded.get_mesh_count(), " (expected 1)")
	var entry := reloaded.get_mesh(0)
	var geo := entry.geometry()
	print("EXPORT_TEST: reloaded vertex count=", (geo.get("vertices", []) as PackedVector3Array).size(), " (expected 4)")
	print("EXPORT_TEST: reloaded face count=", (geo.get("faces", []) as PackedInt32Array).size(), " (expected 1)")

	print("EXPORT_TEST: ---- pass 2: + uv/material/normal ----")
	writer = FbxManager.new().new_scene()
	mesh_index = writer.set_mesh({
		"name": "Quad",
		"vertices": quad_vertices,
		"indices": PackedInt32Array([0, 1, 2, 3]),
		"faces": PackedInt32Array([4]),
	})
	writer.set_uv(mesh_index, {
		"vertices": [[0.0, 0.0], [1.0, 0.0], [1.0, 1.0], [0.0, 1.0]],
		"indices": [[0, 1, 2, 3]],
	})
	writer.set_material(mesh_index, {
		"materials": ["Skin"],
		"paths": {},
		"face_id_map": {0: [0]},
	})
	writer.set_normal(mesh_index, {
		"normals": {0: Vector3(0, 0, 1), 1: Vector3(0, 0, 1), 2: Vector3(0, 0, 1), 3: Vector3(0, 0, 1)},
	})
	ok = writer.export_scene(out_path)
	print("EXPORT_TEST: export_scene ok=", ok)

	reloaded = FbxManager.new().load_scene(out_path)
	if not reloaded:
		print("EXPORT_TEST: FAILED to reload exported file (pass 2)")
		return
	entry = reloaded.get_mesh(0)
	var uv := entry.uv()
	print("EXPORT_TEST: reloaded uv vertex count=", (uv.get("vertices", []) as Array).size(), " (expected 4)")
	var mat := entry.material()
	print("EXPORT_TEST: reloaded materials=", mat.get("materials", []), " (expected [\"Skin\"])")
	var normal := entry.normal()
	var normals_dict: Dictionary = normal.get("normals", {})
	print("EXPORT_TEST: reloaded normal count=", normals_dict.size(), " (expected 4), sample=", normals_dict.get(0, null), " (expected ~(0,0,1))")

	print("EXPORT_TEST: ---- pass 3: + skeleton/skin ----")
	writer = FbxManager.new().new_scene()
	mesh_index = writer.set_mesh({
		"name": "SkinnedQuad",
		"vertices": quad_vertices,
		"indices": PackedInt32Array([0, 1, 2, 3]),
		"faces": PackedInt32Array([4]),
	})

	var child_leaf := {
		"id": 2, "parent": 1, "name": "Bone1",
		"matrix": _transform_to_maya_array(Transform3D(Basis(), Vector3(0, 1, 0))),
		"rotation": [0.0, 0.0, 0.0], "orient": [0.0, 0.0, 0.0],
		"radius": 0.0, "rotateOrder": 0.0, "side": 0.0, "type": 0.0,
	}
	var root_leaf := {
		"id": 1, "parent": 0, "name": "Bone0",
		"matrix": _transform_to_maya_array(Transform3D(Basis(), Vector3(0, 0, 0))),
		"rotation": [0.0, 0.0, 0.0], "orient": [0.0, 0.0, 0.0],
		"radius": 0.0, "rotateOrder": 0.0, "side": 0.0, "type": 0.0,
		2: child_leaf,
	}
	var skeleton_dict := {
		"id": 0, "joints": ["Bone0", "Bone1"],
		1: root_leaf,
	}
	writer.set_skeleton(skeleton_dict)
	writer.set_skin(mesh_index, {
		"Bone0": PackedFloat32Array([1.0, 1.0, 0.0, 0.0]),
		"Bone1": PackedFloat32Array([0.0, 0.0, 1.0, 1.0]),
	})
	ok = writer.export_scene(out_path)
	print("EXPORT_TEST: export_scene ok=", ok)

	reloaded = FbxManager.new().load_scene(out_path)
	if not reloaded:
		print("EXPORT_TEST: FAILED to reload exported file (pass 3)")
		return
	reloaded.load_skeleton()
	var bone_count = reloaded.get_skeleton().get_bone_count() if reloaded.has_skeleton() else -1
	print("EXPORT_TEST: reloaded has_skeleton=", reloaded.has_skeleton(), " bone_count=", bone_count, " (expected true, 2)")
	entry = reloaded.get_mesh(0)
	entry.load_geometry()
	entry.load_skin()
	print("EXPORT_TEST: reloaded is_skin_loaded=", entry.is_skin_loaded(), " (expected true)")
	var skin := entry.skin()
	print("EXPORT_TEST: reloaded skin bones=", skin.keys(), " (expected [Bone0, Bone1] in some order)")
	for bone_name in skin.keys():
		print("EXPORT_TEST:   ", bone_name, " weights=", skin[bone_name])

	var cleanup := DirAccess.open("res://")
	if cleanup:
		cleanup.remove("tmp_export_test.fbx")


func _transform_to_maya_array(p_transform: Transform3D) -> Array:
	var x := p_transform.basis.x
	var y := p_transform.basis.y
	var z := p_transform.basis.z
	var o := p_transform.origin
	return [
		x.x, x.y, x.z, 0.0,
		y.x, y.y, y.z, 0.0,
		z.x, z.y, z.z, 0.0,
		o.x, o.y, o.z, 1.0,
	]
