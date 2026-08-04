// bc7enc_rdo_adapter.h
//
// Compressonator CMP_Core → richgel999/bc7enc_rdo (bc7e.ispc) adapter.
// Compiled in when CMP_USE_BC7ENC_RDO is defined at build time.

#ifndef CMP_CORE_BC7ENC_RDO_ADAPTER_H
#define CMP_CORE_BC7ENC_RDO_ADAPTER_H

#ifdef __cplusplus
extern "C" {
#endif

// Strided uint8 RGBA input variant. Matches the signature of
// CompressBlockBC7() in cmp_core.h so it can slot in wherever that is
// called. srcStrideInBytes = row stride of the source image (in bytes).
void CompressBlockBC7_bc7enc(const unsigned char* srcBlock,
                             unsigned int srcStrideInBytes,
                             unsigned char cmpBlock[16]);

// double[16][4] variant (values 0..255) — used by the
// cmp_compressonatorlib per-block loop, which converts to doubles
// before calling the CPU encoder.
void CompressBlockBC7_bc7enc_from_double(const double in[16][4],
                                         unsigned char cmpBlock[16]);

#ifdef __cplusplus
}
#endif

#endif
