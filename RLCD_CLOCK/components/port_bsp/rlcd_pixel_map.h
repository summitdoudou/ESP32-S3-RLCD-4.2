// RLCD 横屏帧缓冲位映射的纯算术形态（v1.6.32 P1-3），替代 360KB PSRAM 查表 LUT。
// 布局必须与遗留 InitLandscapeLUT 完全一致：1 字节 = 2 列(x) × 4 行(inv_y)，
// 该等价性由 tests/rlcd_pixel_map_host_test.cpp 黄金图逐字节断言。
// 本文件零 esp 依赖，host 侧可直接编译测试。
#pragma once

#include <stddef.h>
#include <stdint.h>

namespace rlcd_pixel_map {

// 横屏字节下标：byte_x = x>>1，行块按自底向上 inv_y = height-1-y 计。
inline size_t LandscapeByteIndex(uint16_t x, uint16_t y, uint16_t height)
{
    const size_t byte_x = static_cast<size_t>(x) >> 1;
    const uint16_t inv_y = static_cast<uint16_t>(height - 1 - y);
    const size_t block_y = static_cast<size_t>(inv_y) >> 2;
    const size_t h4 = static_cast<size_t>(height) >> 2;
    return byte_x * h4 + block_y;
}

// 横屏位掩码：bit = 7 - ((local_y << 1) | local_x)。
inline uint8_t LandscapeBitMask(uint16_t x, uint16_t y, uint16_t height)
{
    const uint16_t inv_y = static_cast<uint16_t>(height - 1 - y);
    const uint8_t local_y = static_cast<uint8_t>(inv_y & 3);
    const uint8_t local_x = static_cast<uint8_t>(x & 1);
    return static_cast<uint8_t>(1u << (7 - ((local_y << 1) | local_x)));
}

// 块打包区域写入：src 为行主序 RGB565（pitch=src_pitch，左上对应 (x1,y1)），
// 把矩形 [x1,y1]..[x2,y2] 阈值化后打包写进 frame。阈值语义与旧 flush 一致：
// 低于 black_threshold 判黑（该位写 0），否则判白（写 1）。
// 完整覆盖的 2×4 块直接整字节覆盖写，不再逐像素读改写。
inline void PackRegionLandscape(uint8_t *frame, uint16_t width, uint16_t height,
                                const uint16_t *src, int src_pitch,
                                int x1, int y1, int x2, int y2,
                                uint16_t black_threshold)
{
    if (!frame || !src || x1 > x2 || y1 > y2) {
        return;
    }
    if (x1 < 0) {
        x1 = 0;
    }
    if (y1 < 0) {
        y1 = 0;
    }
    if (x2 >= static_cast<int>(width)) {
        x2 = static_cast<int>(width) - 1;
    }
    if (y2 >= static_cast<int>(height)) {
        y2 = static_cast<int>(height) - 1;
    }
    if (x1 > x2 || y1 > y2) {
        return;
    }

    const int h4 = static_cast<int>(height) >> 2;
    const int inv_lo = static_cast<int>(height) - 1 - y2;
    const int inv_hi = static_cast<int>(height) - 1 - y1;

    for (int inv_blk = (inv_lo >> 2) << 2; inv_blk <= inv_hi; inv_blk += 4) {
        const bool rows_full = (inv_blk >= inv_lo) && (inv_blk + 3 <= inv_hi);
        const int block_y = inv_blk >> 2;
        for (int bx = x1 >> 1; (bx << 1) <= x2; ++bx) {
            const int x_first = bx << 1;
            const bool cols_full = (x_first >= x1) && (x_first + 1 <= x2);
            const size_t byte_index =
                static_cast<size_t>(bx) * static_cast<size_t>(h4) +
                static_cast<size_t>(block_y);
            uint8_t byte_value = 0;
            const bool full_block = rows_full && cols_full;
            if (!full_block) {
                byte_value = frame[byte_index];
            }
            for (int ly = 0; ly < 4; ++ly) {
                const int y = static_cast<int>(height) - 1 - (inv_blk + ly);
                if (y < y1 || y > y2) {
                    continue;
                }
                const uint16_t *row = src +
                    static_cast<size_t>(y - y1) * static_cast<size_t>(src_pitch);
                for (int lx = 0; lx < 2; ++lx) {
                    const int x = x_first + lx;
                    if (x < x1 || x > x2) {
                        continue;
                    }
                    const uint8_t bit =
                        static_cast<uint8_t>(7 - ((ly << 1) | lx));
                    const uint8_t mask = static_cast<uint8_t>(1u << bit);
                    if (row[x - x1] >= black_threshold) {
                        byte_value |= mask;
                    } else {
                        byte_value = static_cast<uint8_t>(byte_value & ~mask);
                    }
                }
            }
            frame[byte_index] = byte_value;
        }
    }
}

} // namespace rlcd_pixel_map
