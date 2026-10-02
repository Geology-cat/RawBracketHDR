#import "PreviewView.h"

#include <cmath>

// 画像が小さいときに中央に置くクリップビュー。
@interface CenteringClipView : NSClipView
@end

@implementation CenteringClipView
- (NSRect)constrainBoundsRect:(NSRect)proposed {
    NSRect r = [super constrainBoundsRect:proposed];
    NSView* doc = [self documentView];
    if (!doc) return r;
    const NSRect f = [doc frame];
    if (r.size.width > f.size.width) r.origin.x = (f.size.width - r.size.width) / 2.0;
    if (r.size.height > f.size.height) r.origin.y = (f.size.height - r.size.height) / 2.0;
    return r;
}
@end

// 画像を描くビュー（NSImageView は拡大時に補間するので、画素が見えるよう自分で描く）。
// ドラッグで表示位置を動かす（手のひらのカーソル）。
@interface PreviewImageView : NSView
@property(nonatomic, strong) NSImage* image;
@end

@implementation PreviewImageView {
    NSPoint _dragStart;     // ドラッグを始めた位置（ウインドウの座標）
    NSPoint _originStart;   // そのときの表示位置（画像の座標）
    BOOL _dragging;
}

- (BOOL)isOpaque {
    return NO;
}

- (BOOL)acceptsFirstMouse:(NSEvent*)event {
    (void)event;
    return YES;
}

- (void)drawRect:(NSRect)dirty {
    (void)dirty;
    if (!_image) return;
    NSGraphicsContext* ctx = [NSGraphicsContext currentContext];
    // 拡大しているときは画素がそのまま見えるように、縮小しているときは滑らかに。
    CGFloat mag = 1.0;
    NSScrollView* sv = [self enclosingScrollView];
    if (sv) mag = [sv magnification];
    [ctx setImageInterpolation:mag >= 1.5 ? NSImageInterpolationNone : NSImageInterpolationHigh];
    [_image drawInRect:[self bounds] fromRect:NSZeroRect operation:NSCompositingOperationSourceOver fraction:1.0];
}

- (void)resetCursorRects {
    if (_image) [self addCursorRect:[self visibleRect] cursor:_dragging ? [NSCursor closedHandCursor] : [NSCursor openHandCursor]];
}

- (void)mouseDown:(NSEvent*)event {
    if (!_image) return;
    NSClipView* clip = [[self enclosingScrollView] contentView];
    _dragStart = [event locationInWindow];
    _originStart = [clip bounds].origin;
    _dragging = YES;
    [[NSCursor closedHandCursor] set];
    [[self window] invalidateCursorRectsForView:self];
}

- (void)mouseDragged:(NSEvent*)event {
    if (!_dragging) return;
    NSScrollView* sv = [self enclosingScrollView];
    NSClipView* clip = [sv contentView];
    const NSPoint p = [event locationInWindow];
    // 画面上の移動量を画像の座標に直す（拡大率で割る）。掴んだ所がカーソルについてくるように逆向きに動かす。
    const CGFloat mag = [sv magnification];
    NSRect b = [clip bounds];
    b.origin.x = _originStart.x - (p.x - _dragStart.x) / mag;
    b.origin.y = _originStart.y - (p.y - _dragStart.y) / mag * ([self isFlipped] ? -1.0 : 1.0);
    [clip scrollToPoint:[clip constrainBoundsRect:b].origin];
    [sv reflectScrolledClipView:clip];
}

- (void)mouseUp:(NSEvent*)event {
    (void)event;
    _dragging = NO;
    [[self window] invalidateCursorRectsForView:self];
}
@end

@implementation PreviewView {
    PreviewImageView* _imageView;
    NSTextField* _placeholderLabel;
}

- (instancetype)initWithFrame:(NSRect)frame {
    self = [super initWithFrame:frame];
    if (self) {
        CenteringClipView* clip = [[CenteringClipView alloc] initWithFrame:frame];
        [clip setDrawsBackground:YES];
        [clip setBackgroundColor:[NSColor colorWithWhite:0.16 alpha:1.0]];
        [self setContentView:clip];
        // スクロールバーは出さない（表示位置はドラッグで動かす）。
        [self setHasVerticalScroller:NO];
        [self setHasHorizontalScroller:NO];
        [self setAllowsMagnification:YES];
        [self setMinMagnification:0.02];
        [self setMaxMagnification:16.0];
        [self setBorderType:NSNoBorder];
        _imageView = [[PreviewImageView alloc] initWithFrame:NSMakeRect(0, 0, 10, 10)];
        [self setDocumentView:_imageView];

        _placeholderLabel = [NSTextField labelWithString:@""];
        [_placeholderLabel setTextColor:[NSColor colorWithWhite:0.75 alpha:1.0]];
        [_placeholderLabel setFont:[NSFont systemFontOfSize:15]];
        [_placeholderLabel setAlignment:NSTextAlignmentCenter];
        [_placeholderLabel setMaximumNumberOfLines:0];
        [self addSubview:_placeholderLabel];
    }
    return self;
}

// スクロールビューは中のビューを自分で並べる（制約が効かない）ので、案内の文字もここで中央に置き、最前面に戻す。
- (void)tile {
    [super tile];
    if (!_placeholderLabel) return;
    const NSRect b = [self bounds];
    const CGFloat maxw = MAX(40.0, MIN(520.0, b.size.width - 40.0));
    [_placeholderLabel setPreferredMaxLayoutWidth:maxw];
    NSSize size = [_placeholderLabel fittingSize];
    size.width = MIN(size.width, maxw);
    [_placeholderLabel setFrame:NSIntegralRect(NSMakeRect(NSMidX(b) - size.width / 2.0, NSMidY(b) - size.height / 2.0, size.width, size.height))];
    if ([[self subviews] lastObject] != _placeholderLabel) [self addSubview:_placeholderLabel positioned:NSWindowAbove relativeTo:nil];
}

- (void)setPlaceholder:(NSString*)placeholder {
    _placeholder = [placeholder copy];
    [_placeholderLabel setStringValue:placeholder ?: @""];
    [_placeholderLabel setHidden:_imageView.image != nil];
    [self tile];
}

- (NSImage*)image {
    return _imageView.image;
}

- (void)setImage:(NSImage*)image keepZoom:(BOOL)keepZoom {
    const NSSize old = _imageView.image ? _imageView.image.size : NSZeroSize;
    const BOOL same = image && NSEqualSizes(old, image.size);
    _imageView.image = image;
    [_placeholderLabel setHidden:image != nil];
    [[self window] invalidateCursorRectsForView:_imageView];
    if (!image) {
        [_imageView setFrame:NSMakeRect(0, 0, 10, 10)];
        [_imageView setNeedsDisplay:YES];
        [self zoomChanged];
        return;
    }
    if (!same) {
        [_imageView setFrame:NSMakeRect(0, 0, image.size.width, image.size.height)];
    }
    [_imageView setNeedsDisplay:YES];
    if (!(keepZoom && same)) [self zoomToFit];
}

- (void)zoomChanged {
    [_imageView setNeedsDisplay:YES];
    [[self window] invalidateCursorRectsForView:_imageView];
    if (_onZoomChange) _onZoomChange();
}

- (CGFloat)zoomPercent {
    if (!_imageView.image) return 0.0;
    const CGFloat scale = self.window ? self.window.backingScaleFactor : 1.0;
    return [self magnification] * scale;
}

- (void)setZoom:(CGFloat)mag around:(NSPoint)center {
    mag = MAX([self minMagnification], MIN([self maxMagnification], mag));
    [self setMagnification:mag centeredAtPoint:center];
    [self zoomChanged];
}

- (NSPoint)visibleCenter {
    const NSRect visible = [[self contentView] documentVisibleRect];
    return NSMakePoint(NSMidX(visible), NSMidY(visible));
}

- (void)zoomToFit {
    if (!_imageView.image) return;
    const NSSize img = _imageView.image.size;
    const NSSize area = [self contentSize];
    if (img.width <= 0 || img.height <= 0) return;
    const CGFloat m = MIN(area.width / img.width, area.height / img.height);
    [self setMagnification:MAX([self minMagnification], MIN(m, 8.0))];
    [self zoomChanged];
}

- (void)zoomToActual {
    // 画像の1画素を画面の1画素に（Retina では 0.5 倍）。
    const CGFloat scale = self.window ? self.window.backingScaleFactor : 1.0;
    [self setZoom:1.0 / scale around:[self visibleCenter]];
}

- (void)zoomBy:(CGFloat)factor {
    [self setZoom:[self magnification] * factor around:[self visibleCenter]];
}

- (void)magnifyWithEvent:(NSEvent*)event {
    [super magnifyWithEvent:event];
    [self zoomChanged];
}

- (void)scrollWheel:(NSEvent*)event {
    // スクロールでは表示位置を動かさず、カーソルの位置を中心に拡大縮小する（位置はドラッグで動かす）。
    if (!_imageView.image) return;
    CGFloat dy = [event scrollingDeltaY];
    if (![event hasPreciseScrollingDeltas]) dy *= 8.0;  // マウスのホイール（1 刻みが小さい）
    if (dy == 0.0) return;
    const NSPoint p = [_imageView convertPoint:[event locationInWindow] fromView:nil];
    [self setZoom:[self magnification] * std::exp(dy * 0.01) around:p];
}

@end
