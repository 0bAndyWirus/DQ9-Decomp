extends RefCounted

# A crack is a narrow background-colour outlier inside an eroded native polygon
# footprint, not a silhouette/texture difference. Independent ID passes prove
# that the foreground is missing even with texture alpha disabled: a genuine
# hi-res cutout texel is not a mesh crack. Thresholds are RGB8 levels; this
# diagnostic never changes the production hybrid mask or its quality comparison.
static func _delta(a: PackedByteArray, a_index: int, b: PackedByteArray, b_index: int) -> int:
	return maxi(absi(int(a[a_index]) - int(b[b_index])), maxi(absi(int(a[a_index + 1]) - int(b[b_index + 1])), absi(int(a[a_index + 2]) - int(b[b_index + 2]))))

static func measure(gpu: Image, reference: Image, mask: PackedByteArray,
		native_ids: PackedByteArray, metadata: Image, geometry: Image, scale: int) -> Dictionary:
	assert(gpu.get_size() == Vector2i(256, 192) * scale)
	assert(metadata.get_size() == gpu.get_size())
	assert(geometry.get_size() == gpu.get_size())
	assert(native_ids.size() == 256 * 192 and mask.size() == native_ids.size())
	var a := gpu.get_data()
	var b := reference.get_data()
	var ids := metadata.get_data()
	var coverage := geometry.get_data()
	var width := gpu.get_width()
	var footprint := PackedByteArray()
	footprint.resize(256 * 192)
	for y in range(1, 191):
		for x in range(1, 255):
			var pixel := y * 256 + x
			if mask[pixel] == 0 or b[pixel * 4 + 3] != 255: continue
			var interior := true
			for dy in range(-1, 2):
				for dx in range(-1, 2):
					var neighbor := pixel + dy * 256 + dx
					if mask[neighbor] == 0 or native_ids[neighbor] != native_ids[pixel] or _delta(b, pixel * 4, b, neighbor * 4) > 32:
						interior = false
			footprint[pixel] = int(interior)
	var cracks := 0
	var outliers := 0
	var tested := 0
	var texture_outliers := 0
	var by_id: Dictionary = {}
	var highlight := PackedByteArray()
	highlight.resize(width * gpu.get_height() * 4)
	for y in range(scale, gpu.get_height() - scale):
		for x in range(scale, width - scale):
			var native_pixel := (y / scale) * 256 + x / scale
			if footprint[native_pixel] == 0: continue
			tested += 1
			var index := (y * width + x) * 4
			var reference_index := native_pixel * 4
			if a[index + 3] != 0 and _delta(a, index, b, reference_index) <= 48: continue
			outliers += 1
			var gpu_id := clampi(roundi((int(ids[index]) - 64) / 3.0), 0, 63)
			if ids[index + 3] != 0 and gpu_id == native_ids[native_pixel]: continue
			# Reject large legitimate surfaces: a thin join must have foreground
			# on both sides, at most one native pixel away, on at least one axis.
			var narrow := false
			for distance in range(1, scale + 1):
				for offset in [distance * 4, distance * width * 4]:
					if _delta(a, index - offset, b, reference_index) <= 32 and _delta(a, index + offset, b, reference_index) <= 32:
						narrow = true
			if not narrow: continue
			var geometry_id := clampi(roundi((int(coverage[index]) - 64) / 3.0), 0, 63)
			if coverage[index + 3] != 0 and geometry_id == native_ids[native_pixel]:
				texture_outliers += 1
				continue
			cracks += 1
			var id := int(native_ids[native_pixel])
			by_id[str(id)] = int(by_id.get(str(id), 0)) + 1
			highlight[index] = 255
			highlight[index + 3] = 255
	return {"crack_pixels": cracks, "tested_pixels": tested, "strong_outlier_pixels": outliers,
		"texture_alpha_outlier_pixels": texture_outliers, "by_polygon_id": by_id,
		"highlight": Image.create_from_data(width, gpu.get_height(), false, Image.FORMAT_RGBA8, highlight)}

static func self_test() -> bool:
	var reference := Image.create(256, 192, false, Image.FORMAT_RGBA8)
	reference.fill(Color(0.05, 0.2, 0.9, 1))
	var mask := PackedByteArray()
	mask.resize(256 * 192)
	mask.fill(1)
	var native_ids := mask.duplicate()
	native_ids.fill(8)
	for scale in [3, 6]:
		var gpu := reference.duplicate() as Image
		gpu.resize(256 * scale, 192 * scale, Image.INTERPOLATE_NEAREST)
		var ids := gpu.duplicate() as Image
		ids.fill(Color(88.0 / 255, 64.0 / 255, 250.0 / 255, 1))
		var coverage := ids.duplicate() as Image
		if measure(gpu, reference, mask, native_ids, ids, ids, scale).crack_pixels != 0: return false
		# Exactly one output row of terrain through an otherwise solid polygon.
		for x in range(40 * scale, 44 * scale):
			gpu.set_pixel(x, 50 * scale + scale / 2, Color(0.4, 0.6, 0.1, 1))
			ids.set_pixel(x, 50 * scale + scale / 2, Color(64.0 / 255, 64.0 / 255, 250.0 / 255, 1))
		if measure(gpu, reference, mask, native_ids, ids, ids, scale).crack_pixels != 4 * scale: return false
		var cutout := measure(gpu, reference, mask, native_ids, ids, coverage, scale)
		if cutout.crack_pixels != 0 or cutout.texture_alpha_outlier_pixels != 4 * scale: return false
		# A high-resolution texel of that colour is not a coverage hole.
		ids.fill(Color(88.0 / 255, 64.0 / 255, 250.0 / 255, 1))
		if measure(gpu, reference, mask, native_ids, ids, ids, scale).crack_pixels != 0: return false
	return true
