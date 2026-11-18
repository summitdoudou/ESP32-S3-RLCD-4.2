// 黄金图闸门（v1.6.32 P1-3）：块打包路径与遗留 LUT 逐像素路径必须逐字节全等。
// 参考端复刻 InitLandscapeLUT 原始算法建两张表 + 逐像素 RMW（等价旧 RLCD_SetPixel），
// 被测端用 rlcd_pixel_map.h 的算术公式 + PackRegionLandscape。
#include "../components/port_bsp/rlcd_pixel_map.h"

#include <assert.h>
#include <stdint.h>
#include <string.h>
#include <vector>

namespace {

constexpr uint16_t kW = 400;
constexpr uint16_t kH = 300;
constexpr size_t kFrameBytes = static_cast<size_t>(kW) * kH / 8;
constexpr uint16_t kThreshold = 128;

// 参考实现：与 display_bsp.cpp InitLandscapeLUT 同款算法。
struct LegacyLut {
    std::vector<uint16_t> index;
    std::vector<uint8_t> mask;
    LegacyLut() : index(static_cast<size_t>(kW) * kH), mask(static_cast<size_t>(kW) * kH)
    {
        uint16_t H4 = kW >> 2;  // 故意与原始代码一致地用 height>>2（kH=300 -> 75）
        H4 = kH >> 2;
        for (uint16_t y = 0; y < kH; y++) {
            uint16_t inv_y = kH - 1 - y;
            uint16_t block_y = inv_y >> 2;
            uint8_t local_y = static_cast<uint8_t>(inv_y & 3);
            for (uint16_t x = 0; x < kW; x++) {
                uint16_t byte_x = x >> 1;
                uint8_t local_x = static_cast<uint8_t>(x & 1);
                uint32_t byte_index = static_cast<uint32_t>(byte_x) * H4 + block_y;
                uint8_t bit = static_cast<uint8_t>(7 - ((local_y << 1) | local_x));
                size_t lut_offset = static_cast<size_t>(x) * kH + y;
                index[lut_offset] = static_cast<uint16_t>(byte_index);
                mask[lut_offset] = static_cast<uint8_t>(1u << bit);
            }
        }
    }
};

// 参考写入：逐像素 RMW（等价旧 RLCD_SetPixel + 旧 flush 循环）。
void ReferenceFill(std::vector<uint8_t> &frame, const LegacyLut &lut,
                   const std::vector<uint16_t> &src, int src_pitch,
                   int x1, int y1, int x2, int y2)
{
    for (int y = y1; y <= y2; ++y) {
        for (int x = x1; x <= x2; ++x) {
            const size_t lut_offset = static_cast<size_t>(x) * kH + y;
            uint32_t idx = lut.index[lut_offset];
            uint8_t m = lut.mask[lut_offset];
            uint8_t color = (src[static_cast<size_t>(y - y1) * src_pitch +
                                 static_cast<size_t>(x - x1)] < kThreshold)
                                ? 0
                                : 1;
            uint8_t *p = &frame[idx];
            if (color) {
                *p |= m;
            } else {
                *p &= ~m;
            }
        }
    }
}

uint32_t next_rand(uint32_t &seed)
{
    seed = seed * 2416u + 37u;  // 复刻测试惯用的简单 LCG
    return (seed >> 8) ^ (seed << 7);
}

} // namespace

int main()
{
    LegacyLut lut;

    // 1) 公式等价：算术公式必须与 LUT 表逐字节一致。
    for (uint16_t y = 0; y < kH; ++y) {
        for (uint16_t x = 0; x < kW; ++x) {
            const size_t lut_offset = static_cast<size_t>(x) * kH + y;
            assert(static_cast<uint32_t>(lut.index[lut_offset]) ==
                   rlcd_pixel_map::LandscapeByteIndex(x, y, kH));
            assert(lut.mask[lut_offset] ==
                   rlcd_pixel_map::LandscapeBitMask(x, y, kH));
        }
    }

    uint32_t seed = 123u;
    // 2) 黄金图：随机矩形下 打包路径 == 逐像素参考路径（帧起点先随机预填充非零态）。
    for (int round = 0; round < 200; ++round) {
        std::vector<uint8_t> ref_frame(kFrameBytes);
        std::vector<uint8_t> pack_frame(kFrameBytes);
        uint32_t s0 = next_rand(seed);
        for (size_t i = 0; i < kFrameBytes; ++i) {
            s0 = s0 * 1103515245u + 12345u;
            uint8_t v = static_cast<uint8_t>((s0 >> 16) & 0xFF);
            if (round % 3 == 0) {
                v = 0;  // 部分轮次从全零开始，覆盖纯写路径。
            }
            ref_frame[i] = v;
            pack_frame[i] = v;
        }

        int x1 = static_cast<int>(next_rand(seed) % kW);
        int x2 = x1 + static_cast<int>(next_rand(seed) % (kW - x1));
        int y1 = static_cast<int>(next_rand(seed) % kH);
        int y2 = y1 + static_cast<int>(next_rand(seed) % (kH - y1));
        if (round == 0) {
            x1 = 0; y1 = 0; x2 = kW - 1; y2 = kH - 1;  // 全屏
        }
        const int w = x2 - x1 + 1;
        const int h = y2 - y1 + 1;
        std::vector<uint16_t> src(static_cast<size_t>(w) * h);
        uint32_t sp = next_rand(seed);
        for (size_t i = 0; i < src.size(); ++i) {
            sp = sp * 1103515245u + 12345u;
            src[i] = static_cast<uint16_t>((sp >> 16) & 0xFFFF);
        }

        ReferenceFill(ref_frame, lut, src, w, x1, y1, x2, y2);
        rlcd_pixel_map::PackRegionLandscape(pack_frame.data(), kW, kH,
                                            src.data(), w, x1, y1, x2, y2,
                                            kThreshold);
        assert(ref_frame == pack_frame);
    }

    // 3) 单像素角落写：目标位按阈值置位，同字节其余槽位必须保持原值。
    std::vector<uint8_t> edge(kFrameBytes, 0x5A);
    std::vector<uint16_t> px(1, 0);
    rlcd_pixel_map::PackRegionLandscape(edge.data(), kW, kH, px.data(), 1,
                                        399, 299, 399, 299, kThreshold);
    {
        const uint8_t mask = rlcd_pixel_map::LandscapeBitMask(399, 299, kH);
        const uint8_t corner0 = edge[rlcd_pixel_map::LandscapeByteIndex(399, 299, kH)];
        assert((corner0 & mask) == 0);
        assert((corner0 & ~mask) == (0x5A & ~mask));
    }
    px[0] = 0xFFFF;
    rlcd_pixel_map::PackRegionLandscape(edge.data(), kW, kH, px.data(), 1,
                                        399, 299, 399, 299, kThreshold);
    {
        const uint8_t mask = rlcd_pixel_map::LandscapeBitMask(399, 299, kH);
        const uint8_t corner = edge[rlcd_pixel_map::LandscapeByteIndex(399, 299, kH)];
        assert((corner & mask) != 0);
        assert((corner & ~mask) == (0x5A & ~mask));
    }

    return 0;
}
