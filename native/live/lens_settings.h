#pragma once

#include <cmath>

namespace cs2_camera_lens {

// Color-only camera finishing. These controls never claim a scene depth buffer
// or emulate depth of field from color/luminance.
struct Settings {
    bool enabled = false;
    float distortion = 0;  // [-.75,.75], negative pincushion / positive barrel
    float vignette = 0;    // [0,1], linear-light corner attenuation
    float chromatic = 0;   // [0,8], radial red/blue separation in output pixels

    bool Valid() const noexcept {
        return std::isfinite(distortion) && distortion >= -.75f && distortion <= .75f &&
            std::isfinite(vignette) && vignette >= 0 && vignette <= 1 &&
            std::isfinite(chromatic) && chromatic >= 0 && chromatic <= 8;
    }
    bool Active() const noexcept {
        return Valid() && enabled && (distortion != 0 || vignette != 0 || chromatic != 0);
    }
};

} // namespace cs2_camera_lens
