# エンジンのテスト（合成データで露出比・合成・DNG の読み戻しを確かめる）。
add_executable(core_tests core_tests.cpp)
target_link_libraries(core_tests PRIVATE hdrcore libraw_static)
add_test(NAME core_tests COMMAND core_tests)
