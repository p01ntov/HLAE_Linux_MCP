#include "motion_track.h"
#include <cassert>
#include <limits>

int main() {
    using cs2_mvp::CameraFrame;
    using cs2_mvp::MotionTrack;
    MotionTrack track;
    bool rejected = false;
    try { track.Begin({}); } catch (const std::invalid_argument&) { rejected = true; }
    assert(rejected);
    std::vector<CameraFrame> frames;
    for (int frame = 0; frame < 120; ++frame) {
        frames.push_back({{static_cast<double>(frame), -1.25, 64.0}, {-5.0, 179.5}});
    }
    track.Begin(frames);
    rejected = false;
    try { track.Current(); } catch (const std::logic_error&) { rejected = true; }
    assert(rejected);
    // Model a 120 fps movie at a 64 Hz simulation: repeated integer ticks
    // must still consume distinct output-frame poses.
    int repeated = 0;
    int previousTick = -1;
    for (int frame = 0; frame < 120; ++frame) {
        int tick = frame * 64 / 120;
        repeated += tick == previousTick;
        previousTick = tick;
        assert(track.Next().position[0] == frame);
        assert(track.Current().position[0] == frame);
        assert(track.Current().position[0] == frame); // Other stages re-use, never advance.
    }
    assert(repeated > 0);
    assert(track.Applied() == 120 && track.Callbacks() == 120);
    assert(track.Next().position[0] == 119); // Safe hold, no OOB access.
    assert(track.Overflow() == 1 && track.Callbacks() == 121);
    track.End();
    rejected = false;
    try { track.Next(); } catch (const std::logic_error&) { rejected = true; }
    assert(rejected);
    rejected = false;
    try { track.Current(); } catch (const std::logic_error&) { rejected = true; }
    assert(rejected);
    const auto endedApplied=track.Applied(), endedCallbacks=track.Callbacks();
    assert(track.LastApplied().position[0] == 119);
    assert(track.Applied()==endedApplied && track.Callbacks()==endedCallbacks);
    // A 1000 FPS / eight-tick capture contains exactly 125 samples. Its last
    // raster can happen after camera_end and must hold without sample126.
    std::vector<CameraFrame> highFps;
    for(int i=0;i<125;++i) highFps.push_back({{double(i),0,64},{0,0,0},56.0});
    track.Begin(highFps);
    for(int i=0;i<125;++i) track.Next();
    track.End();
    for(int flush=0;flush<16;++flush) assert(track.LastApplied().position[0]==124);
    assert(track.Applied()==125 && track.Callbacks()==125 && track.Overflow()==0);
    track.Begin({frames[0]});
    assert(track.Applied() == 0 && track.Callbacks() == 0);
    assert(track.Next().Command() == "spec_goto 0.000000 -1.250000 64.000000 -5.000000 179.500000");
    frames[0].position[0] = std::numeric_limits<double>::quiet_NaN();
    rejected = false;
    try { track.Begin(frames); } catch (const std::invalid_argument&) { rejected = true; }
    assert(rejected);
    CameraFrame wide{{0,0,200},{20,30,0},105.0};
    track.Begin({wide});
    assert(track.Next().fov.value()==105.0);
    for (double invalid : {29.0,141.0,std::numeric_limits<double>::quiet_NaN()}) {
        wide.fov=invalid;
        rejected=false;
        try { track.Begin({wide}); } catch (const std::invalid_argument&) { rejected=true; }
        assert(rejected);
    }
}
