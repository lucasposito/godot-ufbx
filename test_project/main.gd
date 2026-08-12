extends Node2D

"""D://Monjolo Project//backup//Characters//Meninos//Menino.fbx
D://Monjolo Project//backup//Characters//Meninos//Textures//tx_gianluca_albedo.png
"""

func _ready() -> void:
	##### TEST ZONE #####
	
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
		_fix_material_culling(mesh_entry)
		mesh_entry.load_skin() # no-op if the mesh isn't skinned

	var character := scene.import()
	add_child(character)

	_apply_texture(character, "D://Monjolo Project//backup//Characters//Meninos//Textures//tx_gianluca_albedo.png")
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
