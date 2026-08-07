cmake_minimum_required(VERSION 3.10)

add_library(CMP_Core STATIC)

target_sources(CMP_Core
    PRIVATE
    shaders/bc1_encode_kernel.h
    shaders/bc1_common_kernel.h
    shaders/bc1_encode_kernel.cpp
    shaders/bc2_encode_kernel.h
    shaders/bc2_encode_kernel.cpp
    shaders/bc3_encode_kernel.h
    shaders/bc3_encode_kernel.cpp
    shaders/bc4_encode_kernel.h
    shaders/bc4_encode_kernel.cpp
    shaders/bc5_encode_kernel.h
    shaders/bc5_encode_kernel.cpp
    shaders/bc6_encode_kernel.h
    shaders/bc6_encode_kernel.cpp
    shaders/bc7_encode_kernel.h
    shaders/bc7_common_encoder.h
    shaders/bc7_encode_kernel.cpp
    shaders/bcn_common_kernel.h
    shaders/bcn_common_api.h
    shaders/common_def.h

    source/cmp_core.h
    source/cmp_core.cpp
    source/cmp_math_vec4.h
    source/cmp_math_func.h

    ../applications/_libs/cmp_math/cpu_extensions.cpp
    ../applications/_libs/cmp_math/cmp_math_common.cpp
)

if (OPTION_CMP_GTC)
    target_sources(
        CMP_Core
        PRIVATE
        shaders/gtc_encode_kernel.h
        shaders/gtc_encode_kernel.cpp
    )
endif()

if (OPTION_BUILD_BROTLIG)
    target_sources(
        CMP_Core
        PRIVATE
        shaders/brlg_encode_kernel.h
        shaders/brlg_encode_kernel.cpp
    )
endif()

target_include_directories(CMP_Core
    PUBLIC
    shaders
    source
    ../applications/_libs/cmp_math/
)

if (UNIX)
    target_compile_definitions(CMP_Core PRIVATE _LINUX ASPM_GPU)
endif()

set_target_properties(CMP_Core PROPERTIES FOLDER ${PROJECT_FOLDER_SDK_LIBS})

# Core SIMD options

# SSE
add_library(CMP_Core_SSE STATIC)
target_sources(CMP_Core_SSE PRIVATE source/core_simd_sse.cpp)
target_include_directories(CMP_Core_SSE PRIVATE source shaders)

if (UNIX)
    target_compile_options(CMP_Core_SSE PRIVATE -march=nehalem)
endif()

set_target_properties(CMP_Core_SSE PROPERTIES FOLDER ${PROJECT_FOLDER_SDK_LIBS})

# AVX
add_library(CMP_Core_AVX STATIC)
target_sources(CMP_Core_AVX PRIVATE source/core_simd_avx.cpp)
target_include_directories(CMP_Core_AVX PRIVATE source shaders)

if (WIN32)
    target_compile_options(CMP_Core_AVX PRIVATE /arch:AVX2)
else()
    target_compile_options(CMP_Core_AVX PRIVATE -march=haswell)
endif()

set_target_properties(CMP_Core_AVX PROPERTIES FOLDER ${PROJECT_FOLDER_SDK_LIBS})

# AVX-512
add_library(CMP_Core_AVX512 STATIC)
target_sources(CMP_Core_AVX512 PRIVATE source/core_simd_avx512.cpp)
target_include_directories(CMP_Core_AVX512 PRIVATE source shaders)

if (WIN32)
    target_compile_options(CMP_Core_AVX512 PRIVATE /arch:AVX512)
else()
    target_compile_options(CMP_Core_AVX512 PRIVATE -march=skylake-avx512)
endif()

set_target_properties(CMP_Core_AVX512 PROPERTIES FOLDER ${PROJECT_FOLDER_SDK_LIBS})

# Link SIMD libraries to CMP_Core
target_link_libraries(CMP_Core PRIVATE CMP_Core_SSE CMP_Core_AVX CMP_Core_AVX512)

# ------------------------------------------------------------------------
# Optional: route the CPU BC7 encoder through richgel999/bc7enc_rdo's
# bc7e.ispc SIMD encoder. Additive — stock BC7BlockEncoder stays in place
# and is selected when this option is OFF.
# ------------------------------------------------------------------------
option(OPTION_CMP_USE_BC7ENC_RDO "Route CPU BC7 encode through bc7enc_rdo (bc7e.ispc)" OFF)

if (OPTION_CMP_USE_BC7ENC_RDO)
    set(BC7ENC_RDO_DIR "${CMAKE_SOURCE_DIR}/../bc7enc_rdo" CACHE PATH
        "Path to richgel999/bc7enc_rdo checkout")
    set(BC7ENC_RDO_ISPC "${CMAKE_SOURCE_DIR}/../tools/ispc/linux/bin/ispc" CACHE FILEPATH
        "Path to the ispc compiler binary")

    if (NOT EXISTS "${BC7ENC_RDO_DIR}/bc7e.ispc")
        message(FATAL_ERROR "OPTION_CMP_USE_BC7ENC_RDO=ON but ${BC7ENC_RDO_DIR}/bc7e.ispc not found. Set BC7ENC_RDO_DIR to the bc7enc_rdo checkout.")
    endif()
    if (NOT EXISTS "${BC7ENC_RDO_ISPC}")
        message(FATAL_ERROR "OPTION_CMP_USE_BC7ENC_RDO=ON but ispc binary not found at ${BC7ENC_RDO_ISPC}. Set BC7ENC_RDO_ISPC.")
    endif()

    set(BC7E_OBJ_DIR "${CMAKE_CURRENT_BINARY_DIR}/bc7e_ispc")
    file(MAKE_DIRECTORY "${BC7E_OBJ_DIR}")

    set(BC7E_OBJS
        "${BC7E_OBJ_DIR}/bc7e.o"
        "${BC7E_OBJ_DIR}/bc7e_sse2.o"
        "${BC7E_OBJ_DIR}/bc7e_sse4.o"
        "${BC7E_OBJ_DIR}/bc7e_avx.o"
        "${BC7E_OBJ_DIR}/bc7e_avx2.o"
    )
    set(BC7E_HEADER "${BC7E_OBJ_DIR}/bc7e_ispc.h")

    add_custom_command(
        OUTPUT ${BC7E_OBJS} ${BC7E_HEADER}
        COMMAND ${BC7ENC_RDO_ISPC} --pic -g -O2
                "${BC7ENC_RDO_DIR}/bc7e.ispc"
                -o "${BC7E_OBJ_DIR}/bc7e.o"
                -h "${BC7E_HEADER}"
                --target=sse2,sse4,avx,avx2
                --opt=fast-math --opt=disable-assertions
        DEPENDS "${BC7ENC_RDO_DIR}/bc7e.ispc"
        COMMENT "Generating bc7e.ispc objects for CMP_Core"
        VERBATIM
    )

    add_custom_target(cmp_core_bc7e_ispc DEPENDS ${BC7E_OBJS} ${BC7E_HEADER})
    add_dependencies(CMP_Core cmp_core_bc7e_ispc)

    target_sources(CMP_Core PRIVATE
        source/bc7enc_rdo_adapter.h
        source/bc7enc_rdo_adapter.cpp
        ${BC7E_OBJS}
    )
    set_source_files_properties(${BC7E_OBJS} PROPERTIES
        EXTERNAL_OBJECT TRUE
        GENERATED TRUE
    )
    target_include_directories(CMP_Core PRIVATE "${BC7E_OBJ_DIR}")
    target_compile_definitions(CMP_Core PUBLIC CMP_USE_BC7ENC_RDO=1)

    # Optional producer-side batching (Phase 3 part 5). Groups blocks into
    # BATCH_N=64 chunks before calling into bc7e_compress_blocks, amortizing
    # per-call setup and packing SIMD lanes. Requires OPTION_CMP_USE_BC7ENC_RDO
    # to be on. Kept as a separate toggle so the per-block bc7e path remains
    # reachable for correctness A/B verification.
    option(OPTION_CMP_USE_BC7ENC_RDO_BATCH "Producer-side block batching for bc7e (requires OPTION_CMP_USE_BC7ENC_RDO)" OFF)
    if (OPTION_CMP_USE_BC7ENC_RDO_BATCH)
        target_compile_definitions(CMP_Core PUBLIC CMP_USE_BC7ENC_RDO_BATCH=1)
    endif()
endif()