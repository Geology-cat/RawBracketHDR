#import "DngConverterPrompt.h"

namespace {

// Adobe が公開している「最新版の DNG Converter（macOS）」への固定のリンク（版ごとの dmg へ転送される）。
NSString* const kLatestURL = @"https://www.adobe.com/go/dng_converter_mac";
// Adobe の DNG Converter の説明・ダウンロードのページ（古い版の案内もここから）。
NSString* const kInfoURL = @"https://helpx.adobe.com/camera-raw/using/adobe-dng-converter.html";
// 最新版が動く macOS（Adobe の動作環境: macOS 13 Ventura 以降。2025 年 1 月更新のページで確認）。
const NSInteger kLatestMinMajor = 13;
NSString* const kSuppressKey = @"SuppressDngConverterPrompt";

}  // namespace

@interface DngConverterPrompt () <NSURLSessionDownloadDelegate>
@end

@implementation DngConverterPrompt {
    BOOL _installed;
    NSButton* _downloadButton;
    NSButton* _pageButton;
    NSButton* _closeButton;
    NSButton* _suppressCheck;
    NSProgressIndicator* _progress;
    NSTextField* _status;
    NSURLSession* _session;
    NSURLSessionDownloadTask* _task;
}

+ (BOOL)shouldShowAtLaunch {
    return ![[NSUserDefaults standardUserDefaults] boolForKey:kSuppressKey];
}

+ (BOOL)latestRunsHere {
    return [[NSProcessInfo processInfo] operatingSystemVersion].majorVersion >= kLatestMinMajor;
}

- (instancetype)initWithInstalled:(BOOL)installed {
    NSWindow* w = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, 560, 380)
                                              styleMask:NSWindowStyleMaskTitled
                                                backing:NSBackingStoreBuffered
                                                  defer:NO];
    self = [super initWithWindow:w];
    if (self) {
        _installed = installed;
        [self build];
    }
    return self;
}

- (NSTextField*)paragraph:(NSString*)text {
    NSTextField* t = [NSTextField wrappingLabelWithString:text];
    [t setFont:[NSFont systemFontOfSize:12]];
    [t setSelectable:NO];
    return t;
}

- (void)build {
    NSView* root = [[self window] contentView];
    NSImageView* icon = [NSImageView imageViewWithImage:[NSApp applicationIconImage]];
    [[icon widthAnchor] constraintEqualToConstant:64].active = YES;
    [[icon heightAnchor] constraintEqualToConstant:64].active = YES;

    NSTextField* title = [NSTextField labelWithString:_installed ? @"Adobe DNG Converter（任意）は使用中です" : @"Adobe DNG Converter（無料・任意）を入れますか？"];
    [title setFont:[NSFont boldSystemFontOfSize:14]];

    NSOperatingSystemVersion v = [[NSProcessInfo processInfo] operatingSystemVersion];
    NSString* osNote = [DngConverterPrompt latestRunsHere]
                           ? @"下の「ダウンロードしてインストール」で、Adobe の公式サイトから最新版（約 1.8 GB）をダウンロードし、インストーラーを開きます。"
                             @"インストールは、表示される Adobe のインストーラーで使用許諾を確認しながら進めてください。"
                           : [NSString stringWithFormat:@"お使いの macOS %ld.%ld では最新版は動きません（最新版は macOS %ld 以降）。"
                                                        @"「Adobe のページを開く」から、この macOS に対応する版を確かめてください。",
                                                        (long)v.majorVersion, (long)v.minorVersion, (long)kLatestMinMajor];
    NSTextField* p1 = [self paragraph:@"RawHDR Composer は、Adobe DNG Converter が無くてもそのまま使えます。合成・プレビュー・DNG の書き出しなど、すべての機能が動きます。"];
    [p1 setFont:[NSFont boldSystemFontOfSize:12]];
    NSTextField* p2 = [self paragraph:@"入れておくと、書き出す DNG の色・明るさ・カメラプロファイル・ノイズの情報を、Lightroom や Camera Raw で元の RAW を開いたときとそろえられます"
                                      @"（無いときは LibRaw の値を使うので、色と明るさが元の RAW とわずかに違います）。インストールすると、このアプリが自動で見つけて使います。"];
    NSTextField* p3 = [self paragraph:_installed ? @"新しい版に更新したいときは、下のボタンから入手できます。" : osNote];

    _progress = [[NSProgressIndicator alloc] initWithFrame:NSZeroRect];
    [_progress setStyle:NSProgressIndicatorStyleBar];
    [_progress setIndeterminate:NO];
    [_progress setMinValue:0.0];
    [_progress setMaxValue:1.0];
    [_progress setHidden:YES];
    _status = [NSTextField wrappingLabelWithString:@""];
    [_status setFont:[NSFont systemFontOfSize:11]];
    [_status setTextColor:[NSColor secondaryLabelColor]];

    _suppressCheck = [NSButton checkboxWithTitle:@"起動時にこの案内を表示しない" target:nil action:nil];
    [_suppressCheck setState:[[NSUserDefaults standardUserDefaults] boolForKey:kSuppressKey] ? NSControlStateValueOn : NSControlStateValueOff];
    [_suppressCheck setHidden:_installed];

    _downloadButton = [NSButton buttonWithTitle:@"ダウンロードしてインストール…" target:self action:@selector(download:)];
    _pageButton = [NSButton buttonWithTitle:@"Adobe のページを開く" target:self action:@selector(openPage:)];
    _closeButton = [NSButton buttonWithTitle:_installed ? @"閉じる" : @"あとで" target:self action:@selector(close:)];
    [_closeButton setKeyEquivalent:@"\e"];
    if ([DngConverterPrompt latestRunsHere]) {
        [_downloadButton setKeyEquivalent:@"\r"];
    } else {
        [_downloadButton setHidden:YES];
        [_pageButton setKeyEquivalent:@"\r"];
    }

    NSStackView* text = [NSStackView stackViewWithViews:@[ title, p1, p2, p3, _progress, _status, _suppressCheck ]];
    [text setOrientation:NSUserInterfaceLayoutOrientationVertical];
    [text setAlignment:NSLayoutAttributeLeading];
    [text setSpacing:10];
    for (NSView* v2 in @[ p1, p2, p3, _progress, _status ]) [[v2 widthAnchor] constraintEqualToAnchor:[text widthAnchor]].active = YES;

    NSStackView* top = [NSStackView stackViewWithViews:@[ icon, text ]];
    [top setAlignment:NSLayoutAttributeTop];
    [top setSpacing:16];

    NSView* spacer = [[NSView alloc] initWithFrame:NSZeroRect];
    [spacer setContentHuggingPriority:1 forOrientation:NSLayoutConstraintOrientationHorizontal];
    NSStackView* buttons = [NSStackView stackViewWithViews:@[ spacer, _closeButton, _pageButton, _downloadButton ]];
    [buttons setSpacing:8];

    NSStackView* all = [NSStackView stackViewWithViews:@[ top, buttons ]];
    [all setOrientation:NSUserInterfaceLayoutOrientationVertical];
    [all setAlignment:NSLayoutAttributeLeading];
    [all setSpacing:18];
    [all setEdgeInsets:NSEdgeInsetsMake(20, 20, 16, 20)];
    [all setTranslatesAutoresizingMaskIntoConstraints:NO];
    [root addSubview:all];
    [NSLayoutConstraint activateConstraints:@[
        [[all leadingAnchor] constraintEqualToAnchor:[root leadingAnchor]],
        [[all trailingAnchor] constraintEqualToAnchor:[root trailingAnchor]],
        [[all topAnchor] constraintEqualToAnchor:[root topAnchor]],
        [[all bottomAnchor] constraintEqualToAnchor:[root bottomAnchor]],
        [[root widthAnchor] constraintEqualToConstant:600],
        [[buttons widthAnchor] constraintEqualToAnchor:[all widthAnchor] constant:-40],
        [[top widthAnchor] constraintEqualToAnchor:[all widthAnchor] constant:-40],
    ]];
}

- (void)beginSheetForWindow:(NSWindow*)parent {
    // 中身に合わせた高さにする。
    NSView* root = [[self window] contentView];
    [root layoutSubtreeIfNeeded];
    [[self window] setContentSize:[root fittingSize]];
    [parent beginSheet:[self window] completionHandler:nil];
}

- (void)saveSuppress {
    if (_installed) return;
    [[NSUserDefaults standardUserDefaults] setBool:[_suppressCheck state] == NSControlStateValueOn forKey:kSuppressKey];
}

- (void)close:(id)sender {
    (void)sender;
    [_task cancel];
    _task = nil;
    [_session invalidateAndCancel];
    _session = nil;
    [self saveSuppress];
    NSWindow* w = [self window];
    if ([w sheetParent]) {
        [[w sheetParent] endSheet:w];
    } else {
        [w orderOut:nil];
    }
}

- (void)openPage:(id)sender {
    (void)sender;
    [[NSWorkspace sharedWorkspace] openURL:[NSURL URLWithString:kInfoURL]];
}

- (void)download:(id)sender {
    (void)sender;
    if (_task) return;
    [_downloadButton setEnabled:NO];
    [_closeButton setTitle:@"中止"];
    [_progress setHidden:NO];
    [_progress setDoubleValue:0.0];
    [_status setStringValue:@"Adobe の公式サイトからダウンロードしています…"];
    [self refit];
    NSURLSessionConfiguration* cfg = [NSURLSessionConfiguration defaultSessionConfiguration];
    _session = [NSURLSession sessionWithConfiguration:cfg delegate:self delegateQueue:[NSOperationQueue mainQueue]];
    _task = [_session downloadTaskWithURL:[NSURL URLWithString:kLatestURL]];
    [_task resume];
}

- (void)failWith:(NSString*)message {
    _task = nil;
    [_session finishTasksAndInvalidate];
    _session = nil;
    [_progress setHidden:YES];
    [_downloadButton setEnabled:YES];
    [_closeButton setTitle:_installed ? @"閉じる" : @"あとで"];
    [_status setStringValue:message];
    [self refit];
}

// 状態の文言で高さが変わったら、ウインドウの高さを合わせる。
- (void)refit {
    NSView* root = [[self window] contentView];
    [root layoutSubtreeIfNeeded];
    [[self window] setContentSize:[root fittingSize]];
}

// ---- NSURLSessionDownloadDelegate ----

- (void)URLSession:(NSURLSession*)session
                 downloadTask:(NSURLSessionDownloadTask*)task
                 didWriteData:(int64_t)written
            totalBytesWritten:(int64_t)total
    totalBytesExpectedToWrite:(int64_t)expected {
    (void)session;
    (void)task;
    (void)written;
    if (expected > 0) {
        [_progress setDoubleValue:static_cast<double>(total) / static_cast<double>(expected)];
        [_status setStringValue:[NSString stringWithFormat:@"ダウンロード中… %.0f / %.0f MB", total / 1e6, expected / 1e6]];
    }
}

- (void)URLSession:(NSURLSession*)session downloadTask:(NSURLSessionDownloadTask*)task didFinishDownloadingToURL:(NSURL*)location {
    (void)session;
    // 転送先が Adobe の HTTPS のサーバーであることを確かめる（偽のファイルを開かないように）。
    NSURL* final = [[task response] URL];
    NSString* host = [[final host] lowercaseString];
    const BOOL adobe = [[final scheme] isEqualToString:@"https"] && ([host isEqualToString:@"adobe.com"] || [host hasSuffix:@".adobe.com"]);
    NSHTTPURLResponse* http = [[task response] isKindOfClass:[NSHTTPURLResponse class]] ? (NSHTTPURLResponse*)[task response] : nil;
    if (!adobe || (http && [http statusCode] != 200)) {
        [self failWith:@"Adobe のサーバーから正しく取得できませんでした。「Adobe のページを開く」から入手してください。"];
        return;
    }
    NSString* name = [[task response] suggestedFilename];
    if (![[name pathExtension] isEqualToString:@"dmg"]) name = @"DNGConverter.dmg";
    NSFileManager* fm = [NSFileManager defaultManager];
    NSURL* downloads = [[fm URLsForDirectory:NSDownloadsDirectory inDomains:NSUserDomainMask] firstObject];
    NSURL* dest = [downloads URLByAppendingPathComponent:name];
    [fm removeItemAtURL:dest error:nil];
    NSError* err = nil;
    if (![fm moveItemAtURL:location toURL:dest error:&err]) {
        [self failWith:[NSString stringWithFormat:@"ダウンロードしたファイルを保存できませんでした: %@", [err localizedDescription]]];
        return;
    }
    _task = nil;
    [_progress setDoubleValue:1.0];
    // dmg を開く（Finder がマウントし、Adobe のインストーラーが表示される）。インストーラーの署名は macOS が確かめる。
    [[NSWorkspace sharedWorkspace] openURL:dest];
    [_closeButton setTitle:@"閉じる"];
    [_status setStringValue:[NSString stringWithFormat:@"%@ に保存して開きました。表示されたインストーラーの指示に従ってインストールしてください。"
                                                       @"インストールが済むと、このアプリが自動で見つけて使います。",
                                                       [dest path]]];
    [self refit];
}

- (void)URLSession:(NSURLSession*)session task:(NSURLSessionTask*)task didCompleteWithError:(NSError*)error {
    (void)session;
    (void)task;
    if (!error) return;
    if ([error code] == NSURLErrorCancelled) return;
    [self failWith:[NSString stringWithFormat:@"ダウンロードできませんでした（%@）。「Adobe のページを開く」から入手してください。", [error localizedDescription]]];
}

@end
