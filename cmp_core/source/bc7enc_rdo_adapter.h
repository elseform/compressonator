// bc7enc_rdo_adapter.h
//
// Compressonator CMP_Core → richgel999/bc7enc_rdo (bc7e.ispc) adapter.
// Compiled in when CMP_USE_BC7ENC_RDO is defined at build time.
//
// The options-taking `_opts` variants apply the Phase 3 part 2 mapping
// (see NOTES.md): quality → preset bucket, validModeMask → m_use_mode
// override, colourRestrict/alphaRestrict → per-block decision between
// a default and a modes-6/7-restricted params variant.

#ifndef CMP_CORE_BC7ENC_RDO_ADAPTER_H
#define CMP_CORE_BC7ENC_RDO_ADAPTER_H

#ifdef __cplusplus
extern "C" {
#endif

typedef struct
{
    double        quality;        // 0..1, clamped by caller
    unsigned char validModeMask;  // bit i = mode i; default 0xFF
    unsigned char colourRestrict; // bool: restrict modes 6/7 on opaque blocks
    unsigned char alphaRestrict;  // bool: restrict modes 6/7 on 0/1-alpha blocks
    unsigned char perceptual;     // bool: YCbCrA error metric (default 0)
} CMP_bc7enc_Options;

// Strided uint8 RGBA input variant. Matches the signature of
// CompressBlockBC7() in cmp_core.h so it can slot in wherever that is
// called. srcStrideInBytes = row stride of the source image (in bytes).
// The zero-options entry point uses BC7_Encode's stock defaults
// (quality=1.0, validModeMask=0xFF, no restricts).
void CompressBlockBC7_bc7enc(const unsigned char* srcBlock,
                             unsigned int srcStrideInBytes,
                             unsigned char cmpBlock[16]);

void CompressBlockBC7_bc7enc_opts(const unsigned char* srcBlock,
                                  unsigned int srcStrideInBytes,
                                  unsigned char cmpBlock[16],
                                  const CMP_bc7enc_Options* opts);

// double[16][4] variant (values 0..255) — used by the
// cmp_compressonatorlib per-block loop, which converts to doubles
// before calling the CPU encoder.
void CompressBlockBC7_bc7enc_from_double(const double in[16][4],
                                         unsigned char cmpBlock[16]);

void CompressBlockBC7_bc7enc_from_double_opts(const double in[16][4],
                                              unsigned char cmpBlock[16],
                                              const CMP_bc7enc_Options* opts);

#ifdef __cplusplus
}
#endif

#endif
