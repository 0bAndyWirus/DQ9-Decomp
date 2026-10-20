#include <godot_cpp/classes/display_server.hpp>
#include <godot_cpp/classes/audio_server.hpp>
#include <godot_cpp/classes/audio_stream_generator.hpp>
#include <godot_cpp/classes/audio_stream_generator_playback.hpp>
#include <godot_cpp/classes/audio_stream_player.hpp>
#include <godot_cpp/classes/image.hpp>
#include <godot_cpp/classes/image_texture.hpp>
#include <godot_cpp/classes/node.hpp>
#include <godot_cpp/classes/texture2d.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/packed_vector2_array.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/vector2.hpp>
#include <godot_cpp/variant/packed_string_array.hpp>
#include <godot_cpp/godot.hpp>

#include "dsr/session_c.h"
#include "dsr/session_render_frame.h"
#include "dq_gpu_scene.h"
#include "gpu_frame_delay.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <mutex>
#include <memory>
#include <string>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <iterator>
#include <thread>
#include <vector>
#include <unordered_map>
#include <unordered_set>

namespace {
constexpr int kWidth = 256;
constexpr int kHeight = 192;
constexpr std::size_t kPixels = kWidth * kHeight;
constexpr std::size_t kRingFrames = 32728;
constexpr double kGeneratorBufferSeconds = 0.1;
constexpr double kAudioPrimeSeconds = 0.040;
constexpr double kFrameRate = 59.8261;
constexpr double kAudioRate = 33513982.0 / 1024.0;
struct FrameSlot {
    std::array<std::uint8_t, kPixels * 4> screens[2]{};
    std::uint64_t number = 0;
    std::unique_ptr<dq9::GpuSceneFrame> gpu;
};
struct RecordedInput {
    std::uint64_t frame = 0;
    dsr_input_state input{};
};
struct ReplayHash {
    std::uint64_t frame, top, bottom;
};
struct SaveBackup {
    std::string path;
    std::int64_t size;
};
struct WorkerTexture {
    std::shared_ptr<dq9::DecodedTexture> decoded;
    std::uint64_t last_seen_frame = 0;
};
struct WorkerTextureCache {
    using Map = std::unordered_map<dsr::gpu3d_detail::TextureKey, WorkerTexture, dq9::TextureKeyHash>;
    Map textures;
    std::vector<const Map::value_type*> referenced;
    std::uint64_t epoch = 0, generation = 0;
    bool initialized = false;
};
}

class DqMachine;
namespace {
// Intrusive, main-thread-only list: no Godot objects or joining destructors in
// static storage. Module teardown can release even nodes outside the SceneTree.
DqMachine* live_machines = nullptr;
}

class DqMachine final : public godot::Node {
    GDCLASS(DqMachine, godot::Node)
public:
    DqMachine() : ring_(kRingFrames * 2), next_instance_(live_machines) {
        if (next_instance_) next_instance_->previous_instance_ = this;
        live_machines = this;
    }
    ~DqMachine() override {
        // PREDELETE normally ran before Godot destroyed the children. The
        // fallback must not dereference their already-freed instance bindings.
        release_resources(false);
        if (previous_instance_) previous_instance_->next_instance_ = next_instance_;
        else live_machines = next_instance_;
        if (next_instance_) next_instance_->previous_instance_ = previous_instance_;
    }
    static void shutdown_instances() {
        for (auto* instance = live_machines; instance; instance = instance->next_instance_)
            instance->release_resources(true);
    }
    void _notification(int what) {
        if (what == NOTIFICATION_PREDELETE) release_resources(true);
    }

    godot::String load_game(const godot::String& rom_path, const godot::String& save_path, int language) {
        stop();
        shutdown_worker();
        if (gpu_scene_) { memdelete(gpu_scene_); gpu_scene_ = nullptr; }
        for (auto& slot : frames_) slot.gpu.reset();
        if (language < 0 || language > 5) return "language must be in range 0..5";
        const godot::CharString rom = rom_path.utf8();
        const godot::CharString save = save_path.utf8();
        auto initial_save = read_start_save(save.get_data());
        {
            std::lock_guard lock(mutex_);
            rom_path_ = rom.get_data(); save_path_ = save.get_data();
            session_start_save_ = std::move(initial_save);
            language_ = static_cast<std::uint8_t>(language);
            input_ = {0, 0, 0, 0, 0};
            chip_mismatch_ = false;
            last_save_ok_ = true; flush_requested_ = false;
            backups_requested_ = restore_requested_ = false;
            save_backups_.clear(); restore_path_.clear(); restore_error_.clear();
            save_backup_error_.clear();
            save_capacity_ = 0; started_ = false;
            error_.clear(); running_ = false; paused_ = false;
            {
                std::lock_guard audio_lock(audio_mutex_);
                ring_read_ = ring_write_ = 0; underruns_ = dropped_audio_frames_ = 0;
                audio_frames_pushed_ = 0; resample_phase_ = 0.0; audio_priming_ = true;
            }
            published_slot_ = 0; consuming_slot_ = -1;
            ++gpu_epoch_;
            installed_texture_keys_.clear();
            input_history_.clear();
            input_history_.reserve(4096);
            input_script_.clear(); script_cursor_ = 0;
            replay_active_ = replay_finished_pending_ = false;
            replay_hashes_.clear();
            pending_steps_ = sync_frames_ = 0;
        }
        if (audio_player_) {
            audio_player_->stop();
            audio_player_->play();
            reset_playback();
            audio_player_->set_stream_paused(true);
        }
        owner_thread_ = std::thread(&DqMachine::worker_loop, this);
        std::unique_lock lock(mutex_);
        cv_.wait(lock, [&] { return initialized_; });
        return loaded_ ? godot::String("") : godot::String(error_.c_str());
    }
    void start() {
        std::lock_guard lock(mutex_);
        if (!loaded_) return;
        started_ = true;
        running_ = true; paused_ = false; cv_.notify_all();
        if (audio_player_ && !audio_priming_) audio_player_->set_stream_paused(false);
    }
    void stop() {
        std::lock_guard lock(mutex_);
        running_ = false; paused_ = true; cv_.notify_all();
        if (audio_player_) audio_player_->set_stream_paused(true);
    }
    void shutdown_worker() {
        {
            std::lock_guard lock(mutex_);
            if (!owner_thread_.joinable()) return;
            shutdown_ = true; cv_.notify_all();
        }
        owner_thread_.join();
        std::lock_guard lock(mutex_);
        loaded_ = false; initialized_ = false; shutdown_ = false;
    }
    void set_paused(bool value) {
        std::lock_guard lock(mutex_);
        paused_ = value; cv_.notify_all();
        if (audio_player_) audio_player_->set_stream_paused(value || audio_priming_);
    }
    bool is_paused() const { std::lock_guard lock(mutex_); return paused_; }
    void step_once() {
        std::lock_guard lock(mutex_);
        if (loaded_ && (paused_ || !running_)) {
            started_ = true; ++pending_steps_; cv_.notify_all();
        }
    }
    void step_frames(int n, int mask, int x, int y, bool down) {
        if (n <= 0 || n > 1000000) return;
        std::unique_lock lock(mutex_);
        if (!loaded_ || running_) { error_ = "step_frames requires a loaded, stopped session"; return; }
        started_ = true;
        input_ = {static_cast<std::uint16_t>(mask), x < 0 ? -1 : std::clamp(x, 0, 255),
                  y < 0 ? -1 : std::clamp(y, 0, 191), static_cast<std::uint8_t>(down), 0};
        sync_frames_ = n; sync_done_ = false; cv_.notify_all();
        cv_.wait(lock, [&] { return sync_done_ || shutdown_; });
    }
    void set_speed(float value) {
        std::lock_guard lock(mutex_);
        speed_ = std::isfinite(value) ? std::clamp(value, 0.0f, 16.0f) : 1.0f;
        cv_.notify_all();
    }
    // Input is selected on the session owner thread immediately before each
    // step, never by Godot's render-frame cadence. Missing rows mean no input,
    // and the last duplicate row wins, exactly like dq9 --input-script.
    godot::String set_input_script(const godot::PackedStringArray& rows) {
        std::lock_guard lock(mutex_);
        if (!loaded_ || running_ || frame_count_ != 0)
            return "set_input_script requires a freshly loaded, stopped session";
        std::vector<RecordedInput> parsed;
        parsed.reserve(rows.size());
        for (int64_t i = 0; i < rows.size(); ++i) {
            const auto bytes = rows[i].utf8();
            std::istringstream row(bytes.get_data());
            std::int64_t frame;
            unsigned mask;
            int x, y;
            if (rows[i].strip_edges().is_empty()) continue;
            if (!(row >> frame >> mask >> x >> y) || frame < 0)
                return godot::String("invalid input row: ") + rows[i];
            parsed.push_back({static_cast<std::uint64_t>(frame),
                              {static_cast<std::uint16_t>(mask), x, y,
                               static_cast<std::uint8_t>(x >= 0), 0}});
        }
        std::stable_sort(parsed.begin(), parsed.end(),
                         [](const RecordedInput& a, const RecordedInput& b) { return a.frame < b.frame; });
        std::size_t count = 0;
        for (const auto& record : parsed) {
            if (count && parsed[count - 1].frame == record.frame) parsed[count - 1] = record;
            else parsed[count++] = record;
        }
        parsed.resize(count);
        input_script_ = std::move(parsed); script_cursor_ = 0;
        return "";
    }
    godot::String start_replay(std::int64_t target_frame, bool record_hashes = false) {
        std::lock_guard lock(mutex_);
        if (!loaded_ || running_ || frame_count_ != 0 || target_frame < 0)
            return "start_replay requires a fresh stopped session and a nonnegative note frame";
        replay_target_ = static_cast<std::uint64_t>(target_frame);
        replay_hashes_.clear();
        record_replay_hashes_ = record_hashes;
        if (record_hashes) replay_hashes_.reserve(replay_target_ + 1);
        script_cursor_ = 0;
        started_ = true;
        replay_active_ = true; replay_finished_pending_ = false;
        running_ = true; paused_ = false;
        input_ = {};
        if (audio_player_) audio_player_->set_stream_paused(true);
        cv_.notify_all();
        return "";
    }
    void cancel_replay() {
        std::lock_guard lock(mutex_);
        replay_active_ = replay_finished_pending_ = false;
        running_ = false; paused_ = true;
        input_ = {};
        if (audio_player_) audio_player_->set_stream_paused(true);
        cv_.notify_all();
    }
    godot::PackedStringArray get_replay_hashes() const {
        std::lock_guard lock(mutex_);
        godot::PackedStringArray rows;
        for (const auto& hash : replay_hashes_) {
            char row[80];
            std::snprintf(row, sizeof(row), "%llu %016llx %016llx",
                          static_cast<unsigned long long>(hash.frame),
                          static_cast<unsigned long long>(hash.top),
                          static_cast<unsigned long long>(hash.bottom));
            rows.append(row);
        }
        return rows;
    }
    void set_gpu3d_enabled(bool enabled) {
        {
            std::lock_guard lock(mutex_);
            if (gpu_enabled_ == enabled) return;
            gpu_enabled_ = enabled;
            ++gpu_epoch_;
            installed_texture_keys_.clear();
        }
        if (!enabled && gpu_scene_) { memdelete(gpu_scene_); gpu_scene_ = nullptr; }
    }
    void set_gpu3d_scale(int scale) {
        gpu_scale_ = std::clamp(scale, 1, 6);
        if (gpu_scene_) gpu_scene_->set_scale(gpu_scale_);
    }
    godot::Ref<godot::Texture2D> get_gpu3d_texture() const {
        return gpu_scene_ ? gpu_scene_->get_texture() : godot::Ref<godot::Texture2D>();
    }
    godot::Ref<godot::Texture2D> get_gpu3d_mask_texture() const {
        return gpu_scene_ ? gpu_scene_->get_mask_texture() : godot::Ref<godot::Texture2D>();
    }
    bool get_gpu3d_screen_is_top() const {
        return gpu_scene_ ? gpu_scene_->get_screen_is_top() : true;
    }
    godot::Dictionary get_gpu3d_stats() const {
        if (gpu_scene_) return gpu_scene_->get_stats();
        godot::Dictionary out;
        out["valid"] = false;
        out["fallback"] = "disabled";
        out["surface_count"] = 0;
        out["textures_cached"] = 0;
        return out;
    }
    // Cartridge save chip name as `--backup-type` ("auto", "eeprom64k", ...); call before load_game.
    // An unknown name makes load_game report the error.
    void set_backup_type(const godot::String& name) {
        std::lock_guard lock(mutex_);
        backup_type_ = name.utf8().get_data();
    }
    godot::String get_backup_type() const { return godot::String::utf8(backup_type_.c_str()); }
    void set_rtc_epoch(std::int64_t epoch_seconds, int timezone_offset_minutes) {
        std::lock_guard lock(mutex_);
        if (loaded_ || timezone_offset_minutes < -840 || timezone_offset_minutes > 840) return;
        deterministic_rtc_ = true;
        rtc_epoch_seconds_ = epoch_seconds;
        rtc_timezone_offset_minutes_ = timezone_offset_minutes;
    }
    void set_buttons(int mask) { std::lock_guard lock(mutex_); input_.buttons = static_cast<std::uint16_t>(mask); }
    void set_touch(int x, int y, bool down) {
        std::lock_guard lock(mutex_);
        input_.pen_x = std::clamp(x, 0, 255);
        input_.pen_y = std::clamp(y, 0, 191);
        input_.pen_down = static_cast<std::uint8_t>(down);
    }
    godot::PackedByteArray get_session_start_save() const {
        std::lock_guard lock(mutex_);
        godot::PackedByteArray out;
        out.resize(static_cast<int64_t>(session_start_save_.size()));
        if (!session_start_save_.empty())
            std::memcpy(out.ptrw(), session_start_save_.data(), session_start_save_.size());
        return out;
    }
    godot::PackedStringArray get_input_history() const {
        std::lock_guard lock(mutex_);
        godot::PackedStringArray out;
        for (const RecordedInput& record : input_history_) {
            std::ostringstream line;
            line << record.frame << ' ' << record.input.buttons << ' '
                 << (record.input.pen_down ? record.input.pen_x : -1) << ' '
                 << (record.input.pen_down ? record.input.pen_y : -1);
            out.append(godot::String(line.str().c_str()));
        }
        return out;
    }
    std::int64_t get_rtc_epoch() const { return rtc_epoch_seconds_; }
    int get_rtc_timezone_offset_minutes() const { return rtc_timezone_offset_minutes_; }
    godot::Ref<godot::Texture2D> get_top_texture() const { return top_texture_; }
    godot::Ref<godot::Texture2D> get_bottom_texture() const { return bottom_texture_; }
    godot::AudioStreamPlayer* get_audio_player() const { return audio_player_; }
    godot::Ref<godot::Image> get_top_image() const { return top_image_; }
    godot::Ref<godot::Image> get_bottom_image() const { return bottom_image_; }
    bool flush_save() {
        std::unique_lock lock(mutex_);
        if (!loaded_ || shutdown_) return false;
        flush_requested_ = true; cv_.notify_all();
        // Shutdown also completes the request with its final owner-thread flush.
        cv_.wait(lock, [&] { return !flush_requested_; });
        return last_save_ok_;
    }
    godot::Array get_save_backups() {
        std::unique_lock lock(mutex_);
        godot::Array out;
        if (!loaded_ || shutdown_) {
            save_backup_error_ = "save backup listing requires a loaded session";
            return out;
        }
        backups_requested_ = true; cv_.notify_all();
        cv_.wait(lock, [&] { return !backups_requested_; });
        for (const auto& backup : save_backups_) {
            godot::Dictionary entry;
            entry["path"] = godot::String::utf8(backup.path.c_str());
            entry["size"] = backup.size;
            out.append(entry);
        }
        return out;
    }
    godot::String get_save_backup_error() const {
        std::lock_guard lock(mutex_);
        return godot::String::utf8(save_backup_error_.c_str());
    }
    std::int64_t get_save_capacity() const {
        std::lock_guard lock(mutex_);
        return loaded_ && !shutdown_ ? static_cast<std::int64_t>(save_capacity_) : 0;
    }
    godot::String restore_save(const godot::String& path) {
        std::unique_lock lock(mutex_);
        if (!loaded_ || shutdown_) return "restore_save requires a loaded session";
        if (started_ || running_ || paused_)
            return "restore_save requires a freshly loaded, unstarted session";
        restore_path_ = path.utf8().get_data();
        restore_error_.clear();
        restore_requested_ = true; cv_.notify_all();
        cv_.wait(lock, [&] { return !restore_requested_; });
        return godot::String::utf8(restore_error_.c_str());
    }
    godot::Dictionary get_stats() const {
        std::lock_guard lock(mutex_);
        godot::Dictionary out;
        out["frame"] = static_cast<std::int64_t>(frame_count_);
        {
            std::lock_guard audio_lock(audio_mutex_);
            const std::size_t queued = ring_write_ - ring_read_;
            out["audio_queue_ms"] = 1000.0 * static_cast<double>(queued) / audio_source_rate_;
            out["dropped_audio_ms"] = 1000.0 * static_cast<double>(dropped_audio_frames_) / audio_source_rate_;
            out["underruns"] = static_cast<std::int64_t>(underruns_);
            out["audio_frames_pushed"] = static_cast<std::int64_t>(audio_frames_pushed_);
        }
        out["running"] = running_;
        out["loaded"] = loaded_;
        out["last_save_ok"] = last_save_ok_;
        out["chip_mismatch"] = chip_mismatch_;
        out["paused"] = paused_;
        out["replay_active"] = replay_active_;
        out["replay_target"] = static_cast<std::int64_t>(replay_target_);
        return out;
    }

    void _ready() override {
        top_image_ = godot::Image::create(kWidth, kHeight, false, godot::Image::FORMAT_RGBA8);
        bottom_image_ = godot::Image::create(kWidth, kHeight, false, godot::Image::FORMAT_RGBA8);
        top_texture_ = godot::ImageTexture::create_from_image(top_image_);
        for (auto& pair : screen_bytes_)
            for (auto& bytes : pair) bytes.resize(kPixels * 4);
        bottom_texture_ = godot::ImageTexture::create_from_image(bottom_image_);
        audio_stream_.instantiate();
        audio_target_seconds_ = godot::DisplayServer::get_singleton()->get_name() == godot::String("headless")
                                    ? kGeneratorBufferSeconds : kAudioPrimeSeconds;
        audio_stream_->set_mix_rate(static_cast<float>(godot::AudioServer::get_singleton()->get_mix_rate()));
        audio_stream_->set_buffer_length(kGeneratorBufferSeconds);
        audio_player_ = memnew(godot::AudioStreamPlayer);
        audio_player_->set_stream(audio_stream_);
        add_child(audio_player_);
        audio_player_->play();
        reset_playback();
        audio_player_->set_stream_paused(true);
    }
    void _process(double delta) override {
        if (resources_released_) return;
        bool changed = false;
        bool play_audio = false;
        int read_slot = -1;
        std::uint64_t number = 0;
        bool notify_replay = false;
        std::uint64_t replay_frame = 0;
        {
            std::lock_guard lock(mutex_);
            if (published_slot_ >= 0) {
                consuming_slot_ = published_slot_;
                published_slot_ = -1;
                read_slot = consuming_slot_;
                number = frames_[read_slot].number;
                changed = true;
                if (replay_finished_pending_ && number == replay_target_ + 1) {
                    replay_finished_pending_ = false;
                    notify_replay = true; replay_frame = replay_target_;
                }
            }
            if (delta > 0.0) fps_ = 0.9 * fps_ + 0.1 / delta;
            play_audio = running_ && !paused_ && !replay_active_;
        }
        underruns_ = static_cast<std::uint64_t>(audio_playback_.is_valid() ? audio_playback_->get_skips() : 0);
        // Paused/manual stepping must not drain the generator or count silent
        // mixer skips. Retain its queued samples for a seamless realtime resume.
        if (play_audio) pull_audio();
        if (changed) {
            screen_bytes_index_ = 1 - screen_bytes_index_;
            auto& bytes = screen_bytes_[screen_bytes_index_];
            for (int screen = 0; screen < 2; ++screen)
                std::memcpy(bytes[screen].ptrw(), frames_[read_slot].screens[screen].data(), kPixels * 4);
            bool consume_gpu = false;
            {
                std::lock_guard lock(mutex_);
                consume_gpu = gpu_enabled_ && frames_[read_slot].gpu &&
                              frames_[read_slot].gpu->epoch == gpu_epoch_;
            }
            if (consume_gpu) {
                const auto& frame = *frames_[read_slot].gpu;
                if (!gpu_scene_) {
                    gpu_scene_ = memnew(DqGpuScene);
                    gpu_scene_->set_name("Gpu3DScene");
                    gpu_scene_->set_scale(gpu_scale_);
                    add_child(gpu_scene_);
                }
                gpu_scene_->apply_frame(frame);
                std::lock_guard lock(mutex_);
                if (gpu_enabled_ && gpu_epoch_ == frame.epoch) {
                    if (installed_texture_epoch_ != frame.epoch ||
                        installed_texture_generation_ != frame.vram_generation) {
                        installed_texture_keys_.clear();
                        installed_texture_epoch_ = frame.epoch;
                        installed_texture_generation_ = frame.vram_generation;
                    }
                    for (const auto& texture : frame.new_textures)
                        installed_texture_keys_.insert(texture->key);
                }
            }
            {
                std::lock_guard lock(mutex_);
                consuming_slot_ = -1;
            }
            top_image_->set_data(kWidth, kHeight, false, godot::Image::FORMAT_RGBA8, bytes[0]);
            bottom_image_->set_data(kWidth, kHeight, false, godot::Image::FORMAT_RGBA8, bytes[1]);
            top_texture_->update(top_image_);
            bottom_texture_->update(bottom_image_);
            emit_signal("frame_ready", static_cast<std::int64_t>(number));
            if (notify_replay) {
                if (audio_player_) audio_player_->set_stream_paused(true);
                emit_signal("replay_finished", static_cast<std::int64_t>(replay_frame));
            }
        }
        std::string err;
        { std::lock_guard lock(mutex_); err.swap(error_); }
        if (!err.empty()) emit_signal("error", godot::String(err.c_str()));
    }

protected:
    static void _bind_methods() {
        godot::ClassDB::bind_method(godot::D_METHOD("load_game", "rom_path", "save_path", "language"), &DqMachine::load_game);
        godot::ClassDB::bind_method(godot::D_METHOD("start"), &DqMachine::start);
        godot::ClassDB::bind_method(godot::D_METHOD("stop"), &DqMachine::stop);
        godot::ClassDB::bind_method(godot::D_METHOD("set_paused", "paused"), &DqMachine::set_paused);
        godot::ClassDB::bind_method(godot::D_METHOD("is_paused"), &DqMachine::is_paused);
        godot::ClassDB::bind_method(godot::D_METHOD("step_once"), &DqMachine::step_once);
        godot::ClassDB::bind_method(godot::D_METHOD("step_frames", "n", "mask", "x", "y", "down"), &DqMachine::step_frames);
        godot::ClassDB::bind_method(godot::D_METHOD("set_speed", "speed"), &DqMachine::set_speed);
        godot::ClassDB::bind_method(godot::D_METHOD("set_input_script", "rows"), &DqMachine::set_input_script);
        godot::ClassDB::bind_method(godot::D_METHOD("start_replay", "target_frame", "record_hashes"),
                                   &DqMachine::start_replay, DEFVAL(false));
        godot::ClassDB::bind_method(godot::D_METHOD("cancel_replay"), &DqMachine::cancel_replay);
        godot::ClassDB::bind_method(godot::D_METHOD("get_replay_hashes"), &DqMachine::get_replay_hashes);
        godot::ClassDB::bind_method(godot::D_METHOD("set_backup_type", "name"), &DqMachine::set_backup_type);
        godot::ClassDB::bind_method(godot::D_METHOD("get_backup_type"), &DqMachine::get_backup_type);
        godot::ClassDB::bind_method(godot::D_METHOD("set_rtc_epoch", "epoch_seconds", "timezone_offset_minutes"), &DqMachine::set_rtc_epoch);
        godot::ClassDB::bind_method(godot::D_METHOD("set_buttons", "mask"), &DqMachine::set_buttons);
        godot::ClassDB::bind_method(godot::D_METHOD("set_touch", "x", "y", "down"), &DqMachine::set_touch);
        godot::ClassDB::bind_method(godot::D_METHOD("get_top_texture"), &DqMachine::get_top_texture);
        godot::ClassDB::bind_method(godot::D_METHOD("get_bottom_texture"), &DqMachine::get_bottom_texture);
        godot::ClassDB::bind_method(godot::D_METHOD("get_top_image"), &DqMachine::get_top_image);
        godot::ClassDB::bind_method(godot::D_METHOD("get_bottom_image"), &DqMachine::get_bottom_image);
        godot::ClassDB::bind_method(godot::D_METHOD("get_audio_player"), &DqMachine::get_audio_player);
        godot::ClassDB::bind_method(godot::D_METHOD("flush_save"), &DqMachine::flush_save);
        godot::ClassDB::bind_method(godot::D_METHOD("get_save_backups"), &DqMachine::get_save_backups);
        godot::ClassDB::bind_method(godot::D_METHOD("get_save_backup_error"), &DqMachine::get_save_backup_error);
        godot::ClassDB::bind_method(godot::D_METHOD("get_save_capacity"), &DqMachine::get_save_capacity);
        godot::ClassDB::bind_method(godot::D_METHOD("restore_save", "path"), &DqMachine::restore_save);
        godot::ClassDB::bind_method(godot::D_METHOD("get_stats"), &DqMachine::get_stats);
        godot::ClassDB::bind_method(godot::D_METHOD("set_gpu3d_enabled", "enabled"), &DqMachine::set_gpu3d_enabled);
        godot::ClassDB::bind_method(godot::D_METHOD("set_gpu3d_scale", "scale"), &DqMachine::set_gpu3d_scale);
        godot::ClassDB::bind_method(godot::D_METHOD("get_gpu3d_texture"), &DqMachine::get_gpu3d_texture);
        godot::ClassDB::bind_method(godot::D_METHOD("get_gpu3d_mask_texture"), &DqMachine::get_gpu3d_mask_texture);
        godot::ClassDB::bind_method(godot::D_METHOD("get_gpu3d_screen_is_top"), &DqMachine::get_gpu3d_screen_is_top);
        godot::ClassDB::bind_method(godot::D_METHOD("get_gpu3d_stats"), &DqMachine::get_gpu3d_stats);
        godot::ClassDB::bind_method(godot::D_METHOD("get_session_start_save"), &DqMachine::get_session_start_save);
        godot::ClassDB::bind_method(godot::D_METHOD("get_input_history"), &DqMachine::get_input_history);
        godot::ClassDB::bind_method(godot::D_METHOD("get_rtc_epoch"), &DqMachine::get_rtc_epoch);
        godot::ClassDB::bind_method(godot::D_METHOD("get_rtc_timezone_offset_minutes"), &DqMachine::get_rtc_timezone_offset_minutes);
        ADD_SIGNAL(godot::MethodInfo("frame_ready", godot::PropertyInfo(godot::Variant::INT, "frame")));
        ADD_SIGNAL(godot::MethodInfo("replay_finished", godot::PropertyInfo(godot::Variant::INT, "frame")));
        ADD_SIGNAL(godot::MethodInfo("error", godot::PropertyInfo(godot::Variant::STRING, "message")));
    }

private:
    static std::vector<std::uint8_t> read_start_save(const std::string& path) {
        std::ifstream initial(std::filesystem::u8path(path), std::ios::binary);
        return {std::istreambuf_iterator<char>(initial), std::istreambuf_iterator<char>()};
    }
    static int collect_save_backup(void* context, const char* path, std::uint64_t size) {
        if (size > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
            return 1;
        static_cast<std::vector<SaveBackup>*>(context)->push_back(
            {path, static_cast<std::int64_t>(size)});
        return 0;
    }
    void release_resources(bool children_alive) {
        if (resources_released_) return;
        resources_released_ = true;
        // Destroying the session on its owner thread joins its per-machine
        // render pool before either DLL is unloaded (also required on Windows).
        shutdown_worker();
        if (children_alive) {
            set_process(false);
            if (audio_player_) {
                audio_player_->stop();
                audio_player_->set_stream({});
            }
            if (gpu_scene_) memdelete(gpu_scene_);
        }
        audio_player_ = nullptr;
        gpu_scene_ = nullptr;
        audio_playback_.unref();
        audio_stream_.unref();
        top_texture_.unref();
        bottom_texture_.unref();
        top_image_.unref();
        bottom_image_.unref();
        for (auto& pair : screen_bytes_)
            for (auto& bytes : pair) bytes.clear();
        audio_buffer_.clear();
        for (auto& slot : frames_) slot.gpu.reset();
        installed_texture_keys_.clear();
    }
    void reset_playback() {
        audio_playback_ = audio_player_->get_stream_playback();
        // Godot 4.5's generator playback keeps only a raw generator pointer.
        // AudioServer retains playback for a final asynchronous fade-out after
        // player.stop(). Let the engine-owned playback retain its source too,
        // rather than leaving a mixer pointer into our released Ref<>.
        if (audio_playback_.is_valid())
            audio_playback_->set_meta("_dq9_generator", audio_stream_);
        generator_capacity_ = audio_playback_.is_valid() ? audio_playback_->get_frames_available() : 0;
    }
    static void copy_screen(dsr_session_handle* session, int screen, std::uint8_t* out) {
        const auto* src = dsr_session_get_screen(session, screen);
        if (!src) return;
        for (std::size_t i = 0; i < kPixels; ++i) {
            out[i * 4] = static_cast<std::uint8_t>(src[i] >> 16U);
            out[i * 4 + 1] = static_cast<std::uint8_t>(src[i] >> 8U);
            out[i * 4 + 2] = static_cast<std::uint8_t>(src[i]);
            out[i * 4 + 3] = static_cast<std::uint8_t>(src[i] >> 24U);
        }
    }
    static std::uint64_t hash_screen(const std::uint32_t* pixels) {
        std::uint64_t hash = 0xcbf29ce484222325ull;
        for (std::size_t i = 0; i < kPixels; ++i) {
            for (const int shift : {16, 8, 0}) {
                hash ^= (pixels[i] >> shift) & 0xffu;
                hash *= 0x100000001b3ull;
            }
        }
        return hash;
    }
    void copy_gpu_snapshot(const dsr::RenderFrame& source, dq9::GpuSceneFrame& target,
                           std::uint64_t epoch, WorkerTextureCache& cache) {
        target.gpu_valid = false;
        target.color_valid = false;
        target.fallback = dq9::GpuFallback::Geometry;
        target.epoch = epoch;
        target.source_frame = source.frame_number + 1;
        target.vram_generation = source.vram_generation;
        target.captured_polygon_count = source.geometry.polygon_count;
        target.polygon_count = target.vertex_count = target.opaque_count = 0;
        target.decoded_this_frame = 0;
#ifdef DQ9_GPU_SCENE_TESTS
        target.native_translucent_polygons = static_cast<std::uint32_t>(source.draw_order.translucent_count);
        target.blending_polygons = 0;
#endif
        target.new_textures.clear();
        const auto& geometry = source.geometry;
        if (geometry.vertex_overflow || geometry.polygon_overflow || !source.opaque_only_mask ||
            geometry.polygon_count > dq9::kGpuPolygonLimit || geometry.vertex_count > dq9::kGpuVertexLimit ||
            source.draw_order.opaque_count + source.draw_order.translucent_count != geometry.polygon_count) return;
        for (std::size_t i = 0; i < geometry.polygon_count; ++i) {
            const auto& polygon = geometry.polygons[i];
            if (polygon.count < 3 || polygon.count > 4) return;
            for (std::size_t j = 0; j < polygon.count; ++j)
                if (polygon.index[j] >= geometry.vertex_count) return;
        }
        target.vertex_count = geometry.vertex_count;
        std::copy_n(geometry.vertices, geometry.vertex_count, target.vertices.begin());
        std::copy_n(source.opaque_only_mask, target.opaque_only.size(), target.opaque_only.begin());
#ifdef DQ9_GPU_SCENE_TESTS
        target.capture_cause_pixels.fill(0);
        if (source.raster.capture_causes)
            for (std::size_t p = 0; p < target.opaque_only.size(); ++p)
                for (unsigned cause = 0; cause < target.capture_cause_pixels.size(); ++cause)
                    target.capture_cause_pixels[cause] += (source.raster.capture_causes[p] >> cause) & 1U;
        target.capture_top_polygons.fill({});
        if (source.raster.capture_polygon_touches) {
            for (std::size_t i = 0; i < geometry.polygon_count; ++i) {
                const auto touches = source.raster.capture_polygon_touches[i];
                if (touches == 0) continue;
                for (std::size_t rank = 0; rank < target.capture_top_polygons.size(); ++rank) {
                    if (touches <= target.capture_top_polygons[rank][5]) continue;
                    for (std::size_t move = target.capture_top_polygons.size() - 1; move > rank; --move)
                        target.capture_top_polygons[move] = target.capture_top_polygons[move - 1];
                    const auto& polygon = geometry.polygons[i];
                    target.capture_top_polygons[rank] = {static_cast<std::uint32_t>(i),
                        (polygon.attr >> 24U) & 63U, (polygon.attr >> 16U) & 31U,
                        (polygon.texture >> 26U) & 7U, (polygon.attr >> 4U) & 3U, touches};
                    break;
                }
            }
        }
#endif
        const auto copy_order = [&](const std::uint16_t* order, std::size_t count) {
            for (std::size_t i = 0; i < count; ++i) {
                if (order[i] >= geometry.polygon_count) return false;
                target.polygons[target.polygon_count++] = geometry.polygons[order[i]];
            }
            return true;
        };
        if (!copy_order(source.draw_order.opaque, source.draw_order.opaque_count)) return;
        target.opaque_count = target.polygon_count;
        if (!copy_order(source.draw_order.translucent, source.draw_order.translucent_count)) return;
        std::copy_n(source.fog_table, 32, target.fog_table.begin());
        std::copy_n(source.toon_table, 32, target.toon_table.begin());
        std::copy_n(source.edge_color_table, 8, target.edge_color_table.begin());
        target.fog_color = source.fog_color; target.fog_offset = source.fog_offset;
        target.clear_color = source.clear_color; target.clear_depth = source.clear_depth;
        target.disp3dcnt = source.disp3dcnt; target.alpha_test_ref = source.alpha_test_ref;
        target.swap_mode = geometry.swap_mode;
#ifdef DQ9_GPU_SCENE_TESTS
        if (source.raster.output_pixel_count != target.software_output.size()) return;
        std::copy_n(source.raster.output, target.software_output.size(), target.software_output.begin());
        if (!source.raster.capture_polygon_ids) return;
        std::copy_n(source.raster.capture_polygon_ids, target.software_polygon_ids.size(), target.software_polygon_ids.begin());
#endif
        target.fallback = dq9::GpuFallback::None;
        if ((geometry.swap_mode & 2U) != 0)
            target.fallback = dq9::GpuFallback::WBuffer;
        if ((source.disp3dcnt & (1U << 14U)) != 0) {
            target.fallback = dq9::GpuFallback::BitmapClear;
            return;
        }
        if (!cache.initialized || cache.epoch != epoch || cache.generation != source.vram_generation) {
            cache.textures.clear();
            cache.referenced.clear();
            cache.epoch = epoch; cache.generation = source.vram_generation; cache.initialized = true;
        }
        if (cache.referenced.capacity() < dq9::kGpuPolygonLimit)
            cache.referenced.reserve(dq9::kGpuPolygonLimit);
        cache.referenced.clear();
        dsr::gpu3d_detail::TextureSource textures;
        textures.context = &source.texture;
        textures.texture_lookup = [](const void* context, int index) -> const std::uint8_t* {
            const auto& slots = static_cast<const dsr::Gpu3DTextureView*>(context)->textures;
            return index >= 0 && static_cast<std::size_t>(index) < slots.size() ? slots[index].bytes : nullptr;
        };
        textures.palette_lookup = [](const void* context, int index) -> const std::uint8_t* {
            const auto& slots = static_cast<const dsr::Gpu3DTextureView*>(context)->palettes;
            return index >= 0 && static_cast<std::size_t>(index) < slots.size() ? slots[index].bytes : nullptr;
        };
        for (std::size_t i = 0; i < target.polygon_count; ++i) {
            const auto& polygon = target.polygons[i];
            if ((source.disp3dcnt & 1U) == 0 || ((polygon.texture >> 26U) & 7U) == 0 ||
                ((polygon.attr >> 16U) & 31U) == 0) continue;
            const dsr::gpu3d_detail::TextureKey key{polygon.texture, polygon.palette, source.vram_generation};
            const auto entry = cache.textures.try_emplace(key).first;
            if (entry->second.last_seen_frame == target.source_frame) continue;
            entry->second.last_seen_frame = target.source_frame;
            if (!entry->second.decoded && target.decoded_this_frame < dq9::kGpuNewTextureLimit) {
                auto decoded = std::make_shared<dq9::DecodedTexture>();
                decoded->key = key;
                dsr::gpu3d_detail::decode_texture_rgba8(textures, polygon.texture, polygon.palette,
                    decoded->rgba, decoded->width, decoded->height);
#ifdef DQ9_GPU_SCENE_TESTS
                decoded->classify_alpha();
#endif
                entry->second.decoded = std::move(decoded);
                ++target.decoded_this_frame;
            }
            cache.referenced.push_back(&*entry);
        }
        bool ready = true;
        if (target.new_textures.capacity() < dq9::kGpuNewTextureLimit)
            target.new_textures.reserve(dq9::kGpuNewTextureLimit);
        {
            std::lock_guard lock(mutex_);
            const bool same_cache = installed_texture_epoch_ == epoch &&
                                    installed_texture_generation_ == source.vram_generation;
            for (const auto* entry : cache.referenced) {
                if (same_cache && installed_texture_keys_.contains(entry->first)) continue;
                if (!entry->second.decoded || target.new_textures.size() == dq9::kGpuNewTextureLimit) {
                    ready = false;
                    continue;
                }
                target.new_textures.push_back(entry->second.decoded);
            }
        }
#ifdef DQ9_GPU_SCENE_TESTS
        if (ready) {
            for (std::size_t i = 0; i < target.polygon_count; ++i) {
                const auto& polygon = target.polygons[i];
                if (((polygon.attr >> 16U) & 31U) == 0) continue;
                std::uint8_t texture_max_partial_alpha = 0;
                if ((source.disp3dcnt & 1U) != 0 && ((polygon.texture >> 26U) & 7U) != 0)
                    texture_max_partial_alpha = cache.textures.at({polygon.texture, polygon.palette,
                                                                   source.vram_generation}).decoded->max_partial_alpha;
                if (dq9::requires_fragment_blending(polygon, source.disp3dcnt,
                                                    source.alpha_test_ref, texture_max_partial_alpha)) {
                    ++target.blending_polygons;
                }
            }
        }
#endif
        target.color_valid = ready;
        target.gpu_valid = ready && target.fallback == dq9::GpuFallback::None;
        if (!ready) target.fallback = dq9::GpuFallback::TextureBudget;
    }
    void worker_loop() {
        dsr_session_config config{};
        config.rom_path = rom_path_.c_str();
        config.save_path = save_path_.c_str();
        config.language = language_;
        config.deterministic_rtc = static_cast<std::uint8_t>(deterministic_rtc_);
        config.rtc_epoch_seconds = rtc_epoch_seconds_;
        config.rtc_timezone_offset_minutes = rtc_timezone_offset_minutes_;
        config.backup_type = backup_type_.c_str();
        char error[512]{};
        session_ = dsr_session_create(&config, error, sizeof(error));
        if (!session_) {
            std::lock_guard lock(mutex_);
            error_ = error;
            loaded_ = false;
        } else {
            loaded_ = true;
            frame_count_ = dsr_session_frame_count(session_);
            ticks_ = dsr_session_ticks(session_);
            save_capacity_ = dsr_session_get_save_capacity(session_);
            copy_screen(session_, 0, frames_[0].screens[0].data());
            copy_screen(session_, 1, frames_[0].screens[1].data());
            frames_[0].number = frame_count_;
            published_slot_ = 0; consuming_slot_ = -1;
        }
        {
            std::lock_guard lock(mutex_);
            initialized_ = true; cv_.notify_all();
        }
        if (session_) {
            WorkerTextureCache texture_cache;
            dq9::GpuFrameDelay gpu_delay;
            bool render_callback_enabled = false;
            std::uint64_t render_callback_epoch = 0;
#ifdef DQ9_GPU_SCENE_TESTS
            std::uint64_t captured_snapshots = 0, valid_snapshots = 0;
#endif
            auto deadline = std::chrono::steady_clock::now();
            for (;;) {
                dsr_input_state input{};
                double speed = 0.0;
                bool do_step = false, manual = false;
                bool gpu_enabled = false;
                bool replaying = false, record_hashes = false;
                std::uint64_t gpu_epoch = 0;
                {
                    std::unique_lock lock(mutex_);
                    cv_.wait(lock, [&] {
                        return shutdown_ || flush_requested_ || backups_requested_ || restore_requested_ ||
                               sync_frames_ > 0 || pending_steps_ > 0 || (running_ && !paused_);
                    });
                    if (shutdown_) break;
                    if (flush_requested_) {
                        lock.unlock();
                        const bool save_ok = dsr_session_save(session_) == 0;
                        lock.lock();
                        last_save_ok_ = save_ok;
                        flush_requested_ = false; cv_.notify_all();
                        continue;
                    }
                    if (restore_requested_) {
                        if (started_ || running_ || paused_) {
                            restore_error_ = "restore_save requires a freshly loaded, unstarted session";
                        } else {
                            auto source = std::move(restore_path_);
                            lock.unlock();
                            error[0] = '\0';
                            const bool restored = dsr_session_restore_save(session_, source.c_str(),
                                                                           error, sizeof(error)) == 0;
                            auto initial_save = restored ? read_start_save(save_path_) : std::vector<std::uint8_t>{};
                            lock.lock();
                            last_save_ok_ = restored;
                            restore_error_ = restored ? "" : error;
                            if (restored) session_start_save_ = std::move(initial_save);
                        }
                        restore_requested_ = false; cv_.notify_all();
                        continue;
                    }
                    if (backups_requested_) {
                        lock.unlock();
                        std::vector<SaveBackup> backups;
                        error[0] = '\0';
                        const bool listed = dsr_session_get_save_backups(session_, collect_save_backup,
                                                                         &backups, error, sizeof(error)) == 0;
                        lock.lock();
                        save_backup_error_ = listed ? "" : error;
                        if (!listed) { backups.clear(); error_ = error; }
                        save_backups_ = std::move(backups);
                        backups_requested_ = false; cv_.notify_all();
                        continue;
                    }
                    if (sync_frames_ > 0) { --sync_frames_; manual = true; do_step = true; }
                    else if (pending_steps_ > 0) { --pending_steps_; manual = true; do_step = true; }
                    else if (running_ && !paused_) do_step = true;
                    if (!do_step) continue;
                    input = input_; speed = speed_;
                    replaying = replay_active_;
                    record_hashes = replaying && record_replay_hashes_;
                    if (replaying) {
                        input = {};
                        if (script_cursor_ < input_script_.size() &&
                            input_script_[script_cursor_].frame == frame_count_)
                            input = input_script_[script_cursor_++].input;
                        speed = 0.0;
                    }
                    gpu_enabled = gpu_enabled_; gpu_epoch = gpu_epoch_;
                }
                if (gpu_enabled != render_callback_enabled || gpu_epoch != render_callback_epoch) {
                    gpu_delay.reset();
                    texture_cache.textures.clear();
                    texture_cache.initialized = false;
                    render_callback_enabled = gpu_enabled;
                    render_callback_epoch = gpu_epoch;
                    dsr::set_session_capture_opaque_only(session_, gpu_enabled);
                    if (gpu_enabled) {
                        dsr::set_session_render_callback(session_, [&] {
                            dsr::RenderFrame source{};
                            if (dsr::capture_session_render_frame(session_, source)) {
                                auto& target = gpu_delay.capture_target();
                                copy_gpu_snapshot(source, target, render_callback_epoch, texture_cache);
#ifdef DQ9_GPU_SCENE_TESTS
                                target.captured_snapshots = ++captured_snapshots;
                                target.valid_snapshots = valid_snapshots += target.gpu_valid ? 1U : 0U;
#endif
                            }
                        });
                    } else {
                        dsr::set_session_render_callback(session_, {});
                    }
                }
                if (gpu_enabled) gpu_delay.begin_step();
                if (dsr_session_step(session_, &input, error, sizeof(error)) != 0) {
                    std::lock_guard lock(mutex_); error_ = error; running_ = false; paused_ = true;
                    sync_frames_ = 0; sync_done_ = true; cv_.notify_all();
                    replay_active_ = replay_finished_pending_ = false;
                    continue;
                }
                std::size_t count = 0; double rate = 0;
                ReplayHash replay_hash{};
                if (record_hashes) {
                    replay_hash = {dsr_session_frame_count(session_) - 1,
                                   hash_screen(dsr_session_get_screen(session_, 0)),
                                   hash_screen(dsr_session_get_screen(session_, 1))};
                }
                const auto* audio = dsr_session_get_audio(session_, &count, &rate);
                {
                    std::lock_guard audio_lock(audio_mutex_);
                    audio_source_rate_ = rate;
                    push_audio(audio, count);
                }
                int output_slot = -1;
                {
                    std::lock_guard lock(mutex_);
                    for (int candidate = 0; candidate < 3; ++candidate) {
                        if (candidate != consuming_slot_ && candidate != published_slot_) {
                            output_slot = candidate;
                            break;
                        }
                    }
                }
                if (output_slot >= 0) {
                    copy_screen(session_, 0, frames_[output_slot].screens[0].data());
                    copy_screen(session_, 1, frames_[output_slot].screens[1].data());
                    frames_[output_slot].number = dsr_session_frame_count(session_);
                    if (gpu_enabled) {
                        dsr::RenderFrame displayed{};
                        dsr::capture_session_render_frame(session_, displayed);
                        gpu_delay.publish(frames_[output_slot].gpu, displayed, gpu_epoch);
                    }
                }
                {
                    std::lock_guard lock(mutex_);
                    frame_count_ = dsr_session_frame_count(session_);
                    if (!chip_mismatch_) chip_mismatch_ = dsr_session_chip_mismatch_suspected(session_) != 0;
                    if (output_slot >= 0) published_slot_ = output_slot;
                    const std::uint64_t input_frame = frame_count_ == 0 ? 0 : frame_count_ - 1;
                    input_history_.push_back({input_frame, input});
                    ticks_ = dsr_session_ticks(session_);
                    if (replaying && replay_active_) {
                        if (record_hashes) replay_hashes_.push_back(replay_hash);
                        if (frame_count_ == replay_target_ + 1) {
                            replay_active_ = false; paused_ = true;
                            replay_finished_pending_ = true;
                            input_ = {};
                            std::lock_guard audio_lock(audio_mutex_);
                            ring_read_ = ring_write_ = 0;
                            resample_phase_ = 0.0; audio_priming_ = true;
                        }
                    }
                    if (manual && sync_frames_ == 0) { sync_done_ = true; cv_.notify_all(); }
                }
                if (!manual && speed > 0.0) {
                    deadline += std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                        std::chrono::duration<double>(1.0 / (kFrameRate * speed)));
                    std::unique_lock lock(mutex_);
                    cv_.wait_until(lock, deadline, [&] {
                        return shutdown_ || flush_requested_ || backups_requested_ || restore_requested_ ||
                               paused_ || !running_ || pending_steps_ > 0 || speed_ != speed;
                    });
                    if (std::chrono::steady_clock::now() > deadline + std::chrono::milliseconds(250))
                        deadline = std::chrono::steady_clock::now();
                }
            }
            dsr::set_session_render_callback(session_, {});
            dsr::set_session_capture_opaque_only(session_, false);
            const bool save_ok = dsr_session_save(session_) == 0;
            {
                std::lock_guard lock(mutex_);
                last_save_ok_ = save_ok;
                loaded_ = false; save_capacity_ = 0;
                save_backups_.clear();
                if (backups_requested_) save_backup_error_ = "session closed before save backup listing";
                backups_requested_ = false;
                if (restore_requested_) restore_error_ = "session closed before save restore";
                restore_requested_ = flush_requested_ = false; cv_.notify_all();
            }
            dsr_session_destroy(session_);
            session_ = nullptr;
        }
    }
    void push_audio(const int16_t* samples, std::size_t count) {
        if (!samples || !count) return;
        const std::size_t capacity = ring_.size() / 2;
        const std::size_t queued = ring_write_ - ring_read_;
        const std::size_t max_keep = static_cast<std::size_t>(audio_source_rate_ * 0.2);
        if (queued + count > max_keep) {
            const std::size_t drop = std::min(queued, queued + count - max_keep);
            ring_read_ += drop; dropped_audio_frames_ += drop;
        }
        for (std::size_t i = 0; i < count; ++i) {
            if (ring_write_ - ring_read_ >= capacity) { ++ring_read_; ++dropped_audio_frames_; }
            const auto at = (ring_write_ % capacity) * 2;
            ring_[at] = samples[i * 2]; ring_[at + 1] = samples[i * 2 + 1]; ++ring_write_;
        }
    }
    void pull_audio() {
        if (!audio_player_) return;
        if (audio_playback_.is_null()) reset_playback();
        auto* generator = audio_playback_.ptr();
        if (!generator) return;
        const double output_rate = audio_stream_->get_mix_rate();
        const int target = static_cast<int>(std::ceil(output_rate * audio_target_seconds_));
        const int available = generator->get_frames_available();
        const int generator_queued = generator_capacity_ - available;
        const int wanted = std::min(available, target - generator_queued);
        if (wanted <= 0) return;
        std::size_t queued, queued_total, read;
        double source_rate;
        const double initial_phase = resample_phase_;
        {
            std::lock_guard audio_lock(audio_mutex_);
            queued_total = ring_write_ - ring_read_;
            read = ring_read_;
            source_rate = audio_source_rate_;
            const double max_input = initial_phase + wanted * source_rate / output_rate * 1.02 + 3.0;
            queued = std::min(queued_total, static_cast<std::size_t>(max_input));
            queued = std::min(queued, audio_snapshot_.size() / 2);
            for (std::size_t i = 0; i < queued; ++i) {
                const auto at = ((read + i) % kRingFrames) * 2;
                audio_snapshot_[i * 2] = ring_[at];
                audio_snapshot_[i * 2 + 1] = ring_[at + 1];
            }
        }
        if (queued < 2) return;
        const double total_seconds = static_cast<double>(queued_total) / source_rate +
                                     static_cast<double>(generator_queued) / output_rate;
        const double correction = std::clamp(
            (total_seconds - audio_target_seconds_ - 0.5 / kFrameRate) * 0.15, -0.01, 0.01);
        const double step = source_rate / output_rate * (1.0 + correction);
        audio_buffer_.resize(wanted);
        auto* output = audio_buffer_.ptrw();
        double phase = resample_phase_;
        int written = 0;
        while (written < wanted) {
            const std::size_t index = static_cast<std::size_t>(phase);
            if (index + 1 >= queued) break;
            const double fraction = phase - static_cast<double>(index);
            output[written++] = godot::Vector2(
                static_cast<float>((audio_snapshot_[index * 2] * (1.0 - fraction) +
                                    audio_snapshot_[(index + 1) * 2] * fraction) / 32768.0),
                static_cast<float>((audio_snapshot_[index * 2 + 1] * (1.0 - fraction) +
                                    audio_snapshot_[(index + 1) * 2 + 1] * fraction) / 32768.0));
            phase += step;
        }
        audio_buffer_.resize(written);
        if (written > 0 && generator->push_buffer(audio_buffer_)) {
            const auto consumed = std::min(static_cast<std::size_t>(phase), queued);
            {
                std::lock_guard audio_lock(audio_mutex_);
                audio_frames_pushed_ += static_cast<std::uint64_t>(written);
                ring_read_ = std::max(ring_read_, read + consumed);
            }
            resample_phase_ = phase - static_cast<double>(consumed);
            if (audio_priming_ && generator_capacity_ - generator->get_frames_available() >= target) {
                audio_player_->set_stream_paused(false);
                audio_priming_ = false;
            }
        }
    }

    mutable std::mutex mutex_;
    std::condition_variable cv_;
    dsr_session_handle* session_ = nullptr;
    dsr_input_state input_{};
    bool loaded_ = false, running_ = false, paused_ = false;
    bool audio_priming_ = true;
    bool shutdown_ = false, initialized_ = false, flush_requested_ = false, sync_done_ = false;
    bool last_save_ok_ = true;
    bool started_ = false, backups_requested_ = false, restore_requested_ = false;
    std::uint64_t save_capacity_ = 0;
    std::vector<SaveBackup> save_backups_;
    std::string restore_path_, restore_error_;
    std::string save_backup_error_;
    int pending_steps_ = 0, sync_frames_ = 0;
    float speed_ = 1.0f;
    std::string rom_path_, save_path_;
    std::vector<std::uint8_t> session_start_save_;
    std::vector<RecordedInput> input_history_;
    std::vector<RecordedInput> input_script_;
    std::size_t script_cursor_ = 0;
    std::uint64_t replay_target_ = 0;
    bool replay_active_ = false, replay_finished_pending_ = false, record_replay_hashes_ = false;
    std::vector<ReplayHash> replay_hashes_;
    std::uint8_t language_ = 1;
    bool deterministic_rtc_ = false;
    std::string backup_type_;
    bool chip_mismatch_ = false;
    std::int64_t rtc_epoch_seconds_ = 0;
    std::int32_t rtc_timezone_offset_minutes_ = 0;
    std::thread owner_thread_;
    FrameSlot frames_[3];
    int published_slot_ = -1, consuming_slot_ = -1;
    bool gpu_enabled_ = false;
    int gpu_scale_ = 3;
    std::uint64_t gpu_epoch_ = 0, installed_texture_epoch_ = 0, installed_texture_generation_ = 0;
    std::unordered_set<dsr::gpu3d_detail::TextureKey, dq9::TextureKeyHash> installed_texture_keys_;
    DqGpuScene* gpu_scene_ = nullptr;
    godot::PackedByteArray screen_bytes_[2][2];
    int screen_bytes_index_ = 0;
    std::uint64_t frame_count_ = 0, ticks_ = 0;
    double fps_ = 0.0;
    std::string error_;
    godot::PackedVector2Array audio_buffer_;
    std::vector<std::int16_t> ring_;
    mutable std::mutex audio_mutex_;
    std::array<std::int16_t, kRingFrames * 2> audio_snapshot_{};
    int generator_capacity_ = 0;
    std::size_t ring_read_ = 0, ring_write_ = 0;
    std::uint64_t underruns_ = 0, dropped_audio_frames_ = 0;
    std::uint64_t audio_frames_pushed_ = 0;
    double audio_target_seconds_ = kAudioPrimeSeconds;
    double audio_source_rate_ = kAudioRate, resample_phase_ = 0;
    godot::Ref<godot::Image> top_image_, bottom_image_;
    godot::Ref<godot::ImageTexture> top_texture_, bottom_texture_;
    godot::Ref<godot::AudioStreamGenerator> audio_stream_;
    godot::Ref<godot::AudioStreamGeneratorPlayback> audio_playback_;
    godot::AudioStreamPlayer* audio_player_ = nullptr;
    DqMachine* previous_instance_ = nullptr;
    DqMachine* next_instance_ = nullptr;
    bool resources_released_ = false;
};

void register_dq_machine_class() {
    godot::ClassDB::register_class<DqMachine>();
}

void shutdown_dq_machine_instances() {
    DqMachine::shutdown_instances();
}
