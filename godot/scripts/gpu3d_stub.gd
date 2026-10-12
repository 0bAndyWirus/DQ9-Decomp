extends Node
# A ROM-free producer for the compositor's integration contract. Not used by
# the launcher: tests explicitly create it and publish its viewport + R8 mask.
const LEFT := Color8(231, 47, 63)
const RIGHT := Color8(39, 219, 87)
var viewport: SubViewport
var mask_texture: ImageTexture

func _init() -> void:
	var mask := Image.create(256, 192, false, Image.FORMAT_R8)
	for y in range(33, 159):
		for x in range(65, 191): mask.set_pixel(x, y, Color(1.0 / 255.0, 0, 0))
	mask_texture = ImageTexture.create_from_image(mask)

func set_scale(scale: int) -> void:
	if is_instance_valid(viewport):
		remove_child(viewport)
		viewport.free()
	viewport = SubViewport.new()
	viewport.size = Vector2i(256, 192) * clampi(scale, 2, 6)
	viewport.disable_3d = true
	viewport.transparent_bg = true
	viewport.render_target_update_mode = SubViewport.UPDATE_ALWAYS
	add_child(viewport)
	for i in 2:
		var rect := ColorRect.new()
		rect.position = Vector2(viewport.size.x * i / 2.0, 0)
		rect.size = Vector2(viewport.size.x / 2.0, viewport.size.y)
		rect.color = LEFT if i == 0 else RIGHT
		viewport.add_child(rect)

func publish(host: Control, screen_is_top := true) -> void:
	host.set_gpu3d_frame(viewport.get_texture(), mask_texture, screen_is_top)

static func masked(x: int, y: int) -> bool:
	return x >= 65 and x < 191 and y >= 33 and y < 159
