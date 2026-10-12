extends RefCounted

# Transparent SubViewport readback is premultiplied RGBA. DS raster_view stores
# straight RGB6/A5. Unassociate only for comparison/PNG diagnostics; retain alpha
# and include every covered/mask pixel, including missing-alpha pixels.
static func straight_alpha(image: Image) -> Image:
	var bytes := image.get_data()
	for pixel in image.get_width() * image.get_height():
		var alpha := int(bytes[pixel * 4 + 3])
		if alpha == 0 or alpha == 255: continue
		for channel in 3:
			bytes[pixel * 4 + channel] = clampi(roundi(float(bytes[pixel * 4 + channel]) * 255.0 / alpha), 0, 255)
	return Image.create_from_data(image.get_width(), image.get_height(), false, Image.FORMAT_RGBA8, bytes)

static func rgb555_display_image(native_rgb6: Image) -> Image:
	var bytes := native_rgb6.get_data()
	for pixel in native_rgb6.get_width() * native_rgb6.get_height():
		for channel in 3:
			var rgb6 := roundi(bytes[pixel * 4 + channel] * 63.0 / 255.0)
			var rgb5 := rgb6 >> 1
			bytes[pixel * 4 + channel] = (rgb5 << 3) | (rgb5 >> 2)
	return Image.create_from_data(native_rgb6.get_width(), native_rgb6.get_height(), false, Image.FORMAT_RGBA8, bytes)

static func color_transfer_texture() -> ImageTexture:
	var values := PackedFloat32Array()
	values.resize(256)
	for i in range(1, 256):
		var linear := pow((i / 255.0 + 0.055) / 1.055, 2.4)
		var low := 0.0
		var high := 1.0
		for step in 40:
			var value := (low + high) * 0.5
			var decoded := value * (value * (value * 0.305306011 + 0.682171111) + 0.012522878)
			if decoded < linear: low = value
			else: high = value
		values[i] = (low + high) * 0.5
	return ImageTexture.create_from_image(Image.create_from_data(256, 1, false, Image.FORMAT_RF, values.to_byte_array()))
