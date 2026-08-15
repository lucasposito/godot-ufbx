extends SceneTree

const JOE_MESH_PATH := "D://Monjolo Project//Source//Characters//Joe//Meshes//SKM_BV_Joe_01.fbx"

func _initialize() -> void:
	var fbx := FbxManager.new()
	var scene := fbx.load_scene(JOE_MESH_PATH)
	if not scene:
		print("FAILED to load mesh scene")
		quit(1)
		return

	scene.load_skeleton()
	var skeleton: Skeleton3D = scene.get_skeleton()
	print("bone_count: ", skeleton.get_bone_count())

	# Rest-pose global origin per bone, in Skeleton3D-local space (same space skinned
	# MeshInstance3D vertices live in, since both are parented at identity under `character`).
	var bone_rest_pos: Dictionary = {}
	for i in skeleton.get_bone_count():
		bone_rest_pos[skeleton.get_bone_name(i)] = skeleton.get_bone_global_rest(i).origin

	var check_bones := ["upperarm_l", "upperarm_r", "thigh_l", "thigh_r", "lowerarm_l", "hand_l", "neck_01", "neck_02", "head", "calf_l", "foot_l"]

	for mesh_entry in scene.get_meshes().values():
		mesh_entry.load_geometry()
		mesh_entry.load_skin()
		if not mesh_entry.is_skin_loaded():
			continue
		var mesh: ArrayMesh = mesh_entry.get_loaded_mesh()
		if not mesh:
			continue
		print("=== mesh: ", mesh_entry.get_name(), " surfaces=", mesh.get_surface_count(), " ===")

		# Aggregate: for each bone name, sum of vertex positions (dominant-weight vertices only)
		# and count, so we can compare "where this mesh's vertices for bone X actually sit" vs
		# "where bone X's own rest position is".
		var sums: Dictionary = {}
		var counts: Dictionary = {}

		for surf_idx in mesh.get_surface_count():
			var arrays := mesh.surface_get_arrays(surf_idx)
			var verts: PackedVector3Array = arrays[Mesh.ARRAY_VERTEX]
			var bones: PackedInt32Array = arrays[Mesh.ARRAY_BONES]
			var weights: PackedFloat32Array = arrays[Mesh.ARRAY_WEIGHTS]
			if verts.is_empty() or bones.is_empty():
				continue
			var n := verts.size()
			for vi in n:
				# 4 bone/weight entries per vertex (Godot's default layout).
				var best_w := -1.0
				var best_bone := -1
				for k in 4:
					var w: float = weights[vi * 4 + k]
					if w > best_w:
						best_w = w
						best_bone = bones[vi * 4 + k]
				if best_bone < 0 or best_w <= 0.0:
					continue
				var bname := skeleton.get_bone_name(best_bone)
				if not check_bones.has(bname):
					continue
				sums[bname] = sums.get(bname, Vector3.ZERO) + verts[vi]
				counts[bname] = counts.get(bname, 0) + 1

		for bname in check_bones:
			if not counts.has(bname):
				continue
			var avg: Vector3 = sums[bname] / counts[bname]
			var rest: Vector3 = bone_rest_pos.get(bname, Vector3.INF)
			var dist := avg.distance_to(rest)
			print("  bone=", bname, " vtx_count=", counts[bname], " avg_vertex_pos=", avg, " bone_rest_pos=", rest, " dist=", dist)

	quit(0)
