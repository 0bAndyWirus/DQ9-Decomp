extends SceneTree

const Logic = preload("res://scripts/host_logic.gd")
const WIDTH := 256
const HEIGHT := 192

func _initialize() -> void:
    call_deferred("_run")

func _argument(args: PackedStringArray, name: String) -> String:
    var index := args.find(name)
    return args[index + 1] if index >= 0 and index + 1 < args.size() else ""

func _run() -> void:
    var args := OS.get_cmdline_user_args()
    var rom := _argument(args, "--rom")
    var save_path := _argument(args, "--save")
    var input_path := _argument(args, "--input")
    var output_path := _argument(args, "--hashes")
    var frames_text := _argument(args, "--frames")
    var language := _argument(args, "--language")
    var epoch := _argument(args, "--rtc-epoch")
    var timezone := _argument(args, "--rtc-timezone-offset")
    if rom.is_empty() or save_path.is_empty() or input_path.is_empty() or output_path.is_empty() or not frames_text.is_valid_int() or int(frames_text) <= 0 or language.is_empty() or not epoch.is_valid_int() or not timezone.is_valid_int():
        print("FAIL: invalid arguments")
        quit(2)
        return
    var input_file := FileAccess.open(input_path, FileAccess.READ)
    if input_file == null:
        print("FAIL: cannot open input")
        quit(2)
        return
    var inputs := {}
    while not input_file.eof_reached():
        var parts := input_file.get_line().strip_edges().split(" ", false)
        if parts.size() == 4:
            inputs[int(parts[0])] = [int(parts[1]), int(parts[2]), int(parts[3])]
    input_file.close()
    var initial := Logic.DEFAULTS.duplicate(true)
    initial.rom_path = rom
    initial.save_path = save_path
    initial.language = language
    initial.renderer_gpu3d = 0
    Logic.save_settings("user://settings.cfg", initial)
    var app = load("res://scenes/main.tscn").instantiate()
    root.add_child(app)
    for button in app.find_children("*", "Button", true, false):
        if button.text == "Start" and button.custom_minimum_size.x == 180:
            button.pressed.emit()
            break
    if app.machine == null:
        print("FAIL: start machine")
        quit(2)
        return
    app.machine.stop()
    app.machine.load_game("/nonexistent/checkpoint-test.nds", save_path, Logic.firmware_language(language))
    app.machine.set_rtc_epoch(int(epoch), int(timezone))
    var error: String = app.machine.load_game(rom, save_path, Logic.firmware_language(language))
    if not error.is_empty():
        print("FAIL: load " + error)
        quit(2)
        return
    var output := FileAccess.open(output_path, FileAccess.WRITE)
    if output == null:
        print("FAIL: cannot write hashes")
        quit(2)
        return
    var frames := int(frames_text)
    for frame in range(frames):
        if args.has("--toggles"):
            if frame == frames / 6:
                app._set_layout(3)
            if frame == 2 * frames / 6:
                app.settings.integer_scale = true
                app._layout_screens()
                app.settings.nearest = false
                app._apply_filter()
            if frame == 3 * frames / 6:
                app.settings.window_scale = 2
                app.get_window().size = Vector2i(1280, 800) * 2
            if args.has("--gpu3d") and frame == 4 * frames / 6:
                app.settings.renderer_gpu3d = 1
                app._apply_gpu3d_settings()
            if args.has("--gpu3d") and frame == 5 * frames / 6:
                app.settings.renderer_gpu3d = 0
                app._apply_gpu3d_settings()
        var state: Array = inputs.get(frame, [0, -1, -1])
        var x: int = state[1]
        var y: int = state[2]
        app.machine.step_frames(1, int(state[0]), x, y, x >= 0 and y >= 0)
        await app.machine.frame_ready
        output.store_line("%d %s %s" % [frame, _hash_hex(_hash_rgb(app.machine.get_top_image())), _hash_hex(_hash_rgb(app.machine.get_bottom_image()))])
    app.machine.flush_save()
    output.close()
    print("PASS: %d frames written" % frames)
    quit(0)

func _hash_rgb(image: Image) -> int:
    var data := image.get_data()
    var value: int = -3750763034362895579
    for pixel in range(WIDTH * HEIGHT):
        var base := pixel * 4
        for component in [0, 1, 2]:
            value = (value ^ int(data[base + component])) * 0x100000001b3
    return value

func _hash_hex(value: int) -> String:
    return "%08x%08x" % [(value >> 32) & 0xffffffff, value & 0xffffffff]
