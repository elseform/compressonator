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

// ---- Batched entry points (Phase 3 part 5) -------------------------------
//
// bc7e_compress_blocks amortizes per-call setup and packs SIMD lanes over
// multiple blocks; calling it one-block-at-a-time (as the per-block entry
// points above do) leaves that on the table. The batch API takes a
// prebuilt context — quality/mask/restrict are constant across an entire
// CCodec_BC7::Compress() call (verified against BC7BlockEncoder member
// lifetime), so the params-pair only needs to be built once per call,
// not once per block.
//
// Restrict handling per NOTES.md option 3a: the batch call scans each
// block's alpha, partitions into default + restricted sub-batches, and
// issues 1 or 2 bc7e_compress_blocks calls per flush before scattering
// outputs back into original positions.
//
// Recommended batch size: CMP_BC7ENC_BATCH_N (see below). Callers may
// pass smaller counts (tail of a row); larger counts are rejected.

#define CMP_BC7ENC_BATCH_N 64

typedef struct CMP_bc7enc_BatchContext CMP_bc7enc_BatchContext;

CMP_bc7enc_BatchContext* CompressBlockBC7_bc7enc_batch_create(const CMP_bc7enc_Options* opts);
void                     CompressBlockBC7_bc7enc_batch_destroy(CMP_bc7enc_BatchContext* ctx);

// inputs: pointer to `count` contiguous blocks of double[16][4] each.
// outputs: `count * 16` contiguous bytes.
// count: 1 .. CMP_BC7ENC_BATCH_N. Values outside this range are asserted.
void CompressBlockBC7_bc7enc_batch_from_double(CMP_bc7enc_BatchContext* ctx,
                                               const double* inputs,
                                               unsigned char* outputs,
                                               unsigned int count);

#ifdef __cplusplus
}
#endif

#endif
