#import <Cocoa/Cocoa.h>

// 拡大・縮小のできる画像表示。スクロールバーは出さず、カーソルで掴んで（ドラッグして）動かす。
// 画像が表示領域より小さいときは中央に置く。ピンチ・ホイールで、カーソルの位置を中心に拡大縮小する。
@interface PreviewView : NSScrollView

// 画像を差し替える。keepZoom = YES なら拡大率と位置を保つ（同じ寸法の画像のとき）。
- (void)setImage:(NSImage*)image keepZoom:(BOOL)keepZoom;
- (NSImage*)image;

- (void)zoomToFit;
- (void)zoomToActual;
- (void)zoomBy:(CGFloat)factor;

// 画像の 1 画素が画面の何画素ぶんか（1.0 = 100%）。
- (CGFloat)zoomPercent;

// 拡大率が変わったときに呼ぶ（表示の更新用）。
@property(nonatomic, copy) void (^onZoomChange)(void);

// 画像が無いときに中央に出す案内。
@property(nonatomic, copy) NSString* placeholder;

@end
