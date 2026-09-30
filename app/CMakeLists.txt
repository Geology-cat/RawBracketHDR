# AppKit アプリ（Objective-C++、ARC）。
#
# エンジン（hdrcore）を直接呼ぶ。SwiftUI は 10.15 以降でしか使えないため、UI は AppKit をコードで組む
# （xib は使わない。10.13 から最新まで同じコードで動かすため）。
enable_language(OBJCXX)

set(RBH_THIRD_PARTY_NOTICES
    "${PROJECT_SOURCE_DIR}/third_party/LibRaw/LICENSE.LGPL"
    "${PROJECT_SOURCE_DIR}/third_party/LibRaw/COPYRIGHT"
    "${PROJECT_SOURCE_DIR}/third_party/LibRaw/README.RawBracketHDR.md"
)
set_source_files_properties(${RBH_THIRD_PARTY_NOTICES} PROPERTIES MACOSX_PACKAGE_LOCATION "Resources/ThirdParty/LibRaw")

add_executable(RawBracketHDRApp MACOSX_BUNDLE
    src/main.mm
    src/AppDelegate.mm
    src/MainWindowController.mm
    src/PreviewView.mm
    ${RBH_THIRD_PARTY_NOTICES}
)
target_include_directories(RawBracketHDRApp PRIVATE src)
target_link_libraries(RawBracketHDRApp PRIVATE hdrcore rbh_flags "-framework Cocoa")
target_compile_options(RawBracketHDRApp PRIVATE -fobjc-arc)
set_target_properties(RawBracketHDRApp PROPERTIES
    # バンドル名と実行ファイル名を揃える（食い違うと open で起動したときに黙って終了する）。
    OUTPUT_NAME "RawBracketHDR"
    OBJCXX_STANDARD 17
    OBJCXX_STANDARD_REQUIRED ON
    MACOSX_BUNDLE_BUNDLE_NAME "RawBracketHDR"
    MACOSX_BUNDLE_GUI_IDENTIFIER "io.github.geology-cat.rawbrackethdr"
    MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}"
    MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}"
    MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/Info.plist.in"
)
