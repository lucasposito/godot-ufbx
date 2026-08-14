extends Node2D

const JOE_MESH_PATH := "D://Monjolo Project//Source//Characters//Joe//Meshes//SKM_BV_Joe_01.fbx"
const JOE_RUN_ANIM_PATH := "D://Monjolo Project//Source//Characters//Joe//Animations//Joe_Run_01.fbx"

func _ready() -> void:
	##### TEST ZONE #####

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
