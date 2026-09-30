# コマンドライン版（テスト・検証・バッチ処理用）。
add_executable(rawhdr main.cpp)
target_link_libraries(rawhdr PRIVATE hdrcore)
