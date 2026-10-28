extends SceneTree
const Logic = preload("res://scripts/host_logic.gd")

func _initialize() -> void:
	call_deferred("_run")

func _run() -> void:
	var layouts: Array = []
	for i in 8: layouts.append(Logic.layout_rects(Vector2(1280,800), i))
	assert(layouts.size()==8)
	assert(layouts[0][0].size.x>0 and layouts[0][1].size.y>0)
	assert(layouts[1][1].position.x>layouts[1][0].position.x)
	assert(layouts[2][0].size.x>layouts[2][1].size.x)
	assert(layouts[3][1].size.x>layouts[3][0].size.x)
	assert(layouts[4][0].size.x>0 and layouts[4][1].size==Vector2.ZERO)
	assert(layouts[5][0].size==Vector2.ZERO and layouts[5][1].size.x>0)
	assert(layouts[6][0].size.x>0 and layouts[6][1].size.x>0)
	assert(layouts[7][0].size.x>0 and layouts[7][1].size == Vector2.ZERO)
	var shape := {"source": Rect2(20, 30, 80, 40), "destination": Rect2(0.25, 0.5, 0.5, 0.25)}
	var destination := Logic.single_shape_rect(Rect2(100, 50, 800, 600), shape)
	assert(destination == Rect2(300, 350, 400, 150))
	assert(Logic.single_shape_touch(destination.position, destination, shape.source) == Vector2i(20, 30))
	assert(Logic.single_shape_touch(destination.end, destination, shape.source) == Vector2i(99, 69))
	assert(Logic.single_shape_touch(destination.position + Vector2(4.99, 0), destination, shape.source).x == 20)
	assert(Logic.single_shape_touch(destination.position + Vector2(5.01, 0), destination, shape.source).x == 21)
	assert(Logic.detect_single_screen_shapes({}, [shape]) == [shape])
	var abi_root := OS.get_temp_dir().path_join("dq9-abi-find-%d" % Time.get_ticks_usec())
	DirAccess.make_dir_recursive_absolute(abi_root.path_join("build/gen"))
	DirAccess.make_dir_recursive_absolute(abi_root.path_join("godot/bin"))
	assert(Logic.find_abi_config(abi_root.path_join("godot/bin")) == "")
	var abi_file := FileAccess.open(abi_root.path_join("build/gen/abi.ini"), FileAccess.WRITE); abi_file.store_string("[arm9]\n"); abi_file.close()
	assert(Logic.find_abi_config(abi_root.path_join("godot/bin")) == abi_root.path_join("build/gen/abi.ini"))
	assert(Logic.find_abi_config("") == "")
	DirAccess.remove_absolute(abi_root.path_join("build/gen/abi.ini"))
	assert(Logic.BACKUP_TYPES.size() == Logic.BACKUP_LABELS.size() and Logic.BACKUP_TYPES[0] == "auto")
	assert("eeprom64k" in Logic.BACKUP_TYPES and "flash512k" in Logic.BACKUP_TYPES)
	var backups := [
		{"path": "/saves/backups/game.sav.20261020T123456.000000002Z-000010.bak", "size": 65536},
		{"path": "/saves/backups/game.sav.20261020T123456.000000002Z-000009.bak", "size": 512},
		{"path": "/saves/backups/game.sav.20261020T123456.000000001Z-000000.bak", "size": 65536},
	]
	var backup_rows := Logic.save_backup_rows(backups, 65536)
	assert(backup_rows.size() == 3)
	for index in backups.size():
		assert(backup_rows[index].path == backups[index].path, "Keep native filename ordering, including same-second collision sequence")
	assert(backup_rows[0].label == "2026-10-20 12:34:56 UTC · 65536 B")
	assert(backup_rows[0].error.is_empty() and not backup_rows[1].error.is_empty())
	assert(Logic.save_restore_error(65536, 65536).is_empty())
	assert(Logic.save_restore_error(512, 65536).begins_with("Capacity mismatch"))
	assert(Logic.save_restore_error(524288, 65536).begins_with("Capacity mismatch"))
	assert(not Logic.save_restore_error(0, 65536).is_empty())
	assert(not Logic.save_restore_error(65536, 0).is_empty())
	assert(not Logic.save_restore_error(65536, -1).is_empty())
	assert(Logic.save_backup_rows([], 65536).is_empty())
	assert(Logic.save_backup_rows(backups, 65536, 2).size() == 2)
	assert(Logic.save_backup_rows(backups, 65536, 2)[1].path == backups[1].path)
	assert(Logic.save_backup_rows(backups, 65536, 0).is_empty())
	assert(backups[0].size == 65536 and backups[1].size == 512, "UI hints must not modify native entries or chip selection")
	var many_backups: Array = []
	for index in 60:
		many_backups.append({"path": "/backups/game.sav.20261020T123456.000000002Z-%06d.bak" % (99 - index), "size": 65536})
	var bounded_rows := Logic.save_backup_rows(many_backups, 65536)
	assert(bounded_rows.size() == 50 and bounded_rows[49].path == many_backups[49].path)
	assert(Logic.language_for_locale("fr_FR") == "fr" and Logic.language_for_locale("de-AT") == "de")
	assert(Logic.language_for_locale("it") == "it" and Logic.language_for_locale("ES_mx") == "es")
	assert(Logic.language_for_locale("ja_JP") == "en" and Logic.language_for_locale("") == "en")
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
	Logic.save_settings(path, {"layout":7, "single_screen_shapes":[shape]})
	settings = Logic.load_settings(path)
	assert(settings.layout == 7 and settings.single_screen_shapes == [shape])
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
