# JPEGDEC 1.8.4 by BitBank Software (https://github.com/bitbank2/JPEGDEC, Apache-2.0, see LICENSE): src/ holds the
# upstream files unchanged (its own CMakeLists.txt requires arduino-esp32). jpegdec_grey.cpp is the C entry point the
# firmware uses: a JPEG straight to grey at 1/1 .. 1/8 scale.
idf_component_register(
    SRCS src/JPEGDEC.cpp jpegdec_grey.cpp
    INCLUDE_DIRS include
    PRIV_INCLUDE_DIRS src
)
# upstream code, kept as it is
target_compile_options(${COMPONENT_LIB} PRIVATE -w)
