#define NOMINMAX
#include <windows.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include "features/fast_math.h"
#include "features/wall_sample_span.h"
#include "extracted_wall_span.h"

int checks = 0;
void Check(bool ok, const char* what) {
    ++checks;
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", what); std::exit(1); }
}
template<class T, size_t N> void Put(std::array<unsigned char, N>& bytes, size_t at, T value) {
    std::memcpy(bytes.data() + at, &value, sizeof value);
}
template<class T, size_t N> T Get(const std::array<unsigned char, N>& bytes, size_t at) {
    T value;
    std::memcpy(&value, bytes.data() + at, sizeof value);
    return value;
}
int main() {
    static_assert(sizeof(uintptr_t) == 4);
    // A clipped eave already takes the low-foot UV path; its sample measurement must take it too.
    const float tipFoot = 1003.0f / 256.0f, tipBase = 6.0f;
    const float tipLeft = 212.0f / 256.0f, tipMiddle = 84.0f / 256.0f, tipRight = -44.0f / 256.0f;
    Check(WallSampleSpan::MeasuredCell(tipFoot, tipBase, tipLeft, tipMiddle, tipRight, true),
          "clipped low outdoor tip is measured on the existing UV path");
    Check(!WallSampleSpan::MeasuredCell(tipFoot, tipBase, tipLeft, tipMiddle, tipRight, false),
          "clipped indoor tip keeps its original path");
    Check(!WallSampleSpan::MeasuredCell(tipFoot, 3.0f, tipLeft, tipMiddle, tipRight, true),
          "unverified raised clipped tip keeps its original path");
    Check(!WallSampleSpan::MeasuredCell(tipFoot, tipBase, 3.1f, 1.5f, tipRight, true),
          "clipped tall cell keeps its original path");
    Check(!WallSampleSpan::MeasuredCell(tipFoot, tipBase, tipLeft, tipMiddle, -1.1f, true),
          "deeply inverted cell is not a clipped eave");
    alignas(4) std::array<unsigned char, 0x200> wall{};
    alignas(4) std::array<unsigned char, 13 * 0x30> samples{};
    alignas(4) std::array<unsigned char, 6 * 0x2c> vertices{};
    const float base = 20.708f, foot = 22.947f, height = 0.976f;
    Put(wall, 0x24, 1);
    Put(wall, 0x48, 1); // class 2, columns
    Put(wall, 0x104, base);
    const uintptr_t start = reinterpret_cast<uintptr_t>(samples.data());
    uintptr_t batch[] = {start, start + samples.size()};
    const WallBlock block{0, 0, 0, 12};
    WallCellH cell{foot, height, height, height, true};
    const auto reset = [&] {
        for (int k = 0; k < 13; ++k) {
            Put(samples, k * 0x30 + 4, base + static_cast<float>(k) * 0.25f);
            Put(samples, k * 0x30 + 0x20, uint16_t{0});
        }
    };
    reset();
    Check(MoveWallFootSamples(reinterpret_cast<uintptr_t>(wall.data()), 2, reinterpret_cast<uintptr_t>(batch), block, &cell, 1) == 13,
          "production sample mover accepts the raised gable");
    for (int k = 0; k < 13; ++k)
        Check(std::fabs(Get<float>(samples, k * 0x30 + 4) - (foot + k * 0.25f)) < 1e-5f, "production sample height follows the drawn base");
    const float y[6] = {foot, foot, foot, foot + height, foot + height, foot + height};
    uintptr_t geo[] = {0, 0, reinterpret_cast<uintptr_t>(vertices.data())};
    for (int mode = 0; mode < 2; ++mode) {
        const size_t at = mode ? 0x1e : 0x12;
        Put(vertices, at, int16_t{2000});
        Put(vertices, 5 * 0x2c + at, int16_t{1000});
        int16_t vb = 0, vt = 0;
        Check(RecutCellV(geo, mode, foot, y, vb, vt), "production UV writer accepts both map sets");
        for (int i = 0; i < 6; ++i) {
            const float u = static_cast<float>(Get<int16_t>(vertices, i * 0x2c + at) - vb) / static_cast<float>(vt - vb);
            Check(std::fabs(foot + 3.0f * u - y[i]) <= 0.0016f, "quantized UV and production sample rows agree in world height");
        }
    }
    cell.set = false;
    reset(); const auto untouched = samples;
    Check(MoveWallFootSamples(reinterpret_cast<uintptr_t>(wall.data()), 2, reinterpret_cast<uintptr_t>(batch), block, &cell, 1) == 0 && samples == untouched,
          "unmeasured cell remains untouched");
    cell = WallCellH{base, 3, 3, 3, true}; reset();
    const auto normal = samples;
    Check(MoveWallFootSamples(reinterpret_cast<uintptr_t>(wall.data()), 2, reinterpret_cast<uintptr_t>(batch), block, &cell, 1) == 0 && samples == normal,
          "ordinary wall sample bytes remain unchanged");
    cell.foot = base - 1.9f; reset();
    Check(MoveWallFootSamples(reinterpret_cast<uintptr_t>(wall.data()), 2, reinterpret_cast<uintptr_t>(batch), block, &cell, 1) == 13,
          "existing lower-foot sample mover is preserved");
    for (int k = 0; k < 13; ++k)
        Check(std::fabs(Get<float>(samples, k * 0x30 + 4) - (cell.foot + k * 0.25f)) < 1e-5f, "lower-foot sample height remains correct");
    // Adjacent captured gable cells share a foot but have different tops. The
    // middle one crossed the old native span; its neighbours did not.
    alignas(4) std::array<unsigned char, 3 * 13 * 0x30> adjacent{};
    Put(wall, 0x24, 3);
    Put(wall, 0x48, 3);
    WallCellH cells[] = {{foot, 0.691f, 0.691f, 0.691f, true},
                        {foot, 0.832f, 0.832f, 0.832f, true},
                        {foot, 0.691f, 0.691f, 0.691f, true}};
    for (int row = 0; row < 13; ++row)
        for (int column = 0; column < 3; ++column) {
            const size_t at = static_cast<size_t>(row * 3 + column) * 0x30;
            Put(adjacent, at + 4, base + row * 0.25f);
            Put(adjacent, at + 0x20, static_cast<uint16_t>(column));
        }
    const uintptr_t adjacentStart = reinterpret_cast<uintptr_t>(adjacent.data());
    uintptr_t adjacentBatch[] = {adjacentStart, adjacentStart + adjacent.size()};
    const WallBlock adjacentBlock{0, 0, 2, 12};
    Check(MoveWallFootSamples(reinterpret_cast<uintptr_t>(wall.data()), 2, reinterpret_cast<uintptr_t>(adjacentBatch), adjacentBlock, cells, 3) == 39,
          "adjacent gable cells all use the same drawn foot");
    for (int row = 0; row < 13; ++row)
        for (int column = 0; column < 3; ++column)
            Check(std::fabs(Get<float>(adjacent, static_cast<size_t>(row * 3 + column) * 0x30 + 4) - (foot + row * 0.25f)) < 1e-5f,
                  "neighbouring gable columns agree at every sample row");
    // Captured facade UVs end at V=4 where the gable geometry starts at V=100.
    // The facade's light base is below its drawn foot. This fixture checks
    // that facade independently of the gable's clipped tip and its sample state.
    const float worldOffset = 47.749054f;
    const float bodyFoot = 837.0f / 256.0f, gableFoot = 1605.0f / 256.0f;
    for (const int cls : {0, 2}) {
        const int rows = cls == 0 ? 4 : 13;
        Put(wall, 0x24, 1);
        Put(wall, cls * 0x10 + 0x28, 1);
        const uintptr_t end = start + rows * 0x30;
        uintptr_t seamBatch[] = {start, end};
        const WallBlock seamBlock{0, 0, 0, rows - 1};
        const auto fillRows = [&](float localBase) {
            Put(wall, 0x104, localBase);
            for (int row = 0; row < rows; ++row) {
                Put(samples, row * 0x30 + 4, worldOffset + localBase + 3.0f * row / (rows - 1));
                Put(samples, row * 0x30 + 0x20, uint16_t{0});
            }
        };
        WallCellH body{bodyFoot, 3.0f, 3.0f, 3.0f, true};
        fillRows(3.0f);
        Check(MoveWallFootSamples(reinterpret_cast<uintptr_t>(wall.data()), cls, reinterpret_cast<uintptr_t>(seamBatch), seamBlock, &body, 1) == rows,
              "raised full-height facade samples move to the captured drawn foot");
        const float wallTop = Get<float>(samples, (rows - 1) * 0x30 + 4);
        Check(std::fabs(wallTop - (worldOffset + gableFoot)) < 1e-5f, "facade's upper sample meets the captured gable geometry");
        const float uvWallTop = (100.0f - 4.0f) / 96.0f, uvGableBottom = (100.0f - 100.0f) / 96.0f;
        Check(uvWallTop == 1.0f && uvGableBottom == 0.0f, "captured UV endpoints represent the facade top and gable foot");
    }
    Check(!WallSampleSpan::ShiftFoot(3.0f, 3.0f, 3, 3, 3), "ordinary aligned facade remains byte-equivalent");
    Check(!WallSampleSpan::ShiftFoot(3.27f, 3.0f, 3.1f, 3.1f, 3.1f), "unverified raised tall spans keep the existing path");
    Put(wall, 0x24, 1);
    Put(wall, 0x48, 1);
    Put(wall, 0x104, tipBase);
    cell = WallCellH{tipFoot, tipLeft, tipMiddle, tipRight,
                    WallSampleSpan::MeasuredCell(tipFoot, tipBase, tipLeft, tipMiddle, tipRight, true)};
    for (int row = 0; row < 13; ++row) {
        Put(samples, row * 0x30 + 4, tipBase + row * 0.25f);
        Put(samples, row * 0x30 + 0x20, uint16_t{0});
    }
    Check(MoveWallFootSamples(reinterpret_cast<uintptr_t>(wall.data()), 2, reinterpret_cast<uintptr_t>(batch), block, &cell, 1) == 13,
          "clipped tip measurement reaches the production sample mover");
    for (int row = 0; row < 13; ++row)
        Check(std::fabs(Get<float>(samples, row * 0x30 + 4) - (tipFoot + row * 0.25f)) < 1e-5f,
              "clipped tip samples match its UV reference at every row");
    const float tipY[] = {tipFoot, tipFoot, tipFoot, tipFoot + tipRight, tipFoot + tipMiddle, tipFoot + tipLeft};
    for (int mode : {0, 1}) {
        const size_t offset = mode ? 0x1e : 0x12;
        Put(vertices, offset, int16_t{100});
        Put(vertices, 5 * 0x2c + offset, int16_t{4});
        int16_t vb = 0, vt = 0;
        Check(RecutCellV(geo, mode, tipFoot, tipY, vb, vt), "clipped tip production UV writer succeeds");
        for (int i = 0; i < 6; ++i) {
            const float uv = static_cast<float>(Get<int16_t>(vertices, i * 0x2c + offset) - vb) / (vt - vb);
            Check(std::fabs(tipFoot + uv * 3.0f - std::clamp(tipY[i], tipFoot, tipFoot + 3.0f)) <= 0.015626f,
                  "clipped tip UV and sample heights agree within half an atlas texel");
        }
    }
    // Native UVs of a half-cell clipped at a window corner. Its left end is
    // extended by 20 fixed-point units; the UV midpoint stays on the grid.
    alignas(4) std::array<unsigned char, 0x120> room{}, owner{};
    owner.fill(0);
    Put(owner, 0x30, 1024);
    Put(room, 0x100, 2);
    Put(wall, 0, reinterpret_cast<uintptr_t>(room.data()));
    Put(wall, 0x24, 5);
    Put(wall, 0x48, 21);
    Put(wall, 0x98, 205);
    Put(wall, 0xA0, 225);
    Put(wall, 0xB4, reinterpret_cast<uintptr_t>(owner.data()));
    Put(wall, 0x100, 29.0f);
    Put(wall, 0x108, 10.0f);
    Put(wall, 0xE0, 5.0f);
    Put(wall, 0xE8, 0.0f);
    Put(wall, 0xF0, 5.0f);
    Put(wall, 0xF8, 0.0f);
    Put(wall, 0x110, 464.305206f);
    Put(wall, 0x118, 323.336536f);
    const int16_t xx[] = {7552, 7478, 7404, 7404, 7478, 7552};
    const int16_t uu[] = {830, 826, 822, 822, 826, 830};
    for (int i = 0; i < 6; ++i) {
        Put(vertices, i * 0x2c, xx[i]);
        Put(vertices, i * 0x2c + 4, int16_t{2540});
        Put(vertices, i * 0x2c + 0x10, uu[i]);
    }
    float offsets[3]{};
    unsigned mask = 0;
    Check(ReadWallColumnAnchors(geo, reinterpret_cast<uintptr_t>(wall.data()), 0, 5, offsets, mask) && mask == 3,
          "production reader finds the two captured corner anchors");
    Check(std::fabs(offsets[0] + 0.015625f) < 1e-6f && std::fabs(offsets[1]) < 1e-6f,
          "corner UVs measure the actual extension rather than a uniform shift");
    WallCellH columnCells[5]{};
    columnCells[0].set = true;
    for (int a = 0; a < 3; ++a)
        if (mask & (1u << a)) WallSampleSpan::NoteColumn(columnCells[0].columns, a, offsets[a], 0.001f);
    alignas(4) std::array<unsigned char, 21 * 0x30> columnSamples{};
    const auto fillColumns = [&] {
        for (int i = 0; i < 21; ++i) {
            Put(columnSamples, i * 0x30, 464.305206f + (i + 0.5f) * 5.0f / 21);
            Put(columnSamples, i * 0x30 + 8, 323.336536f);
            Put(columnSamples, i * 0x30 + 0x20, static_cast<uint16_t>(205 + i));
        }
    };
    fillColumns();
    const auto nativeColumns = columnSamples;
    const uintptr_t columnsStart = reinterpret_cast<uintptr_t>(columnSamples.data());
    uintptr_t columnsBatch[] = {columnsStart, columnsStart + columnSamples.size()};
    const WallBlock columnsBlock{205, 0, 225, 12};
    Check(MoveWallColumnSamples(reinterpret_cast<uintptr_t>(wall.data()), 2, reinterpret_cast<uintptr_t>(columnsBatch), columnsBlock, columnCells, 5) == 3,
          "only measured columns move; hidden or unknown fragments stay native");
    for (int i = 0; i < 3; ++i) {
        const float expected = 435.305206f + (7404.0f + i * 74.0f) / 256;
        Check(std::fabs(Get<float>(columnSamples, i * 0x30) - expected) < 5e-5f,
              "production columns land on the positions encoded by captured corner UVs");
        Check(Get<float>(columnSamples, i * 0x30 + 8) == 323.336536f,
              "native perpendicular bias is preserved");
    }
    for (int i = 3; i < 21; ++i)
        Check(std::memcmp(columnSamples.data() + i * 0x30, nativeColumns.data() + i * 0x30, 0x30) == 0,
              "unmeasured sample bytes remain unchanged");
    fillColumns();
    Put(columnSamples, 20 * 0x30, 999.0f);
    const auto malformed = columnSamples;
    Check(MoveWallColumnSamples(reinterpret_cast<uintptr_t>(wall.data()), 2, reinterpret_cast<uintptr_t>(columnsBatch), columnsBlock, columnCells, 5) == 0 && columnSamples == malformed,
          "the entire native layout is checked before any column changes");
    auto conflicting = columnCells[0].columns;
    Check(WallSampleSpan::NoteColumn(conflicting, 0, 0.1f, 0.001f) && !(conflicting.mask & 1) && (conflicting.rejected & 1),
          "conflicting geometry disables the affected anchor");
    float unavailable = 123;
    Check(!WallSampleSpan::ColumnOffset(conflicting, 0.25f, unavailable) && unavailable == 123,
          "a conflict cannot silently interpolate a different surface");
    for (const auto run : {std::array<float, 2>{0, 5}, std::array<float, 2>{3, 4}, std::array<float, 2>{-5, 0}}) {
        Put(wall, 0xF0, run[0]);
        Put(wall, 0xF8, run[1]);
        for (int i = 0; i < 21; ++i) {
            Put(columnSamples, i * 0x30, 464.305206f + (i + 0.5f) * run[0] / 21);
            Put(columnSamples, i * 0x30 + 8, 323.336536f + (i + 0.5f) * run[1] / 21);
        }
        Check(MoveWallColumnSamples(reinterpret_cast<uintptr_t>(wall.data()), 2, reinterpret_cast<uintptr_t>(columnsBatch), columnsBlock, columnCells, 5) == 3,
              "rotated and reversed walls use the measured column mapping");
        for (int i = 0; i < 3; ++i) {
            const float q = i / 20.0f, delta = -0.015625f * (1 - i * 0.5f);
            Check(std::fabs(Get<float>(columnSamples, i * 0x30) - (464.305206f + (q + delta) * run[0])) < 5e-5f &&
                  std::fabs(Get<float>(columnSamples, i * 0x30 + 8) - (323.336536f + (q + delta) * run[1])) < 5e-5f,
                  "the correction follows the wall tangent in world space");
        }
    }
    Put(wall, 0x24, 1);
    Put(wall, 0x48, 5);
    Put(wall, 0x98, 200);
    Put(wall, 0xA0, 204);
    Put(wall, 0x100, 28.0f);
    Put(wall, 0xE0, 1.0f);
    Put(wall, 0xF0, 1.0f);
    Put(wall, 0xF8, 0.0f);
    Put(wall, 0x110, 463.305206f);
    const int16_t tipX[] = {7168, 7296, 7424, 7424, 7296, 7168};
    const int16_t tipU[] = {802, 810, 818, 818, 810, 802};
    for (int i = 0; i < 6; ++i) {
        Put(vertices, i * 0x2c, tipX[i]);
        Put(vertices, i * 0x2c + 0x10, tipU[i]);
    }
    Check(ReadWallColumnAnchors(geo, reinterpret_cast<uintptr_t>(wall.data()), 0, 1, offsets, mask) && mask == 7,
          "the captured gable supplies all three anchors");
    WallCellH tipColumns{};
    tipColumns.set = true;
    for (int a = 0; a < 3; ++a) WallSampleSpan::NoteColumn(tipColumns.columns, a, offsets[a], 0.001f);
    for (int i = 0; i < 5; ++i) {
        Put(columnSamples, i * 0x30, 463.305206f + (i + 0.5f) / 5);
        Put(columnSamples, i * 0x30 + 8, 323.336536f);
        Put(columnSamples, i * 0x30 + 0x20, static_cast<uint16_t>(200 + i));
    }
    uintptr_t tipBatch[] = {columnsStart, columnsStart + 5 * 0x30};
    Check(std::fabs(Get<float>(columnSamples, 0) - 463.305206f) > 0.09f,
          "the native gable columns reproduce the captured endpoint mismatch");
    Check(MoveWallColumnSamples(reinterpret_cast<uintptr_t>(wall.data()), 2, reinterpret_cast<uintptr_t>(tipBatch), WallBlock{200, 0, 204, 12}, &tipColumns, 1) == 5,
          "all gable columns align to their drawn UV positions");
    for (int i = 0; i < 5; ++i)
        Check(std::fabs(Get<float>(columnSamples, i * 0x30) - (463.305206f + i * 0.25f)) < 5e-5f,
              "every gable column matches the captured mesh");
    std::printf("Native wall span: %d checks passed\n", checks);
}
