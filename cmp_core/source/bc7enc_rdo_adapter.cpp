// bc7enc_rdo_adapter.cpp
//
// Thin per-block wrapper around bc7enc_rdo's ISPC-vectorized bc7e encoder.
// See NOTES.md Phase 2 for rationale — this is the additive hook exposed
// under CMP_USE_BC7ENC_RDO; the stock BC7 codec remains reachable when
// the flag is off.

#include "bc7enc_rdo_adapter.h"

#include <cstdint>
#include <cstring>
#include <mutex>

#include "bc7e_ispc.h"

namespace {

ispc::bc7e_compress_block_params g_params;
std::once_flag                    g_init_flag;

void ensure_init()
{
    std::call_once(g_init_flag, []() {
        ispc::bc7e_compress_block_init();
        // "slowest" = highest quality mode preset; matches the intent of
        // Compressonator's default quality=1.0. Perceptual=false so
        // metrics stay comparable to DirectXTex / stock CMP_Core.
        ispc::bc7e_compress_block_params_init_slowest(&g_params, false);
    });
}

inline uint8_t clamp_u8(double v)
{
    if (v <= 0.0)   return 0;
    if (v >= 255.0) return 255;
    return static_cast<uint8_t>(v);
}

inline void encode_one(const uint32_t pixels[16], unsigned char cmpBlock[16])
{
    uint64_t out[2];
    ispc::bc7e_compress_blocks(1, out, pixels, &g_params);
    std::memcpy(cmpBlock, out, 16);
}

} // namespace

extern "C" void CompressBlockBC7_bc7enc(const unsigned char* srcBlock,
                                        unsigned int srcStrideInBytes,
                                        unsigned char cmpBlock[16])
{
    ensure_init();

    uint32_t pixels[16];
    for (unsigned int y = 0; y < 4; ++y)
    {
        const unsigned char* row = srcBlock + y * srcStrideInBytes;
        for (unsigned int x = 0; x < 4; ++x)
        {
            const unsigned char r = row[x * 4 + 0];
            const unsigned char g = row[x * 4 + 1];
            const unsigned char b = row[x * 4 + 2];
            const unsigned char a = row[x * 4 + 3];
            pixels[y * 4 + x] =
                (uint32_t)r
                | ((uint32_t)g << 8)
                | ((uint32_t)b << 16)
                | ((uint32_t)a << 24);
        }
    }
    encode_one(pixels, cmpBlock);
}

extern "C" void CompressBlockBC7_bc7enc_from_double(const double in[16][4],
                                                    unsigned char cmpBlock[16])
{
    ensure_init();

    uint32_t pixels[16];
    for (int i = 0; i < 16; ++i)
    {
        const uint32_t r = clamp_u8(in[i][0]);
        const uint32_t g = clamp_u8(in[i][1]);
        const uint32_t b = clamp_u8(in[i][2]);
        const uint32_t a = clamp_u8(in[i][3]);
        pixels[i] = r | (g << 8) | (b << 16) | (a << 24);
    }
    encode_one(pixels, cmpBlock);
}
