// bc7enc_rdo_adapter.cpp
//
// Thin per-block wrapper around bc7enc_rdo's ISPC-vectorized bc7e encoder.
// See NOTES.md Phase 2 (hook rationale) and Phase 3 part 2 (options
// mapping) for the design derivation.

#include "bc7enc_rdo_adapter.h"

#include <cstdint>
#include <cstring>
#include <mutex>

#include "bc7e_ispc.h"

namespace
{

std::once_flag g_lib_init_flag;

void ensure_lib_init()
{
    std::call_once(g_lib_init_flag, []() { ispc::bc7e_compress_block_init(); });
}

inline uint8_t clamp_u8(double v)
{
    if (v <= 0.0)   return 0;
    if (v >= 255.0) return 255;
    return static_cast<uint8_t>(v);
}

// ---- Options → bc7e params -----------------------------------------------
//
// See NOTES.md §"Phase 3 (part 2)" for the full derivation. Summary:
//   quality (0..1)  → preset bucket. The `ultrafast` tier is intentionally
//                     dropped and its range folded into `veryfast`. Part 3
//                     showed ultrafast loses 2–4 dB PSNR vs stock's
//                     adaptive early-out on photo/noisy content, so biasing
//                     the lowest bucket up preserves quality at the low end
//                     without giving up bc7e's speed advantage elsewhere.
//   validModeMask   → post-init override of m_use_mode[] in both sub-structs.
//   colourRestrict /
//   alphaRestrict   → per-block decision (16-pixel alpha scan); adapter
//                     holds a default and a modes-6/7-restricted variant
//                     and swaps per block, matching notValidBlockForMode()
//                     semantics in bc7_encode.cpp.
//   errorThreshold, minThreshold, maxThreshold, imageNeedsAlpha → unmapped.
//     bc7e has no adaptive early-out; imageNeedsAlpha is a dead field.

void select_preset(double quality, bool perceptual,
                   ispc::bc7e_compress_block_params* p)
{
    // Bucket boundaries mirror bc7e's own preset granularity; see NOTES.md.
    // `ultrafast` is deliberately absent — see file header comment.
    //
    // Upstream Compressonator passes quality through a CODECFLOAT (float)
    // cast in compress.cpp before it reaches the codec, so values like
    // 0.45 and 0.65 arrive here as 0.4499999... / 0.6499999... and would
    // fall into the wrong bucket if compared directly against the double
    // literals. Rounding to an integer centibucket first absorbs that
    // precision loss regardless of where in the call chain it happens.
    const int cb = static_cast<int>(quality * 100.0 + 0.5);
    if      (cb < 25) ispc::bc7e_compress_block_params_init_veryfast(p, perceptual);
    else if (cb < 45) ispc::bc7e_compress_block_params_init_fast    (p, perceptual);
    else if (cb < 65) ispc::bc7e_compress_block_params_init_basic   (p, perceptual);
    else if (cb < 85) ispc::bc7e_compress_block_params_init_slow    (p, perceptual);
    else              ispc::bc7e_compress_block_params_init_slowest (p, perceptual);
}

void apply_mode_mask(uint8_t mask, ispc::bc7e_compress_block_params* p)
{
    for (int i = 0; i < 7; ++i)
    {
        if (!((mask >> i) & 1)) p->m_opaque_settings.m_use_mode[i] = false;
    }
    if (!((mask >> 4) & 1)) p->m_alpha_settings.m_use_mode4 = false;
    if (!((mask >> 5) & 1)) p->m_alpha_settings.m_use_mode5 = false;
    if (!((mask >> 6) & 1)) p->m_alpha_settings.m_use_mode6 = false;
    if (!((mask >> 7) & 1)) p->m_alpha_settings.m_use_mode7 = false;
}

void restrict_modes_67(ispc::bc7e_compress_block_params* p,
                       bool colourRestrict, bool alphaRestrict)
{
    if (colourRestrict) p->m_opaque_settings.m_use_mode[6] = false;
    if (alphaRestrict)
    {
        p->m_alpha_settings.m_use_mode6 = false;
        p->m_alpha_settings.m_use_mode7 = false;
    }
}

// Thread-local cache. Options usually stay constant across a stripe, so a
// single-slot cache resolves ~100% of calls without re-running the preset
// init (which memsets and populates ~15 fields).
struct params_pair
{
    ispc::bc7e_compress_block_params m_default;
    ispc::bc7e_compress_block_params m_restricted;
    bool                             m_has_restricted;
    double                           m_key_quality;
    uint8_t                          m_key_mask;
    bool                             m_key_colour;
    bool                             m_key_alpha;
    bool                             m_key_perceptual;
    bool                             m_initialized;
};

thread_local params_pair tls_params = {};

bool key_matches(const params_pair& p, const CMP_bc7enc_Options& o)
{
    return p.m_initialized
        && p.m_key_quality    == o.quality
        && p.m_key_mask       == o.validModeMask
        && p.m_key_colour     == static_cast<bool>(o.colourRestrict)
        && p.m_key_alpha      == static_cast<bool>(o.alphaRestrict)
        && p.m_key_perceptual == static_cast<bool>(o.perceptual);
}

void build_params_pair(const CMP_bc7enc_Options& o, params_pair& out)
{
    const bool perceptual = (o.perceptual != 0);
    select_preset(o.quality, perceptual, &out.m_default);
    apply_mode_mask(o.validModeMask, &out.m_default);
    out.m_has_restricted = (o.colourRestrict != 0) || (o.alphaRestrict != 0);
    if (out.m_has_restricted)
    {
        std::memcpy(&out.m_restricted, &out.m_default, sizeof(out.m_default));
        restrict_modes_67(&out.m_restricted,
                          o.colourRestrict != 0, o.alphaRestrict != 0);
    }
    out.m_key_quality    = o.quality;
    out.m_key_mask       = o.validModeMask;
    out.m_key_colour     = (o.colourRestrict != 0);
    out.m_key_alpha      = (o.alphaRestrict  != 0);
    out.m_key_perceptual = perceptual;
    out.m_initialized    = true;
}

const ispc::bc7e_compress_block_params*
choose_params(const uint32_t pixels[16], const params_pair& pp,
              bool colourRestrict, bool alphaRestrict)
{
    if (!pp.m_has_restricted) return &pp.m_default;
    // Alpha scan matching notValidBlockForMode() at bc7_encode.cpp:2633-2661.
    bool blockNeedsAlpha   = false;
    bool blockAlphaZeroOne = false;
    for (int i = 0; i < 16; ++i)
    {
        const uint8_t a = static_cast<uint8_t>(pixels[i] >> 24);
        if (a != 255)             blockNeedsAlpha   = true;
        if (a == 0 || a == 255)   blockAlphaZeroOne = true;
    }
    if (colourRestrict && !blockNeedsAlpha)                        return &pp.m_restricted;
    if (alphaRestrict  &&  blockNeedsAlpha && blockAlphaZeroOne)   return &pp.m_restricted;
    return &pp.m_default;
}

inline void encode_one(const uint32_t pixels[16], unsigned char cmpBlock[16],
                       const ispc::bc7e_compress_block_params* p)
{
    uint64_t out[2];
    ispc::bc7e_compress_blocks(1, out, pixels, p);
    std::memcpy(cmpBlock, out, 16);
}

// Defaults mirror BC7_Encode's SetDefaultBC7Options
// (cmp_core/shaders/bc7_encode_kernel.h:958). Used by the zero-arg entry
// points so any caller that hasn't been threaded to the new API still
// gets sensible behavior.
const CMP_bc7enc_Options DEFAULT_OPTIONS = {
    /*quality*/         1.0,
    /*validModeMask*/   0xFF,
    /*colourRestrict*/  0,
    /*alphaRestrict*/   0,
    /*perceptual*/      0,
};

} // namespace

extern "C" void CompressBlockBC7_bc7enc(const unsigned char* srcBlock,
                                        unsigned int srcStrideInBytes,
                                        unsigned char cmpBlock[16])
{
    CompressBlockBC7_bc7enc_opts(srcBlock, srcStrideInBytes, cmpBlock,
                                 &DEFAULT_OPTIONS);
}

extern "C" void CompressBlockBC7_bc7enc_from_double(const double in[16][4],
                                                    unsigned char cmpBlock[16])
{
    CompressBlockBC7_bc7enc_from_double_opts(in, cmpBlock, &DEFAULT_OPTIONS);
}

extern "C" void CompressBlockBC7_bc7enc_opts(const unsigned char* srcBlock,
                                             unsigned int srcStrideInBytes,
                                             unsigned char cmpBlock[16],
                                             const CMP_bc7enc_Options* opts)
{
    ensure_lib_init();

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

    if (!key_matches(tls_params, *opts)) build_params_pair(*opts, tls_params);
    const auto* p = choose_params(pixels, tls_params,
                                  opts->colourRestrict != 0,
                                  opts->alphaRestrict  != 0);
    encode_one(pixels, cmpBlock, p);
}

extern "C" void CompressBlockBC7_bc7enc_from_double_opts(const double in[16][4],
                                                         unsigned char cmpBlock[16],
                                                         const CMP_bc7enc_Options* opts)
{
    ensure_lib_init();

    uint32_t pixels[16];
    for (int i = 0; i < 16; ++i)
    {
        const uint32_t r = clamp_u8(in[i][0]);
        const uint32_t g = clamp_u8(in[i][1]);
        const uint32_t b = clamp_u8(in[i][2]);
        const uint32_t a = clamp_u8(in[i][3]);
        pixels[i] = r | (g << 8) | (b << 16) | (a << 24);
    }

    if (!key_matches(tls_params, *opts)) build_params_pair(*opts, tls_params);
    const auto* p = choose_params(pixels, tls_params,
                                  opts->colourRestrict != 0,
                                  opts->alphaRestrict  != 0);
    encode_one(pixels, cmpBlock, p);
}
