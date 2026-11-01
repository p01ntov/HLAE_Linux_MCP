#pragma once

// CS2 MVP motion ABI 3. No Valve interfaces, wall clock, or demo ticks here.
// One call to Next() consumes one planned pose. The CSDM adapter calls it once
// per FRAME_START, including callbacks sharing an integer demo tick. This is
// not a verified final-view or output-frame hook; capture counts are checked.

#include <array>
#include <cmath>
#include <cstddef>
#include <iomanip>
#include <locale>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace cs2_mvp {

struct CameraFrame {
    std::array<double, 3> position;
    std::array<double, 3> angles; // pitch, yaw, roll.
    std::optional<double> fov{}; // null preserves the game's lens.

    void Validate() const {
        for (double value : position) {
            if (!std::isfinite(value) || std::abs(value) > 1000000.0) {
                throw std::invalid_argument("camera position must be finite and within +/-1000000");
            }
        }
        for (double value : angles) {
            if (!std::isfinite(value) || std::abs(value) > 1000000.0) {
                throw std::invalid_argument("camera angles must be finite and within +/-1000000");
            }
        }
        if (std::abs(angles[0]) > 90.0) {
            throw std::invalid_argument("camera pitch must remain within +/-90 degrees");
        }
        if (fov && (!std::isfinite(*fov) || *fov < 30.0 || *fov > 140.0)) {
            throw std::invalid_argument("camera FOV must be finite and within 30..140 degrees");
        }
    }

    std::string Command() const {
        std::ostringstream output;
        output.imbue(std::locale::classic());
        output << std::fixed << std::setprecision(6) << "spec_goto "
               << position[0] << ' ' << position[1] << ' ' << position[2]
               << ' ' << angles[0] << ' ' << angles[1];
        return output.str();
    }
};

class MotionTrack {
public:
    void Begin(const std::vector<CameraFrame>& frames) {
        if (frames.empty()) {
            throw std::invalid_argument("camera_begin requires nonempty camera_frames");
        }
        for (const auto& frame : frames) {
            frame.Validate();
        }
        frames_ = frames;
        applied_ = 0;
        callbacks_ = 0;
        active_ = true;
    }

    const CameraFrame& Next() {
        if (!active_) {
            throw std::logic_error("camera frame requested while camera is inactive");
        }
        ++callbacks_;
        if (applied_ < frames_.size()) {
            return frames_[applied_++];
        }
        // Tick-based endmovie can arrive one or two render frames late. Hold
        // the last pose, count the surplus, and let the caller validate it.
        return frames_.back();
    }

    void End() { active_ = false; }
    // Capture shutdown can rasterize one final image after the frame clock
    // stops. Reuse this output without consuming or counting another sample.
    const CameraFrame& LastApplied() const {
        if (frames_.empty() || applied_ == 0) {
            throw std::logic_error("camera hold requested before its first sample");
        }
        return frames_[applied_ - 1];
    }
    const CameraFrame& Current() const {
        if (!active_ || applied_ == 0) {
            throw std::logic_error("camera pose requested before its first sample");
        }
        return LastApplied();
    }
    bool Active() const { return active_; }
    std::size_t Applied() const { return applied_; }
    std::size_t Expected() const { return frames_.size(); }
    std::size_t Callbacks() const { return callbacks_; }
    std::size_t Overflow() const { return callbacks_ - applied_; }

private:
    std::vector<CameraFrame> frames_;
    std::size_t applied_ = 0;
    std::size_t callbacks_ = 0;
    bool active_ = false;
};

} // namespace cs2_mvp
