extends Node2D

"""D://Monjolo Project//backup//Characters//Meninos//Menino.fbx
D://Monjolo Project//Source//Characters//Caramelo//SKM_Caramelo.fbx
"""

func _ready() -> void:
	var fbx := FbxManager.new()
	var scene := fbx.load_scene("D://Monjolo Project//backup//Characters//Meninos//Menino.fbx")
	if not scene:
		return

	print("meshes: ", scene.get_mesh_names())

	var first := scene.get_mesh(0)
	#print("geometry: ", first.geometry())
	#print("material: ", first.material())
	#print("normal: ", first.normal())
	#print("uv: ", first.uv())
	#print("skin: ", first.skin())
	#print("skeleton: ", first.skeleton())

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
		mesh_entry.load_skin() # no-op if the mesh isn't skinned

	add_child(scene.import())
