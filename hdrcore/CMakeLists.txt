# HDR合成のエンジン（UIに依存しないC++ライブラリ）。
add_library(hdrcore STATIC
    src/raw_frame.cpp
    src/exposure.cpp
    src/merge.cpp
    src/dng_writer.cpp
    src/tiff_builder.cpp
    src/preview.cpp
    src/image_io_mac.cpp
)
target_include_directories(hdrcore PUBLIC include)
target_link_libraries(hdrcore PUBLIC rbh_flags PRIVATE libraw_static z)
target_link_libraries(hdrcore PUBLIC "-framework CoreFoundation" "-framework CoreGraphics" "-framework ImageIO")
