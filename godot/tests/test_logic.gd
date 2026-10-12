extends SceneTree
const Logic = preload("res://scripts/host_logic.gd")

func _initialize() -> void:
	call_deferred("_run")

func _run() -> void:
	var layouts: Array = []
	for i in 7: layouts.append(Logic.layout_rects(Vector2(1280,800), i))
	assert(layouts.size()==7)
	assert(layouts[0][0].size.x>0 and layouts[0][1].size.y>0)
	assert(layouts[1][1].position.x>layouts[1][0].position.x)
	assert(layouts[2][0].size.x>layouts[2][1].size.x)
	assert(layouts[3][1].size.x>layouts[3][0].size.x)
	assert(layouts[4][0].size.x>0 and layouts[4][1].size==Vector2.ZERO)
	assert(layouts[5][0].size==Vector2.ZERO and layouts[5][1].size.x>0)
	assert(layouts[6][0].size.x>0 and layouts[6][1].size.x>0)
	assert(Logic.touch_coords(Vector2(100,50),Rect2(0,0,100,50))==Vector2i(255,191))
	assert(Logic.touch_coords(Vector2(-10,99),Rect2(0,0,100,50))==Vector2i(0,191))
	var root:Node = get_root()
	assert(Logic.firmware_language("fr") == 2 and Logic.firmware_language("es") == 5 and Logic.firmware_language("it") == 4 and Logic.firmware_language("en") == 1 and Logic.firmware_language("de") == 3)
	assert(InputMap.has_action("ds_a"))
	var expected_button_bits := {"ds_a": 1 << 0, "ds_b": 1 << 1, "ds_select": 1 << 2, "ds_start": 1 << 3, "ds_right": 1 << 4, "ds_left": 1 << 5, "ds_up": 1 << 6, "ds_down": 1 << 7, "ds_r": 1 << 8, "ds_l": 1 << 9, "ds_x": 1 << 10, "ds_y": 1 << 11}
	for action in Logic.BUTTON_ACTIONS:
		assert(Logic.button_mask([action]) == expected_button_bits[action], "wrong DS bit for %s" % action)
	assert(Logic.button_mask(["ds_a", "ds_y", "ds_start"]) == ((1 << 0) | (1 << 11) | (1 << 3)))
	Logic.reset_bindings()
	var key:=InputEventKey.new(); key.physical_keycode=KEY_Z; InputMap.action_add_event("ds_a",key)
	var stored:=Logic.event_to_dict(key); var copy:=Logic.event_from_dict(stored)
	assert(copy is InputEventKey and copy.physical_keycode==KEY_Z)
	var path="user://logic-test.cfg"
	var defaults := Logic.load_settings("user://missing-gpu3d-settings.cfg")
	assert(defaults.renderer_gpu3d == 0 and defaults.gpu3d_scale == 3)
	Logic.save_settings(path,{"language":"de","layout":3,"nearest":false,"renderer_gpu3d":1,"gpu3d_scale":6})
	var settings:=Logic.load_settings(path); assert(settings.language=="de" and settings.layout==3 and not settings.nearest)
	assert(settings.renderer_gpu3d == 1 and settings.gpu3d_scale == 6)
	# Existing settings files without GPU keys retain the exact software default.
	Logic.save_settings(path, {"language":"en"})
	settings = Logic.load_settings(path)
	assert(settings.renderer_gpu3d == 0 and settings.gpu3d_scale == 3)
	Logic.save_settings(path, {"renderer_gpu3d":99, "gpu3d_scale":1})
	settings = Logic.load_settings(path)
	assert(settings.renderer_gpu3d == 1 and settings.gpu3d_scale == 2)
	DirAccess.remove_absolute(ProjectSettings.globalize_path(path))
	var joy:=InputEventJoypadButton.new(); joy.button_index=1
	assert((Logic.event_from_dict(Logic.event_to_dict(joy)) as InputEventJoypadButton).button_index==1)
	print("Godot pure logic tests passed")
	quit()
