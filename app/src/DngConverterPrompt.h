#import <Cocoa/Cocoa.h>

// Adobe DNG Converter（任意）の入手を案内するウインドウ。
// 無くても動くことを明記したうえで、Adobe の公式サイトからダウンロードしてインストーラーを開く。
// インストール自体は利用者が Adobe のインストーラーで行う（使用許諾の確認・管理者のパスワードを含む）。
@interface DngConverterPrompt : NSWindowController

// 起動時に案内を出すか（見つからず、「表示しない」にしていないとき）。
+ (BOOL)shouldShowAtLaunch;

// installed: 今 Adobe DNG Converter が見つかっているか（文言を変える）。
- (instancetype)initWithInstalled:(BOOL)installed;

// 親ウインドウのシートとして出す。
- (void)beginSheetForWindow:(NSWindow*)parent;

@end
