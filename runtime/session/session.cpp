#include "dsr/session.h"
#include "dsr/gpu2d.h"
namespace dsr {


Session::Session(const MachineConfig& config) : machine_(config) {}

void Session::step(const InputState& input) {
    machine_.set_input(input);
    machine_.run_frame();
    audio_frames_ = machine_.spu().drain(audio_.data(), audio_.size() / 2);
    ++frame_count_;
}

const std::uint32_t* Session::screen_top() const {
    return machine_.gpu2d().screen_top();
}

const std::uint32_t* Session::screen_bottom() const {
    return machine_.gpu2d().screen_bottom();
}

void Session::flush_save() {
    if (GameCard* card = machine_.game_card()) card->flush_save();
}

} // namespace dsr
