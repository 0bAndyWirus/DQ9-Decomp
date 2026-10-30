extends SceneTree

const RomArt = preload("res://scripts/rom_art.gd")

func _initialize() -> void:
	call_deferred("_run")

func _run() -> void:
	# This fixture is synthetic and lives in the OS temporary directory, never
	# alongside a user's ROM, save, or launcher settings.
	var path := OS.get_temp_dir().path_join("dq9-rom-art-test-%d.nds" % OS.get_process_id())
	var bytes := PackedByteArray()
	bytes.resize(0x200 + 0x840)
	bytes.encode_u32(0x68, 0x200)
	bytes.encode_u16(0x200, 1)
	# Tile 0: transparent then red. Tile 1: green at its first pixel.
	bytes[0x220] = 0x10
	bytes[0x240] = 0x02
	bytes.encode_u16(0x200 + 0x220 + 2, 0x001f)
	bytes.encode_u16(0x200 + 0x220 + 4, 0x03e0)
	_title(bytes, 0, [0x65e5, 0x672c])
	_title(bytes, 1, [0x0045, 0x006e, 0x0067, 0x006c, 0x0069, 0x0073, 0x0068])
	_title(bytes, 2, [0x0046, 0x0072, 0x0061, 0x006e, 0x00e7, 0x0061, 0x0069, 0x0073, 0x000a, 0xd83d, 0xde00])
	var file := FileAccess.open(path, FileAccess.WRITE)
	if file == null:
		_fail("Cannot create synthetic ROM fixture")
		return
	file.store_buffer(bytes)
	file.close()
	var french: Dictionary = RomArt.load_rom(path, "fr_FR")
	if french.title != "Français\n😀" or french.texture == null:
		_finish(path, "Localized UTF-16 title was not decoded")
		return
	var image: Image = french.texture.get_image()
	if image.get_width() != 32 or image.get_height() != 32 or image.get_pixel(0, 0).a != 0.0 or image.get_pixel(1, 0) != Color(1, 0, 0, 1) or image.get_pixel(8, 0) != Color(0, 1, 0, 1):
		_finish(path, "Tiled icon or palette was decoded incorrectly")
		return
	if RomArt.load_rom(path, "ko").title != "English":
		_finish(path, "Unavailable language did not fall back to English")
		return
	bytes.encode_u16(0x200, 2) # Version 2 requires a seventh title.
	file = FileAccess.open(path, FileAccess.WRITE)
	file.store_buffer(bytes)
	file.close()
	if RomArt.load_rom(path, "en").texture != null:
		_finish(path, "Truncated banner was accepted")
		return
	bytes.encode_u32(0x68, 0xffffffff)
	file = FileAccess.open(path, FileAccess.WRITE)
	file.store_buffer(bytes)
	file.close()
	if RomArt.load_rom(path, "en").texture != null or RomArt.load_rom(path + ".missing", "en").title != "":
		_finish(path, "Invalid or missing ROM was accepted")
		return
	# Structural checks follow the native launcher; a bad header CRC or a
	# missing banner alone must not reject patched or bannerless images.
	bytes.resize(0x10000)
	bytes.fill(0)
	bytes.encode_u32(0x20, 0x4000)
	bytes.encode_u32(0x2c, 0x4000)
	bytes.encode_u32(0x30, 0x8000)
	bytes.encode_u32(0x3c, 0x2000)
	bytes.encode_u32(0x80, 0x10000)
	file = FileAccess.open(path, FileAccess.WRITE)
	file.store_buffer(bytes)
	file.close()
	if not RomArt.validate_rom(path).is_empty() or RomArt.validate_rom(path + ".missing").is_empty():
		_finish(path, "Valid or missing image validation failed")
		return
	bytes.encode_u32(0x80, 0x11000)
	file = FileAccess.open(path, FileAccess.WRITE)
	file.store_buffer(bytes)
	file.close()
	if not RomArt.validate_rom(path).contains("truncated"):
		_finish(path, "Truncated image was accepted")
		return
	bytes.encode_u32(0x80, 0x10000)
	bytes.encode_u32(0x2c, 0)
	file = FileAccess.open(path, FileAccess.WRITE)
	file.store_buffer(bytes)
	file.close()
	if RomArt.validate_rom(path).is_empty():
		_finish(path, "Image without ARM9 code was accepted")
		return
	bytes.resize(0x100)
	file = FileAccess.open(path, FileAccess.WRITE)
	file.store_buffer(bytes)
	file.close()
	if RomArt.validate_rom(path).is_empty():
		_finish(path, "Short image was accepted")
		return
	_finish(path, "")

func _title(bytes: PackedByteArray, language: int, units: Array) -> void:
	for i in units.size():
		bytes.encode_u16(0x200 + 0x240 + language * 0x100 + i * 2, units[i])

func _finish(path: String, error: String) -> void:
	DirAccess.remove_absolute(path)
	if not error.is_empty():
		_fail(error)
		return
	print("ROM banner parser tests passed")
	quit(0)

func _fail(message: String) -> void:
	push_error(message)
	quit(1)
