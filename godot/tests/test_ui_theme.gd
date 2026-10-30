extends SceneTree

func _initialize() -> void:
	call_deferred("_run")

func _run() -> void:
	var launcher = load("res://scripts/launcher.gd").new()
	root.add_child(launcher)
	await process_frame
	assert(launcher.theme != null)
	assert(launcher.theme.default_font != null)
	assert(launcher._joy_button_name(0, "Xbox Wireless Controller") == "A")
	assert(launcher._joy_button_name(1, "Xbox Wireless Controller") == "B")
	assert(launcher._joy_button_name(0, "Sony DualSense") == "Cross")
	assert(launcher._joy_button_name(1, "Sony DualSense") == "Circle")
	assert(launcher._joy_button_name(0, "Nintendo Switch Pro Controller") == "B")
	assert(launcher._joy_button_name(1, "Nintendo Switch Pro Controller") == "A")
	assert(launcher._joy_button_name(0, "Unknown gamepad") == "Button 1")
	var pad := InputEventJoypadButton.new()
	pad.device = 0
	pad.button_index = JOY_BUTTON_DPAD_DOWN
	pad.pressed = true
	launcher._input(pad)
	assert(launcher._prompt_joypad == 0 and "Button" in launcher._prompts.text)
	var key := InputEventKey.new()
	key.keycode = KEY_DOWN
	key.pressed = true
	launcher._input(key)
	assert(launcher._prompt_joypad == -1 and "Button" not in launcher._prompts.text)
	assert(not launcher._live_music)
	launcher.set_music_mode(true)
	assert(launcher._live_music)
	launcher.set_music_mode(false)
	assert(not launcher._live_music)
	launcher.set_menu_music_volume(0.4)
	launcher.set_ui_sfx_volume(0.3)
	assert(is_equal_approx(launcher._music_volume, 0.4))
	assert(is_equal_approx(launcher._sfx_volume, 0.3))
	launcher.set_reduced_motion(true)
	assert(launcher._celestial.get_shader_parameter("reduced_motion"))
	var image := Image.create(256, 192, false, Image.FORMAT_RGBA8)
	image.fill(Color("526885"))
	launcher.set_live_frame(ImageTexture.create_from_image(image), null)
	assert(launcher._showing_live)
	launcher.clear_live_frame()
	assert(not launcher._showing_live)
	launcher.queue_free()
	await process_frame
	print("Observatory theme logic passed")
	quit(0)
