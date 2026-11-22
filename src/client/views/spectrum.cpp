#include "client/views/spectrum.hpp"

#include "client/theme.hpp"
#include "client/views/drawing.hpp"

#include <algorithm>
#include <cstddef>

namespace am::client {

void draw_spectrum(ImDrawList& draw, ImVec2 min, ImVec2 max,
                   const std::array<float, common::kFftBandCount>& bands_db, float alpha) {
    constexpr auto kBars = static_cast<float>(common::kFftBandCount);
    // A bar and the gap after it share one slot; the gap is a quarter of it.
    const float slot = (max.x - min.x) / kBars;
    const float width = slot * 0.75f;
    const float height = max.y - min.y;
    // A sliver even at the floor, so the strip reads as a row of bars, not
    // as nothing, while it's quiet.
    const float sliver = std::max(1.0f, height * 0.02f);
    const ImU32 color = to_u32(palette::kAccent, 0.85f * alpha);
    for (std::size_t band = 0; band < common::kFftBandCount; ++band) {
        const float left = min.x + slot * static_cast<float>(band);
        const float top = max.y - std::max(sliver, height * bar_fraction(bands_db[band]));
        draw.AddRectFilled({left, top}, {left + width, max.y}, color, width * 0.25f,
                           ImDrawFlags_RoundCornersTop);
    }
}

} // namespace am::client
