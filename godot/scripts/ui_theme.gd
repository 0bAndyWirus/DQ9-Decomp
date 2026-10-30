class_name ObservatoryTheme
extends RefCounted

const GOLD := Color("c5a86a")
const IVORY := Color("f5ebd6")
const MIDNIGHT := Color("0c1424")

static func panel(alpha: float = 0.94) -> StyleBoxFlat:
	var box := StyleBoxFlat.new()
	box.bg_color = Color(MIDNIGHT, alpha)
	box.border_color = Color(GOLD, 0.65)
	box.set_border_width_all(1)
	box.set_content_margin_all(14)
	return box
static func toggle_icon(enabled: bool) -> Texture2D:
	var image := Image.create(44, 24, false, Image.FORMAT_RGBA8)
	image.fill(Color.TRANSPARENT)
	var track := Color(GOLD, 0.8) if enabled else Color("344457")
	var edge := GOLD if enabled else Color("8290a0")
	var knob := IVORY if enabled else Color("c2cbd5")
	var knob_x := 32.0 if enabled else 12.0
	for y in 24:
		for x in 44:
			var distance := Vector2(float(x) - clampf(float(x), 11.0, 33.0), float(y) - 11.5).length()
			if distance <= 10.5:
				image.set_pixel(x, y, edge if distance > 9.0 else track)
			if Vector2(float(x) - knob_x, float(y) - 11.5).length() <= 7.0:
				image.set_pixel(x, y, knob)
	return ImageTexture.create_from_image(image)


static func make() -> Theme:
	var result := Theme.new()
	var body := load("res://assets/fonts/NotoSerif.ttf") as Font
	var display := load("res://assets/fonts/Cinzel.ttf") as Font
	if body: result.default_font = body
	result.default_font_size = 20
	if display:
		result.set_font("font", "Window", display)
	for kind in ["Label", "Button", "OptionButton", "CheckButton", "LineEdit", "TextEdit", "TabContainer"]:
		result.set_color("font_color", kind, IVORY)
	result.set_stylebox("panel", "PanelContainer", panel())
	var inset := StyleBoxFlat.new()
	inset.bg_color = Color(0.065, 0.10, 0.16, 0.55)
	inset.border_color = Color(GOLD, 0.24)
	inset.set_border_width_all(1)
	inset.set_content_margin_all(10)
	result.set_stylebox("panel", "TabContainer", inset)
	result.set_stylebox("panel", "ItemList", inset)
	var normal := StyleBoxFlat.new()
	normal.bg_color = Color(0.05, 0.08, 0.13, 0.34)
	normal.set_content_margin_all(10)
	var hover := normal.duplicate() as StyleBoxFlat
	hover.bg_color = Color(0.20, 0.20, 0.18, 0.48)
	hover.border_color = GOLD
	hover.border_width_bottom = 1
	var pressed := hover.duplicate() as StyleBoxFlat
	pressed.bg_color = Color(0.32, 0.27, 0.18, 0.55)
	var focus := StyleBoxFlat.new()
	focus.bg_color = Color(0, 0, 0, 0)
	focus.border_color = GOLD
	focus.border_width_bottom = 2
	for kind in ["Button", "OptionButton", "CheckButton"]:
		result.set_stylebox("normal", kind, normal)
		result.set_stylebox("hover", kind, hover)
		result.set_stylebox("pressed", kind, pressed)
		result.set_stylebox("focus", kind, focus)
		result.set_stylebox("disabled", kind, normal)
		result.set_color("font_hover_color", kind, Color.WHITE)
		result.set_color("font_pressed_color", kind, GOLD)
		result.set_color("font_disabled_color", kind, Color(IVORY, 0.38))
	# CheckButton text begins at the same left edge as ordinary row labels.
	for state in ["normal", "hover", "pressed", "disabled"]:
		var style := result.get_stylebox(state, "CheckButton").duplicate() as StyleBoxFlat
		style.content_margin_left = 0
		result.set_stylebox(state, "CheckButton", style)
	var toggle_focus := focus.duplicate() as StyleBoxFlat
	toggle_focus.border_color = GOLD
	toggle_focus.set_border_width_all(2)
	toggle_focus.bg_color = Color(GOLD, 0.12)
	result.set_stylebox("focus", "CheckButton", toggle_focus)
	var on_icon := toggle_icon(true)
	var off_icon := toggle_icon(false)
	for state in ["checked", "checked_mirrored", "checked_disabled", "checked_disabled_mirrored"]:
		result.set_icon(state, "CheckButton", on_icon)
	for state in ["unchecked", "unchecked_mirrored", "unchecked_disabled", "unchecked_disabled_mirrored"]:
		result.set_icon(state, "CheckButton", off_icon)
	return result
