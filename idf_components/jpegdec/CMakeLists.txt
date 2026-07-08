idf_component_register(
    SRCS
        JPEGDEC.cpp
        jpeg.inl
        s3_simd_420.S
        s3_simd_444.S
        s3_simd_dequant.S
    INCLUDE_DIRS
        include
    REQUIRES
        esp-dsp
)

if(CONFIG_IDF_TARGET_ESP32S3)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE JPEGDEC_HAVE_S3_SIMD=1)
endif()