extends SceneTree

const Logic = preload("res://scripts/host_logic.gd")

const WIDTH := 256
const HEIGHT := 192

func _initialize() -> void:
    call_deferred("_run")

func _run() -> void:
    var args := OS.get_cmdline_user_args()
    if args.size() < 5 or args.size() > 6:
        push_error("usage: Godot --headless --path godot --script res://tests/smoke.gd -- <rom> <save-copy> <input-script> <frames> <hash-out> [native-hash-file]")
        quit(2)
        return
    var rom: String = args[0]
    var save_path: String = args[1]
    var script_path: String = args[2]
    var frames: int = int(args[3])
    var output_path: String = args[4]
    var input_by_frame: Dictionary = {}
    var input_file := FileAccess.open(script_path, FileAccess.READ)
    if input_file == null:
        push_error("cannot open input script: " + script_path)
        quit(2)
        return
    while not input_file.eof_reached():
        var parts := input_file.get_line().strip_edges().split(" ", false)
        if parts.size() == 4:
            var f := int(parts[0])
            input_by_frame[f] = [int(parts[1]), int(parts[2]), int(parts[3])]
    input_file.close()

    if not ClassDB.class_exists("DqMachine"):
        GDExtensionManager.load_extension("res://dq9.gdextension")
    var machine = ClassDB.instantiate("DqMachine")
    if machine == null:
        push_error("DqMachine GDExtension class is unavailable")
        quit(3)
        return
    root.add_child(machine)
    machine.set_rtc_epoch(0, 0)
    var error: String = machine.load_game(rom, save_path, 1)
    if not error.is_empty():
        push_error(error)
        _destroy_machine(machine)
        await process_frame
        quit(4)
        return
    machine.set_paused(true)
    var hashes := FileAccess.open(output_path, FileAccess.WRITE)
    if hashes == null:
        push_error("cannot write hashes: " + output_path)
        _destroy_machine(machine)
        await process_frame
        quit(5)
        return
    for frame in range(frames):
        var state: Array = input_by_frame.get(frame, [0, 0, 0])
        var action_names: Array[String] = []
        for i in Logic.BUTTON_BITS.size():
            if int(state[0]) & (1 << Logic.BUTTON_BITS[i]):
                action_names.append(Logic.BUTTON_ACTIONS[i])
        var mask: int = Logic.button_mask(action_names)
        var x: int = state[1]
        var y: int = state[2]
        machine.step_frames(1, mask, x, y, input_by_frame.has(frame) and x >= 0 and y >= 0)
        await machine.frame_ready
        var top: Image = machine.get_top_image()
        var bottom: Image = machine.get_bottom_image()
        hashes.store_line("%d %s %s" % [frame, _hash_hex(_hash_rgb(top)), _hash_hex(_hash_rgb(bottom))])
    hashes.close()
    var parity_ok := true
    if args.size() == 6:
        parity_ok = _compare_hash_files(args[5], output_path, frames)
    machine.set_speed(0.0)
    machine.start()
    await machine.frame_ready
    machine.set_paused(true)
    await create_timer(0.1).timeout
    var frozen_frame: int = int(machine.get_stats()["frame"])
    await create_timer(0.5).timeout
    if int(machine.get_stats()["frame"]) != frozen_frame:
        push_error("pause did not freeze the frame counter")
        _destroy_machine(machine)
        await process_frame
        quit(6)
        return
    machine.step_once()
    var step_deadline := Time.get_ticks_msec() + 5000
    while int(machine.get_stats()["frame"]) == frozen_frame and Time.get_ticks_msec() < step_deadline:
        await process_frame
    if int(machine.get_stats()["frame"]) != frozen_frame + 1:
        push_error("step_once did not advance exactly one frame")
        _destroy_machine(machine)
        await process_frame
        quit(7)
        return
    machine.set_speed(1.0)
    machine.start()
    await create_timer(2.0).timeout
    var initial_underruns: int = int(machine.get_stats()["underruns"])
    var initial_audio_frames: int = int(machine.get_stats()["audio_frames_pushed"])
    var realtime_start_frame: int = int(machine.get_stats()["frame"])
    await create_timer(20.0).timeout
    var final_stats: Dictionary = machine.get_stats()
    var realtime_frames: int = int(final_stats["frame"]) - realtime_start_frame
    # Session emits about SPU_rate / frame_rate source frames per emulated frame.
    # Resampling to Godot's mix rate therefore yields mix_rate / frame_rate output
    # frames per emulated frame; compare only frames produced in this realtime window.
    var expected_audio_frames: int = int(round(
        float(realtime_frames) * AudioServer.get_mix_rate() / 59.8261))
    var audio_tolerance: int = maxi(4096, int(ceil(expected_audio_frames * 0.02)))
    var audio_frames_received: int = int(final_stats["audio_frames_pushed"]) - initial_audio_frames
    print("Realtime audio: %d pushed / %d expected from %d emulated frames; underruns=%d" % [
        audio_frames_received, expected_audio_frames, realtime_frames,
        int(final_stats["underruns"]) - initial_underruns])
    if int(final_stats["underruns"]) != initial_underruns:
        push_error("audio underruns during realtime interval: %d; stats=%s" % [
            int(final_stats["underruns"]) - initial_underruns, str(final_stats)])
        _destroy_machine(machine)
        await process_frame
        quit(8)
        return
    if abs(audio_frames_received - expected_audio_frames) > audio_tolerance:
        push_error("AudioStreamGenerator sample count deviated from frame-derived expectation by more than %d frames" % audio_tolerance)
        _destroy_machine(machine)
        await process_frame
        quit(10)
        return
    if realtime_frames < 1100 or realtime_frames > 1300:
        push_error("realtime pacing outside expected 20-second frame range: %d" % realtime_frames)
        _destroy_machine(machine)
        await process_frame
        quit(9)
        return
    var save_ok := _destroy_machine(machine)
    await process_frame
    if not save_ok:
        quit(12)
        return
    print("PASS: pause, exact step, 20s realtime audio/pacing, and clean shutdown")
    if not parity_ok:
        quit(11)
        return
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

func _compare_hash_files(native_path: String, godot_path: String, frames: int) -> bool:
    var native_text := FileAccess.get_file_as_string(native_path)
    var godot_text := FileAccess.get_file_as_string(godot_path)
    var native_lines := native_text.strip_edges().split("\n", false)
    var godot_lines := godot_text.strip_edges().split("\n", false)
    if native_lines.size() != frames or godot_lines.size() != frames:
        push_error("hash line count mismatch: native=%d Godot=%d expected=%d" % [
            native_lines.size(), godot_lines.size(), frames])
        return false
    for frame in range(frames):
        if native_lines[frame].strip_edges() != godot_lines[frame].strip_edges():
            push_error("hash mismatch at frame %d: native=%s Godot=%s" % [
                frame, native_lines[frame], godot_lines[frame]])
            return false
    print("PASS: %d frames; top and bottom hashes identical to native dq9" % frames)
    return true

func _destroy_machine(machine: Node) -> bool:
    if is_instance_valid(machine):
        machine.stop()
        machine.set_paused(true)
        var save_ok: bool = machine.flush_save()
        if not save_ok: push_error("save flush failed during smoke cleanup")
        machine.free()
        # Give Godot's audio mixer thread time to retire its generator playback before quit.
        OS.delay_msec(500)
        return save_ok
    return false
