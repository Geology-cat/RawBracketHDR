#import <Cocoa/Cocoa.h>

// メインウインドウ。左にフレームの一覧、中央にプレビュー、右に設定と書き出し。
//
// エンジン（C++）のデータは専用の直列キューの上でだけ触る。画面へは、キューで作った
// 表示用の値（文字列・画像）だけを渡す。
@interface MainWindowController : NSWindowController

- (void)addFiles:(NSArray<NSURL*>*)urls;

// 検証用: 環境変数 RBH_OPEN（改行区切りのパス）を開き、処理が終わったら
// RBH_SNAPSHOT のパスにウインドウの中身を PNG で書いて終了する。
- (void)runAutomationFromEnvironment;

@end
