extends RefCounted

# DS banner language order. Version 1 has the first six, version 2 adds
# Chinese, and version 3 (including DSi banners) adds Korean.
const LANGUAGES := ["ja", "en", "fr", "de", "it", "es", "zh", "ko"]
const HEADER_BANNER_OFFSET := 0x68
const BANNER_ICON_OFFSET := 0x20
const BANNER_PALETTE_OFFSET := 0x220
const BANNER_TITLE_OFFSET := 0x240
const TITLE_BYTES := 0x100

# Matches the native launcher's structural checks; this does not prove that
# the image is the game or region expected by the generated runtime.
static func validate_rom(path: String) -> String:
	var file := FileAccess.open(path, FileAccess.READ)
	if file == null:
		return "The file cannot be read."
	var size := file.get_length()
	if size < 0x4000:
		return "The file is too small to be a Nintendo DS image."
	var header := file.get_buffer(0x200)
	if header.size() != 0x200:
		return "The file cannot be read."
	var used := header.decode_u32(0x80)
	if used > size and used < (1 << 30):
		return "The image is truncated (%d of %d bytes)." % [size, used]
	var arm9_size := header.decode_u32(0x2c)
	var arm7_size := header.decode_u32(0x3c)
	if arm9_size == 0 or arm7_size == 0 or header.decode_u32(0x20) + arm9_size > size or header.decode_u32(0x30) + arm7_size > size:
		return "This does not look like a Nintendo DS image (its header is not valid)."
	return ""

# Returns a null texture and empty title for unavailable or malformed banners.
# Only the header and static banner are read; the cartridge image stays on disk.
static func load_rom(path: String, locale: String = "") -> Dictionary:
	var empty := {"texture": null, "title": ""}
	var file := FileAccess.open(path, FileAccess.READ)
	if file == null or file.get_length() < HEADER_BANNER_OFFSET + 4:
		return empty
	file.seek(HEADER_BANNER_OFFSET)
	var offset := file.get_32()
	var length := file.get_length()
	if offset == 0 or offset > length - 2:
		return empty
	file.seek(offset)
	var version := file.get_16()
	var title_count := 0
	match version:
		1: title_count = 6
		2: title_count = 7
		3, 0x103: title_count = 8
		_: return empty
	var banner_size := BANNER_TITLE_OFFSET + TITLE_BYTES * title_count
	if offset > length - banner_size:
		return empty
	file.seek(offset)
	var banner := file.get_buffer(banner_size)
	if banner.size() != banner_size:
		return empty
	var image := Image.create(32, 32, false, Image.FORMAT_RGBA8)
	for y in 32:
		for x in 32:
			# Tiles are row-major; each 8x8 tile stores the left pixel in
			# the low nibble, and palette index zero is transparent.
			var tile := (y / 8) * 4 + x / 8
			var pixel := (y % 8) * 8 + x % 8
			var packed: int = banner[BANNER_ICON_OFFSET + tile * 32 + pixel / 2]
			var index := (packed >> (4 * (pixel % 2))) & 0xf
			if index == 0:
				image.set_pixel(x, y, Color(0, 0, 0, 0))
			else:
				var color := banner.decode_u16(BANNER_PALETTE_OFFSET + index * 2)
				image.set_pixel(x, y, Color(float(color & 31) / 31.0, float((color >> 5) & 31) / 31.0, float((color >> 10) & 31) / 31.0, 1.0))
	var language := locale if not locale.is_empty() else OS.get_locale()
	language = language.to_lower().replace("-", "_").get_slice("_", 0)
	var preferred := LANGUAGES.find(language)
	var candidates: Array[int] = []
	if preferred >= 0 and preferred < title_count:
		candidates.append(preferred)
	if not 1 in candidates:
		candidates.append(1)
	for i in title_count:
		if not i in candidates:
			candidates.append(i)
	var title := ""
	for index in candidates:
		title = _decode_title(banner, BANNER_TITLE_OFFSET + index * TITLE_BYTES)
		if not title.is_empty():
			break
	return {"texture": ImageTexture.create_from_image(image), "title": title}

# The fixed-size UTF-16LE field may contain surrogate pairs and ends at NUL.
static func _decode_title(bytes: PackedByteArray, offset: int) -> String:
	var result := ""
	var i := 0
	while i < TITLE_BYTES:
		var code := bytes.decode_u16(offset + i)
		i += 2
		if code == 0:
			break
		if code >= 0xd800 and code <= 0xdbff:
			if i < TITLE_BYTES:
				var low := bytes.decode_u16(offset + i)
				if low >= 0xdc00 and low <= 0xdfff:
					code = 0x10000 + ((code - 0xd800) << 10) + low - 0xdc00
					i += 2
				else:
					code = 0xfffd
			else:
				code = 0xfffd
		elif code >= 0xdc00 and code <= 0xdfff:
			code = 0xfffd
		result += String.chr(code)
	return result.strip_edges()
