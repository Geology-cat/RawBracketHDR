#import "MainWindowController.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <exception>
#include <string>
#include <vector>

#include "hdrcore/dng_template.hpp"
#include "hdrcore/dng_writer.hpp"
#include "hdrcore/output_format.hpp"
#include "hdrcore/exposure.hpp"
#include "hdrcore/merge.hpp"
#include "hdrcore/preview.hpp"
#include "hdrcore/raw_frame.hpp"

#import "PreviewView.h"

// ---- 表示用の1行 ----------------------------------------------------------------------

@interface FrameRow : NSObject
@property(nonatomic, copy) NSString* number;
@property(nonatomic, copy) NSString* name;
@property(nonatomic, copy) NSString* shutter;
@property(nonatomic, copy) NSString* relativeEV;
@property(nonatomic, copy) NSString* clip;
@property(nonatomic, copy) NSString* tooltip;
@property(nonatomic) BOOL isReference;
@end

@implementation FrameRow
@end

// ---- ファイルのドロップを受けるビュー --------------------------------------------------

@interface DropView : NSView
@property(nonatomic, copy) void (^onDrop)(NSArray<NSURL*>*);
@end

@implementation DropView
- (instancetype)initWithFrame:(NSRect)frame {
    self = [super initWithFrame:frame];
    if (self) [self registerForDraggedTypes:@[ NSPasteboardTypeFileURL ]];
    return self;
}
- (NSDragOperation)draggingEntered:(id<NSDraggingInfo>)sender {
    (void)sender;
    return NSDragOperationCopy;
}
- (BOOL)performDragOperation:(id<NSDraggingInfo>)sender {
    NSArray<NSURL*>* urls = [[sender draggingPasteboard] readObjectsForClasses:@[ [NSURL class] ]
                                                                        options:@{NSPasteboardURLReadingFileURLsOnlyKey : @YES}];
    if (urls.count && _onDrop) _onDrop(urls);
    return urls.count > 0;
}
@end

// ---- 補助 --------------------------------------------------------------------------------

namespace {

enum ViewMode : NSInteger { kViewMerged = 0, kViewOverlay = 1, kViewSourceMap = 2, kViewFrame = 3 };

// 由来マップの色（暗い→明るい の順）。
const unsigned char kPalette[8][3] = {{40, 40, 255}, {0, 160, 255}, {0, 220, 120}, {200, 220, 0},
                                      {255, 140, 0}, {255, 40, 40}, {255, 0, 200}, {255, 255, 255}};

NSString* ns(const std::string& s) {
    NSString* r = [NSString stringWithUTF8String:s.c_str()];
    return r ?: @"";
}

NSString* shutter_text(double t) {
    if (!(t > 0.0)) return @"–";
    if (t < 0.3) return [NSString stringWithFormat:@"1/%.0f", 1.0 / t];
    return [NSString stringWithFormat:@"%.1f秒", t];
}

NSImage* image_from_rgb(const hdr::Rgb8Image& img) {
    if (img.width <= 0 || img.height <= 0) return nil;
    NSBitmapImageRep* rep = [[NSBitmapImageRep alloc] initWithBitmapDataPlanes:nullptr
                                                                    pixelsWide:img.width
                                                                    pixelsHigh:img.height
                                                                 bitsPerSample:8
                                                               samplesPerPixel:3
                                                                      hasAlpha:NO
                                                                      isPlanar:NO
                                                                colorSpaceName:NSDeviceRGBColorSpace
                                                                   bytesPerRow:img.width * 3
                                                                  bitsPerPixel:24];
    std::memcpy([rep bitmapData], img.rgb.data(), img.rgb.size());
    // sRGB として扱う（プレビューは sRGB で作っている）。
    NSBitmapImageRep* srgb = [rep bitmapImageRepByRetaggingWithColorSpace:[NSColorSpace sRGBColorSpace]];
    NSImage* out = [[NSImage alloc] initWithSize:NSMakeSize(img.width, img.height)];
    [out addRepresentation:srgb ?: rep];
    return out;
}

// render_preview と同じ縮小の刻み（1 出力画素 = cell × cell の CFA 画素）。
int preview_cell(int width, int height, const hdr::CfaPattern& cfa, int max_size) {
    const int block = cfa.is_xtrans() ? 3 : 2;
    const int longest = std::max(width, height);
    const int step = std::max(1, static_cast<int>(std::ceil(static_cast<double>(longest) / block / std::max(16, max_size))));
    return step * block;
}

// 由来マップをプレビューと同じ寸法で作る。
hdr::Rgb8Image source_map(const hdr::MergeResult& m, int max_size) {
    hdr::Rgb8Image out;
    const int cell = preview_cell(m.width, m.height, m.cfa, max_size);
    const int step = cell / m.block;
    out.width = m.width / cell;
    out.height = m.height / cell;
    out.rgb.assign(static_cast<std::size_t>(out.width) * out.height * 3, 0);
    for (int oy = 0; oy < out.height; ++oy) {
        for (int ox = 0; ox < out.width; ++ox) {
            double acc[3] = {};
            int n = 0;
            for (int by = oy * step; by < (oy + 1) * step && by < m.grid_h; ++by) {
                for (int bx = ox * step; bx < (ox + 1) * step && bx < m.grid_w; ++bx) {
                    const std::size_t i = static_cast<std::size_t>(by) * m.grid_w + bx;
                    for (std::size_t o = 0; o < m.weights.size(); ++o) {
                        const float w = m.weights[o][i];
                        if (w == 0.0f) continue;
                        for (int c = 0; c < 3; ++c) acc[c] += w * kPalette[o % 8][c];
                    }
                    ++n;
                }
            }
            uint8_t* d = out.rgb.data() + (static_cast<std::size_t>(oy) * out.width + ox) * 3;
            for (int c = 0; c < 3; ++c) d[c] = static_cast<uint8_t>(std::lround(std::min(255.0, n ? acc[c] / n : 0.0)));
        }
    }
    return out;
}

struct Settings {
    hdr::MergeOptions merge;
    int reference = -1;  // 並べ替えた後のフレームの番号。-1 = 自動
};

const int kPreviewSize = 3200;

}  // namespace

// ---- 本体 --------------------------------------------------------------------------------

@interface MainWindowController () <NSTableViewDataSource, NSTableViewDelegate>
@end

@implementation MainWindowController {
    // 画面
    NSTableView* _table;
    PreviewView* _preview;
    NSSegmentedControl* _modeControl;
    NSSlider* _evSlider;
    NSTextField* _evLabel;
    NSPopUpButton* _refPopup;
    NSPopUpButton* _alignPopup;
    NSSlider* _rampSlider;
    NSSlider* _safetySlider;
    NSSlider* _featherSlider;
    NSTextField* _rampLabel;
    NSTextField* _safetyLabel;
    NSTextField* _featherLabel;
    NSButton* _lensXmpCheck;
    NSPopUpButton* _formatPopup;
    NSTextField* _formatNote;
    NSButton* _exportButton;
    NSTextView* _infoView;
    NSProgressIndicator* _progress;
    NSTextField* _status;
    NSArray<FrameRow*>* _rows;

    // エンジン（_queue の上でだけ触る）
    dispatch_queue_t _queue;
    std::vector<hdr::RawFrame> _frames;
    hdr::ExposurePlan _plan;
    hdr::MergeResult _merged;
    bool _hasMerged;
    hdr::FormatDecision _autoFormat;  // 合成のたびに求める自動の判定
    std::string _converter;           // Adobe DNG Converter の場所（無ければ空）
    hdr::DngTemplate _template;       // 基準フレームを変換したもの（_templateFor のフレームについて）
    std::string _templateFor;

    NSInteger _mergeGeneration;
    NSInteger _renderGeneration;
    NSInteger _busyCount;

    // 検証用
    NSString* _snapshotPath;
    NSString* _autoExportPath;
    BOOL _automationPending;
}

- (instancetype)init {
    NSWindow* window = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, 1320, 820)
                                                   styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                                                             NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable
                                                     backing:NSBackingStoreBuffered
                                                       defer:NO];
    self = [super initWithWindow:window];
    if (self) {
        _queue = dispatch_queue_create("io.github.geology-cat.rawbrackethdr.engine", DISPATCH_QUEUE_SERIAL);
        _hasMerged = false;
        _rows = @[];
        _converter = hdr::find_dng_converter();
        [window setTitle:@"RawBracketHDR"];
        [window setMinSize:NSMakeSize(1040, 660)];
        [window setFrameAutosaveName:@"MainWindow"];
        [window center];
        [self buildInterface];
        [self updateControls];
    }
    return self;
}

// ---- 画面の組み立て ----------------------------------------------------------------------

- (NSTextField*)sectionLabel:(NSString*)text {
    NSTextField* l = [NSTextField labelWithString:text];
    [l setFont:[NSFont boldSystemFontOfSize:12]];
    return l;
}

- (NSStackView*)sliderRow:(NSString*)title
                   slider:(NSSlider* __strong*)slider
                    label:(NSTextField* __strong*)valueLabel
                      min:(double)min
                      max:(double)max
                    value:(double)value
                  tooltip:(NSString*)tooltip {
    NSTextField* t = [NSTextField labelWithString:title];
    [t setToolTip:tooltip];
    NSSlider* s = [NSSlider sliderWithValue:value minValue:min maxValue:max target:self action:@selector(settingChanged:)];
    [s setContinuous:YES];
    [s setToolTip:tooltip];
    NSTextField* v = [NSTextField labelWithString:@""];
    [v setFont:[NSFont monospacedDigitSystemFontOfSize:11 weight:NSFontWeightRegular]];
    [v setAlignment:NSTextAlignmentRight];
    [[v widthAnchor] constraintEqualToConstant:52].active = YES;
    NSStackView* row = [NSStackView stackViewWithViews:@[ s, v ]];
    [row setSpacing:6];
    NSStackView* col = [NSStackView stackViewWithViews:@[ t, row ]];
    [col setOrientation:NSUserInterfaceLayoutOrientationVertical];
    [col setAlignment:NSLayoutAttributeLeading];
    [col setSpacing:2];
    [[row widthAnchor] constraintEqualToAnchor:[col widthAnchor]].active = YES;
    *slider = s;
    *valueLabel = v;
    return col;
}

- (void)buildInterface {
    DropView* root = [[DropView alloc] initWithFrame:NSMakeRect(0, 0, 1320, 820)];
    __weak MainWindowController* weakSelf = self;
    root.onDrop = ^(NSArray<NSURL*>* urls) {
        [weakSelf addFiles:urls];
    };
    [[self window] setContentView:root];

    // ---- 左: フレームの一覧 ----
    _table = [[NSTableView alloc] initWithFrame:NSZeroRect];
    struct Col {
        NSString* ident;
        NSString* title;
        CGFloat width;
    };
    const Col cols[] = {{@"number", @"#", 22}, {@"name", @"ファイル", 112}, {@"shutter", @"シャッター", 58},
                        {@"ev", @"相対EV", 50}, {@"clip", @"飽和", 42}};
    for (const Col& c : cols) {
        NSTableColumn* col = [[NSTableColumn alloc] initWithIdentifier:c.ident];
        [[col headerCell] setStringValue:c.title];
        [col setWidth:c.width];
        [col setMinWidth:24];
        [_table addTableColumn:col];
    }
    [_table setDataSource:self];
    [_table setDelegate:self];
    [_table setUsesAlternatingRowBackgroundColors:YES];
    [_table setAllowsMultipleSelection:YES];
    [_table setColumnAutoresizingStyle:NSTableViewNoColumnAutoresizing];
    // macOS 11 以降の既定（inset）は左右に余白を足して列が収まらなくなるので、10.13 と同じ見た目にする。
    if (@available(macOS 11.0, *)) [_table setStyle:NSTableViewStylePlain];
    NSScrollView* tableScroll = [[NSScrollView alloc] initWithFrame:NSZeroRect];
    [tableScroll setDocumentView:_table];
    [tableScroll setHasVerticalScroller:YES];
    [tableScroll setBorderType:NSBezelBorder];

    NSButton* addButton = [NSButton buttonWithTitle:@"追加…" target:self action:@selector(openDocument:)];
    NSButton* removeButton = [NSButton buttonWithTitle:@"取り除く" target:self action:@selector(removeSelected:)];
    NSButton* clearButton = [NSButton buttonWithTitle:@"すべて取り除く" target:self action:@selector(clearFrames:)];
    NSStackView* listButtons = [NSStackView stackViewWithViews:@[ addButton, removeButton, clearButton ]];
    [listButtons setSpacing:6];

    NSTextField* hint = [NSTextField wrappingLabelWithString:@"同じ場面を露出を変えて撮った RAW（AEB など）をここへドロップしてください。"];
    [hint setFont:[NSFont systemFontOfSize:11]];
    [hint setTextColor:[NSColor secondaryLabelColor]];

    NSStackView* left = [NSStackView stackViewWithViews:@[ [self sectionLabel:@"フレーム（暗い順）"], tableScroll, listButtons, hint ]];
    [left setOrientation:NSUserInterfaceLayoutOrientationVertical];
    [left setAlignment:NSLayoutAttributeLeading];
    [left setSpacing:8];
    [left setEdgeInsets:NSEdgeInsetsMake(12, 12, 12, 6)];
    [[tableScroll widthAnchor] constraintEqualToAnchor:[left widthAnchor] constant:-18].active = YES;
    [[hint widthAnchor] constraintEqualToAnchor:[tableScroll widthAnchor]].active = YES;
    [tableScroll setContentHuggingPriority:1 forOrientation:NSLayoutConstraintOrientationVertical];

    // ---- 中央: プレビュー ----
    _modeControl = [NSSegmentedControl segmentedControlWithLabels:@[ @"合成結果", @"重ねて表示", @"由来マップ", @"選んだフレーム" ]
                                                     trackingMode:NSSegmentSwitchTrackingSelectOne
                                                           target:self
                                                           action:@selector(viewModeChanged:)];
    [_modeControl setSelectedSegment:kViewMerged];
    [_modeControl setToolTip:@"由来マップ: どのフレームを使ったかを色で表示（色はフレーム一覧の番号の色）"];
    _evSlider = [NSSlider sliderWithValue:0 minValue:-6 maxValue:6 target:self action:@selector(viewModeChanged:)];
    [_evSlider setContinuous:NO];
    [_evSlider setToolTip:@"プレビューの明るさ（段）。書き出す DNG には影響しません"];
    [[_evSlider widthAnchor] constraintEqualToConstant:180].active = YES;
    _evLabel = [NSTextField labelWithString:@"表示 ±0.0 EV"];
    [_evLabel setFont:[NSFont monospacedDigitSystemFontOfSize:11 weight:NSFontWeightRegular]];
    NSButton* fitButton = [NSButton buttonWithTitle:@"全体" target:self action:@selector(zoomToFit:)];
    NSButton* actualButton = [NSButton buttonWithTitle:@"100%" target:self action:@selector(zoomToActual:)];
    NSStackView* viewBar = [NSStackView stackViewWithViews:@[ _modeControl, _evSlider, _evLabel, fitButton, actualButton ]];
    [viewBar setSpacing:10];

    _preview = [[PreviewView alloc] initWithFrame:NSMakeRect(0, 0, 600, 500)];
    [_preview setPlaceholder:@"RAW をここへドロップ（2枚以上）"];

    NSStackView* center = [NSStackView stackViewWithViews:@[ viewBar, _preview ]];
    [center setOrientation:NSUserInterfaceLayoutOrientationVertical];
    [center setAlignment:NSLayoutAttributeLeading];
    [center setSpacing:8];
    [center setEdgeInsets:NSEdgeInsetsMake(12, 6, 12, 6)];
    [[_preview widthAnchor] constraintEqualToAnchor:[center widthAnchor] constant:-12].active = YES;
    [_preview setContentHuggingPriority:1 forOrientation:NSLayoutConstraintOrientationVertical];
    [_preview setContentHuggingPriority:1 forOrientation:NSLayoutConstraintOrientationHorizontal];

    // ---- 右: 設定と書き出し ----
    _refPopup = [[NSPopUpButton alloc] initWithFrame:NSZeroRect pullsDown:NO];
    [_refPopup setTarget:self];
    [_refPopup setAction:@selector(settingChanged:)];
    [_refPopup setToolTip:@"DNG はこのフレームと同じ明るさ・メタデータ（撮影情報・レンズ）で開きます"];
    _alignPopup = [[NSPopUpButton alloc] initWithFrame:NSZeroRect pullsDown:NO];
    [_alignPopup addItemsWithTitles:@[ @"しない（三脚）", @"自動（今後対応）", @"手動（今後対応）" ]];
    [[_alignPopup itemAtIndex:1] setEnabled:NO];
    [[_alignPopup itemAtIndex:2] setEnabled:NO];
    [_alignPopup setAutoenablesItems:NO];

    const hdr::MergeOptions defaults;
    NSStackView* rampRow = [self sliderRow:@"切り替えを始める明るさ"
                                    slider:&_rampSlider
                                     label:&_rampLabel
                                       min:0.3
                                       max:0.9
                                     value:defaults.ramp_start
                                   tooltip:@"明るいフレームの値が飽和の閾値のこの割合を超えたら、暗いフレームへ滑らかに切り替え始めます。"
                                           @"小さいほど切り替わりが緩やかになりますが、暗い（ノイズの多い）フレームを使う範囲が広がります"];
    NSStackView* safetyRow = [self sliderRow:@"飽和とみなす閾値"
                                      slider:&_safetySlider
                                       label:&_safetyLabel
                                         min:0.80
                                         max:0.99
                                       value:defaults.safety
                                     tooltip:@"飽和レベルに対する割合。これを超えた画素（とその隣）は使いません。"
                                             @"飽和の手前はセンサーの応答がまっすぐでないことがあるので、少し余裕をとります"];
    NSStackView* featherRow = [self sliderRow:@"重みのぼかし"
                                       slider:&_featherSlider
                                        label:&_featherLabel
                                          min:4
                                          max:64
                                        value:defaults.feather_px
                                      tooltip:@"重みのちらつきを抑えるぼかしの幅（画素）"];
    _lensXmpCheck = [NSButton checkboxWithTitle:@"レンズ補正を有効にして開く" target:nil action:nil];
    [_lensXmpCheck setState:NSControlStateValueOff];
    [_lensXmpCheck setToolTip:@"DNG の XMP にレンズプロファイル補正の設定を入れます。"
                              @"入れると Camera Raw はユーザーの既定の現像設定（プロファイルなど）を使わなくなるので、既定ではオフです。"
                              @"オフでもレンズは EXIF から認識され、Lightroom でプロファイル補正を選べます"];

    _formatPopup = [[NSPopUpButton alloc] initWithFrame:NSZeroRect pullsDown:NO];
    [_formatPopup addItemsWithTitles:@[ @"自動（おすすめ）", @"CFA（色補間前）", @"LinearRaw（色補間済み）" ]];
    [_formatPopup setTarget:self];
    [_formatPopup setAction:@selector(formatChanged:)];
    [_formatPopup setToolTip:@"CFA: Lightroom・Camera Raw が色補間する。ただし Camera Raw は白から下およそ16段までしか"
                             @"階調を扱えないので、明暗差が大きいと暗部に段差が出る。\n"
                             @"LinearRaw: このアプリが色補間する（RCD）。Camera Raw は浮動小数点のまま処理するので明暗差の制限がない。\n"
                             @"自動: 暗部のノイズと Camera Raw の刻みを比べて、段差が見えないなら CFA、見えるなら LinearRaw"];
    _formatNote = [NSTextField wrappingLabelWithString:@""];
    [_formatNote setFont:[NSFont systemFontOfSize:11]];
    [_formatNote setTextColor:[NSColor secondaryLabelColor]];

    _exportButton = [NSButton buttonWithTitle:@"DNG を書き出す…" target:self action:@selector(exportDNG:)];
    [_exportButton setKeyEquivalent:@"\r"];
    [_exportButton setControlSize:NSControlSizeRegular];  // Large は macOS 11 以降

    _infoView = [[NSTextView alloc] initWithFrame:NSMakeRect(0, 0, 280, 200)];
    [_infoView setEditable:NO];
    [_infoView setRichText:NO];
    [_infoView setFont:[NSFont fontWithName:@"Menlo" size:10.5] ?: [NSFont userFixedPitchFontOfSize:10.5]];
    [_infoView setTextContainerInset:NSMakeSize(4, 4)];
    [_infoView setHorizontallyResizable:NO];
    [[_infoView textContainer] setWidthTracksTextView:YES];
    NSScrollView* infoScroll = [[NSScrollView alloc] initWithFrame:NSZeroRect];
    [infoScroll setDocumentView:_infoView];
    [infoScroll setHasVerticalScroller:YES];
    [infoScroll setBorderType:NSBezelBorder];
    [_infoView setAutoresizingMask:NSViewWidthSizable];

    NSStackView* right = [NSStackView stackViewWithViews:@[
        [self sectionLabel:@"基準フレーム"], _refPopup,
        [self sectionLabel:@"位置合わせ"], _alignPopup,
        [self sectionLabel:@"合成"], rampRow, safetyRow, featherRow,
        [self sectionLabel:@"書き出し"], _formatPopup, _formatNote, _lensXmpCheck, _exportButton,
        [self sectionLabel:@"解析の結果"], infoScroll
    ]];
    [right setOrientation:NSUserInterfaceLayoutOrientationVertical];
    [right setAlignment:NSLayoutAttributeLeading];
    [right setSpacing:8];
    [right setEdgeInsets:NSEdgeInsetsMake(12, 6, 12, 12)];
    for (NSView* v in @[ _refPopup, _alignPopup, rampRow, safetyRow, featherRow, _formatPopup, _formatNote, _exportButton, infoScroll ]) {
        [[v widthAnchor] constraintEqualToAnchor:[right widthAnchor] constant:-18].active = YES;
    }
    [right setCustomSpacing:14 afterView:featherRow];
    [right setCustomSpacing:14 afterView:_alignPopup];
    [right setCustomSpacing:14 afterView:_exportButton];
    [infoScroll setContentHuggingPriority:1 forOrientation:NSLayoutConstraintOrientationVertical];

    // ---- 下: 状態 ----
    _progress = [[NSProgressIndicator alloc] initWithFrame:NSZeroRect];
    [_progress setStyle:NSProgressIndicatorStyleSpinning];
    [_progress setControlSize:NSControlSizeSmall];
    [_progress setDisplayedWhenStopped:NO];
    _status = [NSTextField labelWithString:@"RAW を開いてください"];
    [_status setLineBreakMode:NSLineBreakByTruncatingTail];
    [_status setContentCompressionResistancePriority:1 forOrientation:NSLayoutConstraintOrientationHorizontal];
    NSStackView* statusBar = [NSStackView stackViewWithViews:@[ _progress, _status ]];
    [statusBar setSpacing:8];
    [statusBar setEdgeInsets:NSEdgeInsetsMake(4, 12, 6, 12)];

    for (NSView* v in @[ left, center, right, statusBar ]) {
        [v setTranslatesAutoresizingMaskIntoConstraints:NO];
        [root addSubview:v];
    }
    [NSLayoutConstraint activateConstraints:@[
        [[left leadingAnchor] constraintEqualToAnchor:[root leadingAnchor]],
        [[left topAnchor] constraintEqualToAnchor:[root topAnchor]],
        [[left bottomAnchor] constraintEqualToAnchor:[statusBar topAnchor]],
        [[left widthAnchor] constraintEqualToConstant:390],
        [[center leadingAnchor] constraintEqualToAnchor:[left trailingAnchor]],
        [[center topAnchor] constraintEqualToAnchor:[root topAnchor]],
        [[center bottomAnchor] constraintEqualToAnchor:[statusBar topAnchor]],
        [[right leadingAnchor] constraintEqualToAnchor:[center trailingAnchor]],
        [[right trailingAnchor] constraintEqualToAnchor:[root trailingAnchor]],
        [[right topAnchor] constraintEqualToAnchor:[root topAnchor]],
        [[right bottomAnchor] constraintEqualToAnchor:[statusBar topAnchor]],
        [[right widthAnchor] constraintEqualToConstant:300],
        [[statusBar leadingAnchor] constraintEqualToAnchor:[root leadingAnchor]],
        [[statusBar trailingAnchor] constraintEqualToAnchor:[root trailingAnchor]],
        [[statusBar bottomAnchor] constraintEqualToAnchor:[root bottomAnchor]],
        [[statusBar heightAnchor] constraintEqualToConstant:28],
    ]];
    [self refreshSettingLabels];
}

// ---- 状態の表示 --------------------------------------------------------------------------

- (void)beginBusy:(NSString*)message {
    ++_busyCount;
    [_progress startAnimation:nil];
    [_status setStringValue:message];
    [self updateControls];
}

- (void)endBusy:(NSString*)message {
    _busyCount = MAX(0, _busyCount - 1);
    if (_busyCount == 0) [_progress stopAnimation:nil];
    if (message) [_status setStringValue:message];
    [self updateControls];
}

- (void)updateControls {
    const BOOL ready = _rows.count >= 2;
    [_exportButton setEnabled:ready && _busyCount == 0];
}

- (void)refreshSettingLabels {
    [_rampLabel setStringValue:[NSString stringWithFormat:@"%.2f", [_rampSlider doubleValue]]];
    [_safetyLabel setStringValue:[NSString stringWithFormat:@"%.2f", [_safetySlider doubleValue]]];
    [_featherLabel setStringValue:[NSString stringWithFormat:@"%.0f px", [_featherSlider doubleValue]]];
    const double ev = std::round([_evSlider doubleValue] * 2.0) / 2.0;
    [_evLabel setStringValue:[NSString stringWithFormat:@"表示 %@%.1f EV", ev > 0 ? @"+" : (ev < 0 ? @"" : @"±"), ev]];
}

- (Settings)currentSettings {
    Settings s;
    s.merge.ramp_start = [_rampSlider doubleValue];
    s.merge.safety = [_safetySlider doubleValue];
    s.merge.feather_px = static_cast<int>(std::lround([_featherSlider doubleValue]));
    s.reference = static_cast<int>([_refPopup indexOfSelectedItem]) - 1;  // 先頭は「自動」
    return s;
}

- (void)showError:(NSString*)message {
    NSAlert* alert = [[NSAlert alloc] init];
    [alert setMessageText:@"処理できませんでした"];
    [alert setInformativeText:message];
    [alert setAlertStyle:NSAlertStyleWarning];
    if ([self window].isVisible) {
        [alert beginSheetModalForWindow:[self window] completionHandler:nil];
    } else {
        [alert runModal];
    }
}

// ---- ファイル -----------------------------------------------------------------------------

- (NSArray<NSURL*>*)expandFiles:(NSArray<NSURL*>*)urls {
    NSMutableArray<NSURL*>* out = [NSMutableArray array];
    NSFileManager* fm = [NSFileManager defaultManager];
    for (NSURL* url in urls) {
        NSNumber* isDir = nil;
        [url getResourceValue:&isDir forKey:NSURLIsDirectoryKey error:nil];
        if ([isDir boolValue]) {
            NSArray<NSURL*>* items = [fm contentsOfDirectoryAtURL:url
                                       includingPropertiesForKeys:nil
                                                          options:NSDirectoryEnumerationSkipsHiddenFiles
                                                            error:nil];
            items = [items sortedArrayUsingComparator:^NSComparisonResult(NSURL* a, NSURL* b) {
                return [[a lastPathComponent] localizedStandardCompare:[b lastPathComponent]];
            }];
            static NSSet* skip = [NSSet setWithArray:@[ @"jpg", @"jpeg", @"xmp", @"tif", @"tiff", @"png", @"heic", @"mov", @"mp4" ]];
            for (NSURL* item in items) {
                if (![skip containsObject:[[item pathExtension] lowercaseString]]) [out addObject:item];
            }
        } else {
            [out addObject:url];
        }
    }
    return out;
}

- (void)addFiles:(NSArray<NSURL*>*)urls {
    NSArray<NSURL*>* files = [self expandFiles:urls];
    if (!files.count) return;
    std::vector<std::string> paths;
    for (NSURL* u in files) paths.push_back([[u path] fileSystemRepresentation]);
    [self beginBusy:[NSString stringWithFormat:@"%lu 枚を読み込み中…", (unsigned long)files.count]];
    const Settings settings = [self currentSettings];
    dispatch_async(_queue, ^{
        NSMutableArray<NSString*>* errors = [NSMutableArray array];
        for (const std::string& p : paths) {
            bool dup = false;
            for (const hdr::RawFrame& f : self->_frames) dup |= f.path == p;
            if (dup) continue;
            try {
                self->_frames.push_back(hdr::load_raw_frame(p, true));
            } catch (const std::exception& e) {
                [errors addObject:ns(e.what())];
            }
        }
        [self analyzeOnQueueWithSettings:settings resetReference:YES];
        dispatch_async(dispatch_get_main_queue(), ^{
            [self endBusy:nil];
            if (errors.count) [self showError:[errors componentsJoinedByString:@"\n"]];
        });
    });
}

- (void)openDocument:(id)sender {
    (void)sender;
    NSOpenPanel* panel = [NSOpenPanel openPanel];
    [panel setAllowsMultipleSelection:YES];
    [panel setCanChooseDirectories:YES];
    [panel setMessage:@"露出を変えて撮った RAW を選んでください（フォルダも選べます）"];
    [panel beginSheetModalForWindow:[self window]
                  completionHandler:^(NSModalResponse result) {
                      if (result == NSModalResponseOK) [self addFiles:[panel URLs]];
                  }];
}

- (void)removeSelected:(id)sender {
    (void)sender;
    NSIndexSet* sel = [_table selectedRowIndexes];
    if (!sel.count) return;
    __block std::vector<int> remove;
    [sel enumerateIndexesUsingBlock:^(NSUInteger idx, BOOL* stop) {
        (void)stop;
        remove.push_back(static_cast<int>(idx));
    }];
    const Settings settings = [self currentSettings];
    [self beginBusy:@"取り除いています…"];
    dispatch_async(_queue, ^{
        std::vector<int> rm = remove;
        std::sort(rm.rbegin(), rm.rend());
        for (int i : rm) {
            if (i >= 0 && i < static_cast<int>(self->_frames.size())) self->_frames.erase(self->_frames.begin() + i);
        }
        [self analyzeOnQueueWithSettings:settings resetReference:YES];
        dispatch_async(dispatch_get_main_queue(), ^{
            [self endBusy:nil];
        });
    });
}

- (void)clearFrames:(id)sender {
    (void)sender;
    const Settings settings = [self currentSettings];
    dispatch_async(_queue, ^{
        self->_frames.clear();
        [self analyzeOnQueueWithSettings:settings resetReference:YES];
    });
}

// ---- 解析と合成（_queue の上） ------------------------------------------------------------

// フレームを並べ替え、露出比を推定し、合成し、画面を更新する。
- (void)analyzeOnQueueWithSettings:(Settings)settings resetReference:(BOOL)resetReference {
    std::stable_sort(_frames.begin(), _frames.end(),
                     [](const hdr::RawFrame& a, const hdr::RawFrame& b) { return a.nominal_ev() < b.nominal_ev(); });
    if (resetReference) settings.reference = -1;
    _hasMerged = false;
    NSString* problem = nil;
    if (_frames.size() >= 2) {
        const hdr::RawFrame& f0 = _frames[0];
        for (const hdr::RawFrame& f : _frames) {
            if (f.width != f0.width || f.height != f0.height) problem = @"画像の寸法が揃っていません（別のカメラ・設定の RAW が混ざっています）";
            else if (f.unique_camera_model != f0.unique_camera_model) problem = @"別のカメラの RAW が混ざっています";
        }
        if (!problem) {
            try {
                _plan = hdr::estimate_exposures(_frames);
            } catch (const std::exception& e) {
                problem = ns(e.what());
            }
        }
    }
    NSArray<FrameRow*>* rows = [self rowsOnQueue];
    NSArray<NSString*>* refTitles = [self referenceTitlesOnQueue];
    NSString* info = problem ?: [self infoTextOnQueue];
    dispatch_async(dispatch_get_main_queue(), ^{
        self->_rows = rows;
        [self->_table reloadData];
        const NSInteger keep = resetReference ? 0 : [self->_refPopup indexOfSelectedItem];
        [self->_refPopup removeAllItems];
        [self->_refPopup addItemsWithTitles:refTitles];
        if (keep >= 0 && keep < (NSInteger)refTitles.count) [self->_refPopup selectItemAtIndex:keep];
        [self->_infoView setString:info ?: @""];
        [self updateControls];
        if (problem) {
            [self->_status setStringValue:problem];
            [self->_preview setImage:nil keepZoom:NO];
            [self->_preview setPlaceholder:problem];
        } else if (rows.count < 2) {
            [self->_preview setImage:nil keepZoom:NO];
            [self->_preview setPlaceholder:rows.count ? @"もう1枚以上、露出の違う RAW を加えてください" : @"RAW をここへドロップ（2枚以上）"];
            [self->_status setStringValue:rows.count ? @"2枚以上の RAW が要ります" : @"RAW を開いてください"];
        } else {
            [self scheduleMerge:NO];
        }
    });
}

- (NSArray<FrameRow*>*)rowsOnQueue {
    NSMutableArray<FrameRow*>* rows = [NSMutableArray array];
    const bool planned = _plan.order.size() == _frames.size() && _frames.size() >= 2;
    const int ref = _hasMerged ? _merged.reference : -1;
    for (std::size_t i = 0; i < _frames.size(); ++i) {
        const hdr::RawFrame& f = _frames[i];
        FrameRow* r = [[FrameRow alloc] init];
        r.number = [NSString stringWithFormat:@"%zu", i + 1];
        r.name = ns(f.file_name);
        r.shutter = shutter_text(f.exposure_time);
        r.relativeEV = @"–";
        r.clip = @"–";
        r.isReference = static_cast<int>(i) == ref;
        if (planned) {
            for (std::size_t o = 0; o < _plan.order.size(); ++o) {
                if (_plan.order[o] == static_cast<int>(i)) r.relativeEV = [NSString stringWithFormat:@"%+.2f", std::log2(_plan.rel_exposure[o])];
            }
            r.clip = [NSString stringWithFormat:@"%.0f", _plan.clip[i].level[1]];
        }
        r.tooltip = [NSString stringWithFormat:@"%@\n%@  F%.1f  ISO %.0f\n%@\n%@", ns(f.file_name), shutter_text(f.exposure_time), f.fnumber,
                                               f.iso, ns(f.model), ns(f.lens_model)];
        [rows addObject:r];
    }
    return rows;
}

- (NSArray<NSString*>*)referenceTitlesOnQueue {
    NSMutableArray<NSString*>* titles = [NSMutableArray arrayWithObject:@"自動（露光量が中央のもの）"];
    for (std::size_t i = 0; i < _frames.size(); ++i) {
        [titles addObject:[NSString stringWithFormat:@"%zu: %@（%@）", i + 1, ns(_frames[i].file_name), shutter_text(_frames[i].exposure_time)]];
    }
    return titles;
}

- (NSString*)infoTextOnQueue {
    if (_frames.size() < 2 || _plan.order.size() != _frames.size()) return @"";
    NSMutableString* s = [NSMutableString string];
    const hdr::RawFrame& f0 = _frames[0];
    [s appendFormat:@"%@\n%@\n", ns(f0.unique_camera_model), ns(f0.lens_model)];
    [s appendString:_converter.empty() ? @"Adobe DNG Converter: なし\n（色と明るさは LibRaw の値。Camera Raw で\n CR2 を開いたときと少し違います）\n\n"
                                       : @"Adobe DNG Converter: あり\n（色と明るさを Camera Raw に揃えます）\n\n"];
    [s appendString:@"露出比（隣どうし、実測）\n"];
    for (const hdr::PairFit& f : _plan.fits) {
        [s appendFormat:@" %d→%d  ×%.3f  (%+.3f EV)%@\n", f.dark + 1, f.bright + 1, f.ratio, std::log2(f.ratio / f.nominal_ratio),
                        f.measured ? @"" : @" 名目値"];
    }
    [s appendString:@"  ※(差) = EXIF の名目値とのずれ\n"];
    if (_hasMerged) {
        [s appendFormat:@"\n基準: %d（最も暗いフレームより %+.2f 段明るい）\n", _merged.reference + 1, std::log2(_merged.reference_rel_exposure)];
        [s appendFormat:@"最も暗いフレームでも飽和: %.3f%%\n", _merged.clipped_fraction * 100.0];
        [s appendString:@"\n使った割合（面積）\n"];
        for (std::size_t o = 0; o < _merged.weights.size(); ++o) {
            double sum = 0.0;
            for (float w : _merged.weights[o]) sum += w;
            const double pct = 100.0 * sum / std::max<std::size_t>(1, _merged.weights[o].size());
            if (pct > 0.0 && pct < 0.1) {
                [s appendFormat:@" %d:  <0.1%%\n", _plan.order[o] + 1];
            } else {
                [s appendFormat:@" %d: %5.1f%%\n", _plan.order[o] + 1, pct];
            }
        }
    }
    return s;
}

// 設定が変わったら少し待ってから合成し直す（スライダーを動かしている間に何度も走らせない）。
- (void)scheduleMerge:(BOOL)debounce {
    [NSObject cancelPreviousPerformRequestsWithTarget:self selector:@selector(runMerge) object:nil];
    if (debounce) {
        [self performSelector:@selector(runMerge) withObject:nil afterDelay:0.35];
    } else {
        [self runMerge];
    }
}

- (void)runMerge {
    const NSInteger gen = ++_mergeGeneration;
    const Settings settings = [self currentSettings];
    [self beginBusy:@"合成中…"];
    dispatch_async(_queue, ^{
        if (gen != self->_mergeGeneration || self->_frames.size() < 2 || self->_plan.order.size() != self->_frames.size()) {
            dispatch_async(dispatch_get_main_queue(), ^{
                [self endBusy:nil];
            });
            return;
        }
        NSString* message = nil;
        try {
            hdr::MergeOptions mo = settings.merge;
            mo.reference = settings.reference;
            self->_merged = hdr::merge_frames(self->_frames, self->_plan, mo);
            self->_hasMerged = true;
            self->_autoFormat = hdr::decide_output_format(self->_merged, hdr::OutputFormat::Auto);
            message = [NSString stringWithFormat:@"%zu 枚を合成しました。基準: %@", self->_frames.size(),
                                                 ns(self->_frames[self->_merged.reference].file_name)];
        } catch (const std::exception& e) {
            self->_hasMerged = false;
            message = ns(e.what());
        }
        NSArray<FrameRow*>* rows = [self rowsOnQueue];
        NSString* info = [self infoTextOnQueue];
        dispatch_async(dispatch_get_main_queue(), ^{
            self->_rows = rows;
            [self->_table reloadData];
            [self->_infoView setString:info];
            [self endBusy:message];
            [self updateFormatNote];
            [self renderPreview];
        });
    });
}

- (void)renderPreview {
    const NSInteger gen = ++_renderGeneration;
    const NSInteger mode = [_modeControl selectedSegment];
    const double ev = std::round([_evSlider doubleValue] * 2.0) / 2.0;
    const NSInteger selected = [_table selectedRow];
    dispatch_async(_queue, ^{
        if (gen != self->_renderGeneration || !self->_hasMerged) return;
        const hdr::MergeResult& m = self->_merged;
        const hdr::RawFrame& ref = self->_frames[m.reference];
        hdr::PreviewOptions po;
        po.max_size = kPreviewSize;
        hdr::Rgb8Image img;
        if (mode == kViewFrame) {
            const int i = selected >= 0 && selected < static_cast<NSInteger>(self->_frames.size()) ? static_cast<int>(selected) : m.reference;
            po.exposure_ev = ev;
            img = hdr::render_frame_preview(self->_frames[i], po);
        } else if (mode == kViewSourceMap) {
            img = source_map(m, kPreviewSize);
        } else {
            po.exposure_ev = m.reference_ev_offset + ev;
            img = hdr::render_preview(m.data.data(), m.width, m.height, m.cfa, ref.color_matrix, ref.as_shot_neutral, po);
            if (mode == kViewOverlay) {
                const hdr::Rgb8Image map = source_map(m, kPreviewSize);
                if (map.rgb.size() == img.rgb.size()) {
                    for (std::size_t i = 0; i < img.rgb.size(); ++i) img.rgb[i] = static_cast<uint8_t>(0.6 * img.rgb[i] + 0.4 * map.rgb[i]);
                }
            }
        }
        img = hdr::apply_orientation(img, ref.orientation);
        NSImage* image = image_from_rgb(img);
        dispatch_async(dispatch_get_main_queue(), ^{
            if (gen != self->_renderGeneration) return;
            [self->_preview setImage:image keepZoom:YES];
            [self finishAutomationIfNeeded];
        });
    });
}

// ---- 操作 ----------------------------------------------------------------------------------

- (void)settingChanged:(id)sender {
    [self refreshSettingLabels];
    if (sender == _refPopup) {
        [self scheduleMerge:NO];
    } else {
        [self scheduleMerge:YES];
    }
}

- (void)viewModeChanged:(id)sender {
    (void)sender;
    [self refreshSettingLabels];
    [self renderPreview];
}

- (void)zoomToFit:(id)sender {
    (void)sender;
    [_preview zoomToFit];
}

- (void)zoomToActual:(id)sender {
    (void)sender;
    [_preview zoomToActual];
}

- (void)zoomIn:(id)sender {
    (void)sender;
    [_preview zoomBy:1.5];
}

- (void)zoomOut:(id)sender {
    (void)sender;
    [_preview zoomBy:1.0 / 1.5];
}

- (BOOL)validateMenuItem:(NSMenuItem*)item {
    if ([item action] == @selector(exportDNG:)) return _rows.count >= 2 && _busyCount == 0;
    if ([item action] == @selector(clearFrames:)) return _rows.count > 0;
    return YES;
}

- (void)exportDNG:(id)sender {
    (void)sender;
    if (_rows.count < 2) return;
    // 既定の名前と場所は基準フレームから（キューの上で調べる）。
    __block NSString* defaultDir = nil;
    __block NSString* defaultName = nil;
    dispatch_sync(_queue, ^{
        if (!self->_hasMerged) return;
        const hdr::RawFrame& ref = self->_frames[self->_merged.reference];
        NSString* path = ns(ref.path);
        defaultDir = [path stringByDeletingLastPathComponent];
        defaultName = [[[path lastPathComponent] stringByDeletingPathExtension] stringByAppendingString:@"_HDR.dng"];
    });
    if (!defaultName) return;
    NSSavePanel* panel = [NSSavePanel savePanel];
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    [panel setAllowedFileTypes:@[ @"dng" ]];  // allowedContentTypes は macOS 11 以降
#pragma clang diagnostic pop
    [panel setDirectoryURL:[NSURL fileURLWithPath:defaultDir]];
    [panel setNameFieldStringValue:defaultName];
    [panel beginSheetModalForWindow:[self window]
                  completionHandler:^(NSModalResponse result) {
                      if (result == NSModalResponseOK) [self writeDNGToPath:[[panel URL] path]];
                  }];
}

- (hdr::OutputFormat)requestedFormat {
    switch ([_formatPopup indexOfSelectedItem]) {
        case 1: return hdr::OutputFormat::Cfa;
        case 2: return hdr::OutputFormat::LinearRaw;
        default: return hdr::OutputFormat::Auto;
    }
}

- (void)formatChanged:(id)sender {
    (void)sender;
    [self updateFormatNote];
}

// 自動の判定の結果を書き出しの欄の下に出す。
- (void)updateFormatNote {
    const hdr::OutputFormat req = [self requestedFormat];
    __block hdr::FormatDecision d;
    __block bool has = false;
    dispatch_sync(_queue, ^{
        has = self->_hasMerged;
        if (has) d = self->_autoFormat;
    });
    if (!has) {
        [_formatNote setStringValue:@""];
        return;
    }
    NSString* autoText = d.chosen == hdr::OutputFormat::Cfa ? @"CFA" : @"LinearRaw";
    if (req == hdr::OutputFormat::Auto) {
        [_formatNote setStringValue:[NSString stringWithFormat:@"→ %@（暗部のノイズが Camera Raw の刻みの %.2f 倍）", autoText, d.noise_to_step]];
    } else if (req == hdr::OutputFormat::Cfa && d.chosen == hdr::OutputFormat::LinearRaw) {
        [_formatNote setStringValue:[NSString stringWithFormat:@"注意: 明暗差が大きく、CFA では暗部を持ち上げると段差が出ます（自動なら LinearRaw）"]];
    } else {
        [_formatNote setStringValue:[NSString stringWithFormat:@"自動なら %@", autoText]];
    }
}

- (void)writeDNGToPath:(NSString*)path {
    const std::string out = [path fileSystemRepresentation];
    hdr::DngWriteOptions opt;
    opt.enable_lens_profile = [_lensXmpCheck state] == NSControlStateValueOn;
    NSString* version = [[NSBundle mainBundle] objectForInfoDictionaryKey:@"CFBundleShortVersionString"];
    opt.software = std::string("RawBracketHDR ") + (version ? [version UTF8String] : "");
    const hdr::OutputFormat requested = [self requestedFormat];
    [self beginBusy:@"DNG を書き出し中…"];
    dispatch_async(_queue, ^{
        NSString* error = nil;
        NSString* summary = @"";
        hdr::DngWriteOptions o = opt;  // ブロックに取り込んだ値は書き換えられないので写す
        try {
            if (!self->_hasMerged) throw std::runtime_error("合成の結果がありません");
            const hdr::FormatDecision fd = hdr::decide_output_format(self->_merged, requested);
            o.linear_raw = fd.chosen == hdr::OutputFormat::LinearRaw;
            // Adobe DNG Converter があれば基準フレームを変換して、色・明るさを Camera Raw に揃える。
            const hdr::RawFrame& ref = self->_frames[self->_merged.reference];
            if (!self->_converter.empty()) {
                if (self->_templateFor != ref.path) {
                    dispatch_async(dispatch_get_main_queue(), ^{
                        [self->_status setStringValue:@"Adobe DNG Converter で基準フレームを変換中…"];
                    });
                    self->_template = hdr::make_dng_template(ref.path, self->_converter);
                    self->_templateFor = ref.path;
                }
                o.adobe_template = &self->_template;
            }
            hdr::write_dng(out, self->_merged, self->_frames, self->_plan, o);
            const bool adobe = o.adobe_template && o.adobe_template->valid;
            summary = [NSString stringWithFormat:@"（%s、%@）", hdr::format_name(fd.chosen),
                                                 adobe ? @"Adobe の色と明るさ" : @"LibRaw の色と明るさ"];
        } catch (const std::exception& e) {
            error = ns(e.what());
        }
        dispatch_async(dispatch_get_main_queue(), ^{
            if (error) {
                [self endBusy:@"書き出せませんでした"];
                [self showError:error];
            } else {
                [self endBusy:[NSString stringWithFormat:@"書き出しました%@: %@", summary, path]];
            }
            [self finishAutomationIfNeeded];
        });
    });
}

// ---- 表 ----------------------------------------------------------------------------------

- (NSInteger)numberOfRowsInTableView:(NSTableView*)tableView {
    (void)tableView;
    return static_cast<NSInteger>(_rows.count);
}

- (NSView*)tableView:(NSTableView*)tableView viewForTableColumn:(NSTableColumn*)column row:(NSInteger)row {
    NSTextField* cell = [tableView makeViewWithIdentifier:[column identifier] owner:self];
    if (!cell) {
        cell = [NSTextField labelWithString:@""];
        [cell setIdentifier:[column identifier]];
        [cell setLineBreakMode:NSLineBreakByTruncatingMiddle];
        [cell setFont:[NSFont monospacedDigitSystemFontOfSize:12 weight:NSFontWeightRegular]];
    }
    FrameRow* r = _rows[static_cast<NSUInteger>(row)];
    NSString* ident = [column identifier];
    NSString* text = @"";
    if ([ident isEqualToString:@"number"]) text = r.number;
    else if ([ident isEqualToString:@"name"]) text = r.name;
    else if ([ident isEqualToString:@"shutter"]) text = r.shutter;
    else if ([ident isEqualToString:@"ev"]) text = r.relativeEV;
    else if ([ident isEqualToString:@"clip"]) text = r.clip;
    [cell setStringValue:text];
    [cell setToolTip:r.tooltip];
    // 番号の欄は由来マップの色で塗る（行は暗い順なので、行の番号 = 由来マップの色の番号）。
    if ([ident isEqualToString:@"number"]) {
        const unsigned char* c = kPalette[row % 8];
        [cell setDrawsBackground:YES];
        [cell setBackgroundColor:[NSColor colorWithSRGBRed:c[0] / 255.0 green:c[1] / 255.0 blue:c[2] / 255.0 alpha:1.0]];
        [cell setTextColor:(c[0] + c[1] + c[2] > 450) ? [NSColor blackColor] : [NSColor whiteColor]];
        [cell setAlignment:NSTextAlignmentCenter];
    }
    [cell setFont:r.isReference ? [NSFont boldSystemFontOfSize:12] : [NSFont monospacedDigitSystemFontOfSize:12 weight:NSFontWeightRegular]];
    return cell;
}

- (void)tableViewSelectionDidChange:(NSNotification*)notification {
    (void)notification;
    if ([_modeControl selectedSegment] == kViewFrame) [self renderPreview];
}

// ---- 検証用の自動操作 ----------------------------------------------------------------------

- (void)runAutomationFromEnvironment {
    const char* open = getenv("RBH_OPEN");
    const char* snap = getenv("RBH_SNAPSHOT");
    const char* exportPath = getenv("RBH_EXPORT");
    const char* mode = getenv("RBH_VIEW");
    if (snap) _snapshotPath = [NSString stringWithUTF8String:snap];
    if (exportPath) _autoExportPath = [NSString stringWithUTF8String:exportPath];
    if (mode) [_modeControl setSelectedSegment:atoi(mode)];
    if (!open) return;
    NSMutableArray<NSURL*>* urls = [NSMutableArray array];
    for (NSString* line in [[NSString stringWithUTF8String:open] componentsSeparatedByString:@"\n"]) {
        if (line.length) [urls addObject:[NSURL fileURLWithPath:line]];
    }
    _automationPending = YES;
    [self addFiles:urls];
}

- (void)finishAutomationIfNeeded {
    if (!_automationPending || _busyCount > 0) return;
    if (_autoExportPath) {
        NSString* p = _autoExportPath;
        _autoExportPath = nil;
        [self writeDNGToPath:p];
        return;
    }
    _automationPending = NO;
    if (_snapshotPath) {
        // ウインドウの枠を含めずに中身を描く。cacheDisplayInRect では文字が描かれないことがあるので、
        // ビットマップの文脈へ直接描く。
        NSView* v = [[self window] contentView];
        NSBitmapImageRep* rep = [v bitmapImageRepForCachingDisplayInRect:[v bounds]];
        NSGraphicsContext* ctx = [NSGraphicsContext graphicsContextWithBitmapImageRep:rep];
        [NSGraphicsContext saveGraphicsState];
        [NSGraphicsContext setCurrentContext:ctx];
        [[NSColor windowBackgroundColor] setFill];
        NSRectFill([v bounds]);
        [NSGraphicsContext restoreGraphicsState];
        [v displayRectIgnoringOpacity:[v bounds] inContext:ctx];
        NSData* png = [rep representationUsingType:NSBitmapImageFileTypePNG properties:@{}];
        [png writeToFile:_snapshotPath atomically:YES];
        [NSApp terminate:nil];
    }
}

@end
