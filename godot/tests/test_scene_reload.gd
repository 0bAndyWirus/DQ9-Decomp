extends SceneTree

# The input script must drive at least two scene transitions within the given
# frame count. Use disposable save copies: loading and stepping may write them.
var machine
var rom: String
var save_path: String
var reload_on: String
var reload_error := ""
var reloaded := false
var scenes_after_reload: Array[int] = []
var delivered_scenes: Array[int] = []

func _initialize() -> void:
    call_deferred("_run")

func _on_frame(_frame: int) -> void:
    if reload_on == "frame" and not reloaded:
        reloaded = true
        reload_error = machine.load_game(rom, save_path, 1)

func _on_scene(scene: int) -> void:
    delivered_scenes.append(scene)
    if reloaded:
        scenes_after_reload.append(scene)
    elif reload_on == "scene":
        reloaded = true
        reload_error = machine.load_game(rom, save_path, 1)

func _run() -> void:
    var args := OS.get_cmdline_user_args()
    if args.size() != 5:
        push_error("usage: test_scene_reload.gd -- <rom> <frame-reload-save-copy> <scene-reload-save-copy> <input-script> <frames>")
        quit(2)
        return
    rom = args[0]
    var input_file := FileAccess.open(args[3], FileAccess.READ)
    if input_file == null:
        push_error("cannot open input script: " + args[3])
        quit(2)
        return
    var inputs := {}
    while not input_file.eof_reached():
        var parts := input_file.get_line().strip_edges().split(" ", false)
        if parts.size() == 4:
            inputs[int(parts[0])] = [int(parts[1]), int(parts[2]), int(parts[3])]
    if not ClassDB.class_exists("DqMachine"):
        GDExtensionManager.load_extension("res://dq9.gdextension")
    machine = ClassDB.instantiate("DqMachine")
    if machine == null:
        push_error("DqMachine GDExtension class is unavailable")
        quit(3)
        return
    root.add_child(machine)
    machine.set_rtc_epoch(0, 0)
    machine.frame_ready.connect(_on_frame)
    machine.scene_changed.connect(_on_scene)
    for mode in ["frame", "scene"]:
        reload_on = ""
        save_path = args[1] if mode == "frame" else args[2]
        var error: String = machine.load_game(rom, save_path, 1)
        if not error.is_empty():
            push_error(error)
            quit(4)
            return
        machine.set_paused(true)
        var previous: int = machine.get_scene()
        var transitions := 0
        for frame in range(int(args[4])):
            var state: Array = inputs.get(frame, [0, 0, 0])
            machine.step_frames(1, int(state[0]), int(state[1]), int(state[2]),
                inputs.has(frame) and int(state[1]) >= 0 and int(state[2]) >= 0)
            var scene: int = machine.get_scene()
            if scene != previous:
                transitions += 1
                previous = scene
        if transitions < 2:
            push_error("fixture must produce at least two scene transitions; got %d" % transitions)
            quit(5)
            return
        reloaded = false
        reload_error = ""
        scenes_after_reload.clear()
        delivered_scenes.clear()
        reload_on = mode
        await process_frame
        await process_frame
        reload_on = ""
        if not reloaded or not reload_error.is_empty() or not scenes_after_reload.is_empty() or machine.get_scene() != 0:
            push_error("%s reload failed: reloaded=%s error=%s stale=%s scene=%d" % [mode, reloaded, reload_error, scenes_after_reload, machine.get_scene()])
            quit(6)
            return
        if (mode == "frame" and not delivered_scenes.is_empty()) or (mode == "scene" and delivered_scenes.size() != 1):
            push_error("%s reload delivered unexpected scenes: %s" % [mode, delivered_scenes])
            quit(7)
            return
    machine.queue_free()
    await process_frame
    quit(0)
