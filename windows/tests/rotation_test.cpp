#include "rtsp/rotation_math.h"
#include <cassert>
#include <cstdint>
#include <iostream>

int main()
{
    int warnings = 0;
    auto warn = [&](int) { ++warnings; };
    for (int degrees : {0, 90, 180, 270, 360, -90, -180, -270, -360}) {
        const int actual = RTSP::NormalizeRotation(degrees, warn);
        assert(actual == (degrees % 360 + 360) % 360);
        assert(actual % 90 == 0);
    }
    assert(warnings == 0);
    for (int invalid : {2, 7, 44, 46, 91, 178, 359, -2}) {
        assert(RTSP::NormalizeRotation(invalid, warn) == 0);
    }
    assert(warnings == 8);
    const std::uint8_t wire = static_cast<std::uint8_t>(-90);
    assert(wire == 166);
    assert(RTSP::NormalizeRotation(static_cast<std::int8_t>(wire), warn) == 270);
    std::cout << "Rotation validation and signed-wire tests passed\n";
}
