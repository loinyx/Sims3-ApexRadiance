// Compile the production shader at its actual profile and verify the inverse
// mapping used for native floor maps. No game access or configuration writes.
#define wmain terrain_suite_unused_main
#include "check.cpp"
#undef wmain
int wmain(int argc, wchar_t** argv) {
    if (argc != 2) return 2;
    const auto code = Compile(LotHlsl(argv[1]), "ps_3_0");
    Gpu gpu;
    if (!gpu.dev || code.empty()) return 1;
    auto* shader = gpu.Shader(code);
    Check(shader != nullptr, "production ps_3_0 accepted by D3D9 device");
    for (float scale : {1.f/16,1.f/32,1.f/64,1.f/128,1.f/256}) {
        for (int p = -100; p <= 25600; ++p) {
            const float local = p / 100.f;
            const float native = (local * (63.f/64) + .25f) * scale;
            const float aligned = native * (1.f + 1.f/63) - scale*(16.f/63);
            Check(std::abs(aligned-local*scale) < 4e-6f,
                  "contracted native UV inverted without changing map intensity");
        }
    }
    // (2.15.3) a lot map need not be square (512 x 256: c12 = 1/128, 1/64): each axis takes its own offset (PS c30.zw)
    for (const auto [sx, sz] : {std::pair{1.f/128, 1.f/64}, std::pair{1.f/64, 1.f/128}, std::pair{1.f/256, 1.f/64}}) {
        for (int p = -100; p <= 5000; p += 7) {
            const float lx = p / 100.f, lz = (5000 - p) / 100.f;
            const float ax = ((lx * (63.f/64) + .25f) * sx) * (1.f + 1.f/63) - sx*(16.f/63);
            const float az = ((lz * (63.f/64) + .25f) * sz) * (1.f + 1.f/63) - sz*(16.f/63);
            Check(std::abs(ax-lx*sx) < 4e-6f && std::abs(az-lz*sz) < 4e-6f, "a non-square lot map's UV inverted on each axis");
        }
    }
    const float local = 33.1249129f;
    const float legacyTexel = (local*(63.f/64)+.25f)*4-.5f;
    const float alignedTexel = local*4-.5f;
    Check(legacyTexel < 131 && alignedTexel > 131.99f,
          "captured outside point moves from indoor row to outside row");
    Release(shader);
    std::printf("Lot UV: %d checks, %d failures; shader compile/device and mapping, not gameplay\n", checks, failures);
    return failures ? 1 : 0;
}
