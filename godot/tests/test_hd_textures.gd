extends SceneTree
# ROM-free rendered test; requires DQ9_GPU_SCENE_TESTS and a rendered backend.
# xvfb-run -a Godot --path godot --script res://tests/test_hd_textures.gd

var failures: Array[String] = []
var fixture: Node
var scene: Node

func check(condition: bool, message: String) -> void:
	if not condition:
		failures.append(message)
		push_error(message)

func _initialize() -> void:
	call_deferred("_run")

func _settle() -> void:
	for i in 3: await process_frame
	await RenderingServer.frame_post_draw

func _info(stats: Dictionary) -> Dictionary:
	return scene.test_texture_info(int(stats.texture_param), int(stats.palette_base), int(stats.texture_generation))

func _render(options: Dictionary) -> Dictionary:
	var stats: Dictionary = fixture.render_probe(options)
	await _settle()
	return {"stats": stats, "info": _info(stats), "pixels": scene.get_texture().get_image().get_data()}

func _run() -> void:
	if not ClassDB.class_exists("DqGpuSceneFixture"):
		GDExtensionManager.load_extension("res://dq9.gdextension")
	if not ClassDB.class_exists("DqGpuSceneFixture"):
		push_error("DqGpuSceneFixture requires a DQ9_GPU_SCENE_TESTS build")
		quit(2)
		return
	fixture = ClassDB.instantiate("DqGpuSceneFixture")
	root.add_child(fixture)
	# First render initializes the fixture and its scene.
	fixture.render_probe({"format": 4, "quad": true})
	scene = fixture.get_scene()
	var base := {"format": 4, "quad": true, "color": 0x7fff}
	scene.set_hd_textures_enabled(false)
	var original: Dictionary = await _render(base)
	var identity: String = original.info.get("identity", "")
	check(not identity.is_empty(), "paletted texture has an identity")
	var relocated := base.merged({"texture_slot": 1, "palette_base": 1}, true)
	var moved: Dictionary = await _render(relocated)
	check(identity == moved.info.get("identity", ""), "same payload and palette in different VRAM slots have the same key")
	var changed: Dictionary = await _render(base.merged({"palette_changed": true}, true))
	check(identity != changed.info.get("identity", ""), "changing palette content changes the key")
	check(original.info.image.get_data() == moved.info.image.get_data(), "relocated fixture really decodes identical texels")
	check(original.info.image.get_data() != changed.info.image.get_data(), "palette change affects decoded texels")

	# Compressed metadata contains palette addresses as well as decode modes.
	# Moving 16 bytes from the global palette base into each tile's offset
	# preserves every effective palette address and the decoded image.
	var compressed := {"format": 5, "quad": true, "palette_base": 1}
	var compressed_a: Dictionary = await _render(compressed)
	var compressed_b: Dictionary = await _render(compressed.merged(
		{"palette_base": 0, "compressed_palette_offset": 4}, true))
	var compressed_key: String = compressed_a.info.get("identity", "")
	check(not compressed_key.is_empty(), "compressed texture has an identity")
	check(compressed_key == compressed_b.info.get("identity", ""),
		"equivalent compressed palette addresses have the same key")
	check(compressed_a.info.image.get_data() == compressed_b.info.image.get_data(),
		"equivalent compressed palette addresses decode the same image")
	var compressed_mode: Dictionary = await _render(compressed.merged({"compressed_mode_changed": true}, true))
	check(compressed_key != compressed_mode.info.get("identity", ""),
		"compressed tile mode changes the key")
	var compressed_data: Dictionary = await _render(compressed.merged({"texture_changed": true}, true))
	check(compressed_key != compressed_data.info.get("identity", ""),
		"compressed texture payload changes the key")
	var compressed_palette: Dictionary = await _render(compressed.merged({"palette_changed": true}, true))
	check(compressed_key != compressed_palette.info.get("identity", ""),
		"compressed referenced palette changes the key")

	var dir := "user://texture_overrides"
	check(DirAccess.make_dir_recursive_absolute(dir) == OK, "create override directory")
	var path := dir.path_join(identity + ".png")
	# Never overwrite an existing user asset, even in a test build.
	if identity.is_empty() or FileAccess.file_exists(path):
		push_error("refusing to overwrite existing texture override: " + path)
		quit(1)
		return

	# Absence and malformed file must retain the exact disabled GPU output.
	scene.set_hd_textures_enabled(true)
	var missing: Dictionary = await _render(base)
	check(missing.pixels == original.pixels, "missing override preserves all GPU pixels")
	var invalid := FileAccess.open(path, FileAccess.WRITE)
	if invalid == null:
		push_error("cannot create invalid override fixture")
		quit(1)
		return
	invalid.store_string("not a PNG")
	invalid.close()
	scene.set_hd_textures_enabled(false)
	scene.set_hd_textures_enabled(true)
	var malformed: Dictionary = await _render(base)
	check(malformed.pixels == original.pixels, "invalid override preserves all GPU pixels")
	# Oversized dimensions are rejected before decoding, retaining the DS output.
	var oversized := Image.create(2049, 1, false, Image.FORMAT_RGBA8)
	oversized.fill(Color.WHITE)
	check(oversized.save_png(path) == OK, "write oversized replacement")
	scene.set_hd_textures_enabled(false)
	scene.set_hd_textures_enabled(true)
	var rejected_size: Dictionary = await _render(base)
	check(rejected_size.pixels == original.pixels, "oversized replacement falls back to DS pixels")
	check(rejected_size.info.image.get_size() == original.info.image.get_size(), "oversized image is not bound")
	# A small IHDR must not allow an unbounded encoded file into the decoder.
	var encoded_fixture := Image.create(16, 16, false, Image.FORMAT_RGBA8)
	encoded_fixture.fill(Color8(255, 0, 255, 255))
	check(encoded_fixture.save_png(path) == OK, "write visibly different small PNG header")
	var oversized_file := FileAccess.open(path, FileAccess.READ_WRITE)
	check(oversized_file != null, "open oversized encoded fixture")
	if oversized_file != null:
		oversized_file.seek(20 * 1024 * 1024)
		oversized_file.store_8(0)
		oversized_file.close()
		scene.set_hd_textures_enabled(false)
		scene.set_hd_textures_enabled(true)
		var rejected_file: Dictionary = await _render(base)
		check(rejected_file.pixels == original.pixels, "oversized encoded PNG falls back")
		check(rejected_file.info.image.get_data() == original.info.image.get_data(),
			"oversized encoded PNG leaves DS image bound")


	# Opaque DS polygons have no partial-fragment pass. Do not silently discard
	# the fractional pixels of an otherwise valid replacement.
	var fractional := Image.create(16, 16, false, Image.FORMAT_RGBA8)
	fractional.fill(Color8(255, 0, 255, 128))
	check(fractional.save_png(path) == OK, "write partial-alpha replacement")
	scene.set_hd_textures_enabled(false)
	scene.set_hd_textures_enabled(true)
	var rejected_alpha: Dictionary = await _render(base)
	check(rejected_alpha.pixels == original.pixels, "partial-alpha replacement on opaque DS texture falls back")
	check(rejected_alpha.info.image.get_data() == original.info.image.get_data(), "opaque DS decode remains bound")
	# Fractional DS textures also require the same alpha eligibility at each
	# texel; moving partial alpha onto opaque texels is unsafe.
	var partial_opts := {"format": 1, "quad": true, "partial_texture": true, "blend": true}
	scene.set_hd_textures_enabled(false)
	var partial_original: Dictionary = await _render(partial_opts)
	var partial_key: String = partial_original.info.get("identity", "")
	var partial_path := dir.path_join(partial_key + ".png")
	if partial_key.is_empty() or FileAccess.file_exists(partial_path):
		push_error("refusing to overwrite partial texture override: " + partial_path)
		DirAccess.remove_absolute(ProjectSettings.globalize_path(path))
		quit(1)
		return
	var changed_alpha: Image = partial_original.info.image.duplicate()
	changed_alpha.fill(Color8(255, 0, 255, 128))
	check(changed_alpha.save_png(partial_path) == OK, "write changed fractional alpha")
	scene.set_hd_textures_enabled(true)
	var partial_rejected: Dictionary = await _render(partial_opts)
	check(partial_rejected.pixels == partial_original.pixels, "changed fractional alpha falls back")
	check(partial_rejected.info.image.get_data() == partial_original.info.image.get_data(),
		"changed fractional alpha leaves original bound")
	var preserved_alpha: Image = partial_original.info.image.duplicate()
	preserved_alpha.resize(preserved_alpha.get_width() * 2, preserved_alpha.get_height() * 2,
		Image.INTERPOLATE_NEAREST)
	check(preserved_alpha.save_png(partial_path) == OK, "write preserved fractional alpha")
	scene.set_hd_textures_enabled(false)
	scene.set_hd_textures_enabled(true)
	var partial_accepted: Dictionary = await _render(partial_opts)
	check(partial_accepted.info.image.get_size() == preserved_alpha.get_size(),
		"nearest-expanded fractional alpha is accepted")
	check(partial_accepted.pixels == partial_original.pixels, "preserved fractional alpha renders identically")
	DirAccess.remove_absolute(ProjectSettings.globalize_path(partial_path))


	# A 2x nearest expansion must render identically, including clamp, repeat,
	# and mirrored repeat. It exercises the shader's native-coordinate mapping.
	var expanded: Image = original.info.image.duplicate()
	expanded.resize(expanded.get_width() * 2, expanded.get_height() * 2, Image.INTERPOLATE_NEAREST)
	check(expanded.save_png(path) == OK, "write 2x replacement")
	for wrap in [0, 3, 15]:
		var opts := base.merged({"wrap": wrap}, true)
		scene.set_hd_textures_enabled(false)
		var disabled: Dictionary = await _render(opts)
		scene.set_hd_textures_enabled(true)
		var enabled: Dictionary = await _render(opts)
		check(enabled.info.get("identity", "") == identity, "sampling flags do not change content key, wrap=%d" % wrap)
		check(enabled.info.image.get_size() == expanded.get_size(), "2x replacement is bound, wrap=%d" % wrap)
		check(enabled.pixels == disabled.pixels, "2x nearest replacement preserves GPU mapping, wrap=%d" % wrap)
	# Different sampling flags create different DS cache keys within one VRAM
	# generation, but the content-addressed replacement resource is shared.
	var shared_a: Dictionary = await _render(base)
	var shared_b: Dictionary = await _render(base.merged({"wrap": 15, "reuse_generation": true}, true))
	var shared_first: Dictionary = _info(shared_a.stats)
	check(shared_a.info.resource_id == shared_b.info.resource_id and
		shared_first.resource_id == shared_b.info.resource_id,
		"wrap and flip variants share the same replacement texture resource")

	# A visibly different replacement proves the selected image reaches the draw.
	var solid := Image.create(expanded.get_width(), expanded.get_height(), false, Image.FORMAT_RGBA8)
	solid.fill(Color8(255, 0, 255, 255))
	check(solid.save_png(path) == OK, "write distinct replacement")
	scene.set_hd_textures_enabled(false)
	scene.set_hd_textures_enabled(true)
	var replaced: Dictionary = await _render(base)
	check(replaced.pixels != original.pixels, "valid replacement changes rendered GPU pixels")
	DirAccess.remove_absolute(ProjectSettings.globalize_path(path))
	fixture.queue_free()
	await process_frame
	if failures.is_empty():
		print("HD_TEXTURE_SYNTHETIC_OK")
	quit(0 if failures.is_empty() else 1)
