extends SceneTree

const JOE_MESH_PATH := "D://Monjolo Project//Source//Characters//Joe//Meshes//SKM_BV_Joe_01.fbx"
const MAYA_JSON_PATH := "D://Monjolo Project//Source//Characters//Joe//Data//Skins//jacket_debug.json"

func _initialize() -> void:
	var f := FileAccess.open(MAYA_JSON_PATH, FileAccess.READ)
	if not f:
		print("FAILED to open maya json")
		quit(1)
		return
	var text := f.get_as_text()
	f.close()
	var data = JSON.parse_string(text)
	if not data:
		print("FAILED to parse json")
		quit(1)
		return

	var weights_entries: Array = data["deformerWeight"]["weights"]
	print("maya weight entries (one per bone): ", weights_entries.size())

	# maya_weights[bone_name][vertex_index] = weight
	var maya_weights: Dictionary = {}
	for entry in weights_entries:
		var bone: String = entry["source"]
		var pts: Array = entry["points"]
		var per_vert: Dictionary = {}
		for p in pts:
			per_vert[int(p["index"])] = float(p["value"])
		maya_weights[bone] = per_vert

	print("maya bones referenced: ", maya_weights.keys())

	var fbx := FbxManager.new()
	var scene := fbx.load_scene(JOE_MESH_PATH)
	if not scene:
		print("FAILED to load fbx scene")
		quit(1)
		return

	# ufbx_mesh names often come back empty (name lives on the instance node, not the mesh
	# data-block) - match by control-point count against Maya's "size": 5335 instead.
	var jacket_entry = null
	for mesh_entry in scene.get_meshes().values():
		var geo: Dictionary = mesh_entry.geometry()
		var vcount: int = (geo.get("vertices", PackedVector3Array()) as PackedVector3Array).size()
		print("mesh idx=", mesh_entry.get_index(), " name='", mesh_entry.get_name(), "' vertex_count=", vcount)
		if vcount == 5335:
			jacket_entry = mesh_entry
	if not jacket_entry:
		print("no mesh with 5335 vertices found")
		quit(1)
		return

	print("jacket mesh: ", jacket_entry.get_name())
	var our_skin: Dictionary = jacket_entry.skin()
	print("our skin bones: ", our_skin.keys())

	# Compare per-(bone, vertex_index) weight values for every bone that appears in both.
	var total_checked := 0
	var total_mismatch := 0
	var worst: Array = [] # [ [diff, bone, idx, maya_w, our_w], ... ]
	for bone in maya_weights.keys():
		if not our_skin.has(bone):
			print("  bone in maya but not in our skin(): ", bone)
			continue
		var our_arr: Array = our_skin[bone]
		var maya_per_vert: Dictionary = maya_weights[bone]
		for idx in maya_per_vert.keys():
			if idx < 0 or idx >= our_arr.size():
				continue
			var maya_w: float = maya_per_vert[idx]
			var our_w: float = our_arr[idx]
			total_checked += 1
			var diff: float = abs(maya_w - our_w)
			if diff > 0.01:
				total_mismatch += 1
				worst.append([diff, bone, idx, maya_w, our_w])

	print("checked=", total_checked, " mismatched(>0.01)=", total_mismatch)
	worst.sort_custom(func(a, b): return a[0] > b[0])
	for i in min(20, worst.size()):
		var w = worst[i]
		print("  MISMATCH bone=", w[1], " vtx=", w[2], " maya=", w[3], " ours=", w[4], " diff=", w[0])

	quit(0)
