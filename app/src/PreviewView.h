#import <Cocoa/Cocoa.h>

// 拡大・縮小・スクロールのできる画像表示。
// 画像が表示領域より小さいときは中央に置く。ピンチ・⌘＋スクロールで拡大縮小する。
@interface PreviewView : NSScrollView

// 画像を差し替える。keepZoom = YES なら拡大率と位置を保つ（同じ寸法の画像のとき）。
- (void)setImage:(NSImage*)image keepZoom:(BOOL)keepZoom;
- (NSImage*)image;

- (void)zoomToFit;
- (void)zoomToActual;
- (void)zoomBy:(CGFloat)factor;

// 画像が無いときに中央に出す案内。
@property(nonatomic, copy) NSString* placeholder;

@end
