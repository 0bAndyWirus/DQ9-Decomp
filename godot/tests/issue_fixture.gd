extends RefCounted

const FIXTURES := "res://tests/fixtures/issue_bundles"

static func make_synthetic(base: String) -> PackedStringArray:
	var roots := PackedStringArray([base.path_join("sdl"), base.path_join("godot")])
	for path in roots: DirAccess.make_dir_recursive_absolute(path)
	var source := DirAccess.open(FIXTURES)
	var index := 0
	for name in source.get_directories():
		var destination := roots[index % 2].path_join(name)
		DirAccess.make_dir_recursive_absolute(destination)
		var directory := DirAccess.open(FIXTURES.path_join(name))
		for file in directory.get_files():
			DirAccess.copy_absolute(ProjectSettings.globalize_path(FIXTURES.path_join(name).path_join(file)), destination.path_join(file))
		var image := Image.create(256, 384, false, Image.FORMAT_RGBA8)
		image.fill(Color(0.1, 0.2, 0.35))
		image.fill_rect(Rect2i(0, 192, 256, 192), Color(0.2, 0.35, 0.15))
		image.save_png(destination.path_join("screenshot_both.png"))
		index += 1
	return roots

# The real session is generated locally during the test, never stored in fixtures.
static func make_real_bundle(base: String, rom: String, save: String, inputs: String, native: String, backup_type: String, frame_count: int) -> Dictionary:
	var bundle := base.path_join("godot/20261014-120000-4-real-session")
	DirAccess.make_dir_recursive_absolute(bundle)
	if DirAccess.copy_absolute(save, bundle.path_join("session_start.sav")) != OK: return {"error": "Cannot copy local launch save"}
	var rows := PackedStringArray()
	for line in FileAccess.get_file_as_string(inputs).split("\n", false):
		var parts := line.strip_edges().split(" ", false)
		if parts.size() == 4 and int(parts[0]) < frame_count: rows.append(line)
	_write(bundle.path_join("session_input.txt"), "\n".join(rows) + "\n")
	_write(bundle.path_join("session_input.txt.frames"), "%d\n" % frame_count)
	_write(bundle.path_join("comment.txt"), "Real replay fixture, generated from user-local files.\nframe: %d\nrtc_epoch: 0\nrtc_timezone_offset_minutes: 0\n" % (frame_count - 1))
	_write(bundle.path_join("env.txt"), "rom_path: %s\nlanguage: en\nbackup_type: %s\nrtc_epoch: 0\nrtc_timezone_offset_minutes: 0\n" % [rom, backup_type])
	_write(bundle.path_join("state.txt"), "audio_debug: solo=2 muted=3\nframe=%d\naudio_queue_ms=40\nunderruns=0\n" % (frame_count - 1))
	_write(bundle.path_join("audio_stats.txt"), "audio_driver: synthetic\nqueued_audio_ms: 40\nunderruns: 0\npeak: 0.1\n")
	var native_save := base.path_join("native.sav")
	DirAccess.copy_absolute(save, native_save)
	var native_hashes := base.path_join("native.hashes")
	var output := []
	var status := OS.execute(native, PackedStringArray(["--headless", "--rom", rom, "--save", native_save, "--backup-type", backup_type, "--frames", str(frame_count), "--input-script", bundle.path_join("session_input.txt"), "--rtc-epoch", "0", "--rtc-timezone-offset", "0", "--hash-file", native_hashes, "--dump-frames", str(frame_count - 1), "--dump-dir", base]), output, true)
	if status != 0: return {"error": "Native fixture run exited %d: %s" % [status, str(output)]}
	var top := _ppm(base.path_join("frame_%04d_top.ppm" % (frame_count - 1)))
	var bottom := _ppm(base.path_join("frame_%04d_bottom.ppm" % (frame_count - 1)))
	if top == null or bottom == null: return {"error": "Missing native fixture screenshots"}
	top.save_png(bundle.path_join("screenshot_top.png")); bottom.save_png(bundle.path_join("screenshot_bottom.png"))
	var both := Image.create(256, 384, false, Image.FORMAT_RGB8)
	both.blit_rect(top, Rect2i(0, 0, 256, 192), Vector2i.ZERO)
	both.blit_rect(bottom, Rect2i(0, 0, 256, 192), Vector2i(0, 192))
	both.save_png(bundle.path_join("screenshot_both.png"))
	return {"bundle": bundle, "hashes": native_hashes}

static func snapshot_originals(roots: PackedStringArray) -> Dictionary:
	var hashes := {}
	for root in roots:
		var directory := DirAccess.open(root)
		if directory == null: continue
		for name in directory.get_directories():
			var bundle := root.path_join(name)
			for file in DirAccess.open(bundle).get_files():
				if file != "status.json": hashes[bundle.path_join(file)] = FileAccess.get_sha256(bundle.path_join(file))
	return hashes

static func remove_tree(path: String) -> void:
	var directory := DirAccess.open(path)
	if directory == null: return
	for name in directory.get_directories(): remove_tree(path.path_join(name))
	for name in directory.get_files(): DirAccess.remove_absolute(path.path_join(name))
	DirAccess.remove_absolute(path)

static func _ppm(path: String) -> Image:
	if not FileAccess.file_exists(path): return null
	var bytes := FileAccess.get_file_as_bytes(path)
	var offset := 0
	for line in 3:
		while offset < bytes.size() and bytes[offset] != 10: offset += 1
		offset += 1
	if bytes.size() - offset != 256 * 192 * 3: return null
	return Image.create_from_data(256, 192, false, Image.FORMAT_RGB8, bytes.slice(offset))

static func _write(path: String, text: String) -> void:
	var file := FileAccess.open(path, FileAccess.WRITE); file.store_string(text); file.close()
