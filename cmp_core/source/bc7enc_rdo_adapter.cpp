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

// Defensive guard: bc7e's handle_alpha_block / handle_opaque_block leave
// opt_results.m_mode uninitialized when every mode in the corresponding
// settings block is disabled, then encode_bc7_block reads the garbage
// mode and crashes on the resulting out-of-bounds bit-writes. This can
// happen when ModeMask + colourRestrict/alphaRestrict together disable
// every mode a block needs. Stock Compressonator hits the same latent
// condition but its release-mode block loop simply falls through with
// no output written, silently producing garbage. Detect the "restricted
// variant has no valid modes for this alpha class" case so we can route
// affected blocks to the unrestricted variant instead of crashing.
bool has_any_alpha_mode(const ispc::bc7e_compress_block_params* p)
{
    return p->m_alpha_settings.m_use_mode4
        || p->m_alpha_settings.m_use_mode5
        || p->m_alpha_settings.m_use_mode6
        || p->m_alpha_settings.m_use_mode7;
}

bool has_any_opaque_mode(const ispc::bc7e_compress_block_params* p)
{
    for (int i = 0; i < 7; ++i)
        if (p->m_opaque_settings.m_use_mode[i]) return true;
    return false;
}

// Thread-local cache. Options usually stay constant across a stripe, so a
// single-slot cache resolves ~100% of calls without re-running the preset
// init (which memsets and populates ~15 fields).
struct params_pair
{
    ispc::bc7e_compress_block_params m_default;
    ispc::bc7e_compress_block_params m_restricted;
    bool                             m_has_restricted;
    // Fallback guards — see has_any_alpha_mode / has_any_opaque_mode.
    // Cleared when the restricted variant would leave a block-type unencodable,
    // in which case choose_params falls back to m_default for that block.
    bool                             m_restricted_alpha_ok;
    bool                             m_restricted_opaque_ok;
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
        out.m_restricted_alpha_ok  = has_any_alpha_mode(&out.m_restricted);
        out.m_restricted_opaque_ok = has_any_opaque_mode(&out.m_restricted);
    }
    else
    {
        out.m_restricted_alpha_ok  = true;
        out.m_restricted_opaque_ok = true;
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
    if (colourRestrict && !blockNeedsAlpha && pp.m_restricted_opaque_ok)
        return &pp.m_restricted;
    if (alphaRestrict  &&  blockNeedsAlpha && blockAlphaZeroOne && pp.m_restricted_alpha_ok)
        return &pp.m_restricted;
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

// ---- Batched entry points -------------------------------------------------
//
// The batch context caches both preset variants once per Compress() call
// (verified constant in Phase 3 part 5 investigation). Per-block work is
// reduced to: (a) double→uint32 pixel pack, (b) alpha scan when restricts
// are enabled, (c) partition into default/restricted sub-batches, and
// (d) one or two bc7e_compress_blocks calls per flush.

struct CMP_bc7enc_BatchContext
{
    ispc::bc7e_compress_block_params m_default;
    ispc::bc7e_compress_block_params m_restricted;
    bool                             m_has_restricted;
    bool                             m_colour_restrict;
    bool                             m_alpha_restrict;
    // Fallback guards — mirror params_pair. If the restricted variant
    // would leave a block class unencodable, block_needs_restricted()
    // returns false for that class so the block routes to m_default.
    bool                             m_restricted_alpha_ok;
    bool                             m_restricted_opaque_ok;
};

extern "C" CMP_bc7enc_BatchContext*
CompressBlockBC7_bc7enc_batch_create(const CMP_bc7enc_Options* opts)
{
    ensure_lib_init();

    auto* ctx = new CMP_bc7enc_BatchContext();

    const bool perceptual = (opts->perceptual != 0);
    select_preset(opts->quality, perceptual, &ctx->m_default);
    apply_mode_mask(opts->validModeMask, &ctx->m_default);

    ctx->m_colour_restrict = (opts->colourRestrict != 0);
    ctx->m_alpha_restrict  = (opts->alphaRestrict  != 0);
    ctx->m_has_restricted  = ctx->m_colour_restrict || ctx->m_alpha_restrict;

    if (ctx->m_has_restricted)
    {
        std::memcpy(&ctx->m_restricted, &ctx->m_default, sizeof(ctx->m_default));
        restrict_modes_67(&ctx->m_restricted, ctx->m_colour_restrict, ctx->m_alpha_restrict);
        ctx->m_restricted_alpha_ok  = has_any_alpha_mode(&ctx->m_restricted);
        ctx->m_restricted_opaque_ok = has_any_opaque_mode(&ctx->m_restricted);
    }
    else
    {
        ctx->m_restricted_alpha_ok  = true;
        ctx->m_restricted_opaque_ok = true;
    }

    return ctx;
}

extern "C" void CompressBlockBC7_bc7enc_batch_destroy(CMP_bc7enc_BatchContext* ctx)
{
    delete ctx;
}

namespace
{

// Alpha scan matching notValidBlockForMode() at bc7_encode.cpp:2633-2661
// and the per-block choose_params() above. Returns true if the block
// should use the restricted params variant. The `..._ok` flags are the
// fallback guards from CMP_bc7enc_BatchContext — when the restricted
// variant would leave a block class unencodable, we refuse to restrict
// that block (route it to m_default instead).
bool block_needs_restricted(const uint32_t pixels[16],
                            bool colour_restrict, bool alpha_restrict,
                            bool restricted_opaque_ok, bool restricted_alpha_ok)
{
    bool blockNeedsAlpha   = false;
    bool blockAlphaZeroOne = false;
    for (int i = 0; i < 16; ++i)
    {
        const uint8_t a = static_cast<uint8_t>(pixels[i] >> 24);
        if (a != 255)             blockNeedsAlpha   = true;
        if (a == 0 || a == 255)   blockAlphaZeroOne = true;
    }
    if (colour_restrict && !blockNeedsAlpha && restricted_opaque_ok)
        return true;
    if (alpha_restrict  &&  blockNeedsAlpha && blockAlphaZeroOne && restricted_alpha_ok)
        return true;
    return false;
}

} // namespace

extern "C" void CompressBlockBC7_bc7enc_batch_from_double(CMP_bc7enc_BatchContext* ctx,
                                                          const double* inputs,
                                                          unsigned char* outputs,
                                                          unsigned int count)
{
    if (count == 0) return;
    if (count > CMP_BC7ENC_BATCH_N) return;  // Caller contract violation; drop.

    // Pack all inputs to uint32 RGBA once. 64 blocks * 16 pixels * 4 bytes = 4KB.
    uint32_t all_pixels[CMP_BC7ENC_BATCH_N * 16];
    for (unsigned int b = 0; b < count; ++b)
    {
        const double* blk = inputs + b * 16 * 4;
        uint32_t* dst = all_pixels + b * 16;
        for (int i = 0; i < 16; ++i)
        {
            const uint32_t r = clamp_u8(blk[i * 4 + 0]);
            const uint32_t g = clamp_u8(blk[i * 4 + 1]);
            const uint32_t b_ = clamp_u8(blk[i * 4 + 2]);
            const uint32_t a = clamp_u8(blk[i * 4 + 3]);
            dst[i] = r | (g << 8) | (b_ << 16) | (a << 24);
        }
    }

    // Fast path: no restrict variant in play — single bc7e call, direct output.
    if (!ctx->m_has_restricted)
    {
        uint64_t out_blocks[CMP_BC7ENC_BATCH_N * 2];
        ispc::bc7e_compress_blocks(count,
                                   out_blocks,
                                   all_pixels,
                                   &ctx->m_default);
        std::memcpy(outputs, out_blocks, count * 16);
        return;
    }

    // Option 3a partition: classify each block, gather into two contiguous
    // sub-batches, one bc7e call per non-empty sub-batch, scatter results back.
    uint32_t def_pixels[CMP_BC7ENC_BATCH_N * 16];
    uint32_t res_pixels[CMP_BC7ENC_BATCH_N * 16];
    uint8_t  def_indices[CMP_BC7ENC_BATCH_N];
    uint8_t  res_indices[CMP_BC7ENC_BATCH_N];
    unsigned int def_count = 0;
    unsigned int res_count = 0;

    for (unsigned int b = 0; b < count; ++b)
    {
        const uint32_t* src = all_pixels + b * 16;
        if (block_needs_restricted(src, ctx->m_colour_restrict, ctx->m_alpha_restrict,
                                    ctx->m_restricted_opaque_ok, ctx->m_restricted_alpha_ok))
        {
            std::memcpy(res_pixels + res_count * 16, src, 16 * sizeof(uint32_t));
            res_indices[res_count++] = static_cast<uint8_t>(b);
        }
        else
        {
            std::memcpy(def_pixels + def_count * 16, src, 16 * sizeof(uint32_t));
            def_indices[def_count++] = static_cast<uint8_t>(b);
        }
    }

    uint64_t def_out[CMP_BC7ENC_BATCH_N * 2];
    uint64_t res_out[CMP_BC7ENC_BATCH_N * 2];

    if (def_count > 0)
    {
        ispc::bc7e_compress_blocks(def_count, def_out, def_pixels, &ctx->m_default);
        for (unsigned int i = 0; i < def_count; ++i)
        {
            std::memcpy(outputs + def_indices[i] * 16, def_out + i * 2, 16);
        }
    }
    if (res_count > 0)
    {
        ispc::bc7e_compress_blocks(res_count, res_out, res_pixels, &ctx->m_restricted);
        for (unsigned int i = 0; i < res_count; ++i)
        {
            std::memcpy(outputs + res_indices[i] * 16, res_out + i * 2, 16);
        }
    }
}
