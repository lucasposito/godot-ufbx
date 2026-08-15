extends SceneTree

const JOE_MESH_PATH := "D://Monjolo Project//Source//Characters//Joe//Meshes//SKM_BV_Joe_01.fbx"
const JOE_RUN_ANIM_PATH := "D://Monjolo Project//Source//Characters//Joe//Animations//Joe_Run_02.fbx"

func _initialize() -> void:
	var fbx := FbxManager.new()
	var scene := fbx.load_scene(JOE_MESH_PATH)
	if not scene:
		print("FAILED to load mesh scene")
		quit(1)
		return

	scene.load_skeleton()
	print("has_skeleton: ", scene.has_skeleton())
	if not scene.has_skeleton():
		quit(1)
		return
	print("bone_count: ", scene.get_skeleton().get_bone_count())

	for mesh_entry in scene.get_meshes().values():
		mesh_entry.load_geometry()
		mesh_entry.load_material()
		mesh_entry.load_skin()

	var character := scene.import()
	root.add_child(character)

	var fbx2 := FbxManager.new()
	var anim_scene := fbx2.load_scene(JOE_RUN_ANIM_PATH)
	if not anim_scene:
		print("FAILED to load anim scene")
		quit(1)
		return

	print("animations: ", anim_scene.get_animation_names())
	var anim := anim_scene.load_animation(0, 0.0, scene.get_skeleton())
	anim.loop_mode = Animation.LOOP_LINEAR

	var library := AnimationLibrary.new()
	library.add_animation("Run", anim)

	var player := AnimationPlayer.new()
	player.name = "AnimationPlayer"
	character.add_child(player)
	player.add_animation_library("", library)
	player.play("Run")

	var sample_time: float = 0.3 * anim.length
	player.seek(sample_time, true)
	print("sample_time: ", sample_time, " anim.length: ", anim.length)

	var skeleton: Skeleton3D = scene.get_skeleton()
	var bone_names := ["root", "pelvis", "spine_01", "spine_05", "clavicle_l", "upperarm_l", "lowerarm_l", "hand_l", "thigh_l", "calf_l", "foot_l", "head"]
	print("character.transform: ", character.transform)
	for bn in bone_names:
		var idx := skeleton.find_bone(bn)
		if idx < 0:
			print("  ", bn, ": NOT FOUND")
			continue
		var gp: Transform3D = skeleton.get_bone_global_pose(idx)
		var world_pos: Vector3 = character.transform * gp.origin
		print("  ", bn, " idx=", idx, " skeleton_local_pose_origin=", gp.origin, " world_pos=", world_pos)

	quit(0)
