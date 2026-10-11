#pragma once

namespace RTSP
{
    // The control protocol carries signed cardinal deltas, not camera-roll metadata.
    template<class Warn>
    int NormalizeRotation(int degrees, Warn warn)
    {
        if (degrees % 90 != 0) { warn(degrees); return 0; }
        return (degrees % 360 + 360) % 360;
    }
}
