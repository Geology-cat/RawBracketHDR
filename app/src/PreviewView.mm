#import "PreviewView.h"

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

// 画像を描くだけのビュー（NSImageView は拡大時に補間するので、画素が見えるよう自分で描く）。
@interface PreviewImageView : NSView
@property(nonatomic, strong) NSImage* image;
@end

@implementation PreviewImageView
- (BOOL)isOpaque {
    return NO;
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
        [self setHasVerticalScroller:YES];
        [self setHasHorizontalScroller:YES];
        [self setAutohidesScrollers:YES];
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
        [_placeholderLabel setTranslatesAutoresizingMaskIntoConstraints:NO];
        [self addSubview:_placeholderLabel];
        [NSLayoutConstraint activateConstraints:@[
            [[_placeholderLabel centerXAnchor] constraintEqualToAnchor:[self centerXAnchor]],
            [[_placeholderLabel centerYAnchor] constraintEqualToAnchor:[self centerYAnchor]],
            [[_placeholderLabel widthAnchor] constraintLessThanOrEqualToAnchor:[self widthAnchor] constant:-40],
        ]];
    }
    return self;
}

- (void)setPlaceholder:(NSString*)placeholder {
    _placeholder = [placeholder copy];
    [_placeholderLabel setStringValue:placeholder ?: @""];
    [_placeholderLabel setHidden:_imageView.image != nil];
}

- (NSImage*)image {
    return _imageView.image;
}

- (void)setImage:(NSImage*)image keepZoom:(BOOL)keepZoom {
    const NSSize old = _imageView.image ? _imageView.image.size : NSZeroSize;
    const BOOL same = image && NSEqualSizes(old, image.size);
    _imageView.image = image;
    [_placeholderLabel setHidden:image != nil];
    if (!image) {
        [_imageView setFrame:NSMakeRect(0, 0, 10, 10)];
        [_imageView setNeedsDisplay:YES];
        return;
    }
    if (!same) {
        [_imageView setFrame:NSMakeRect(0, 0, image.size.width, image.size.height)];
    }
    [_imageView setNeedsDisplay:YES];
    if (!(keepZoom && same)) [self zoomToFit];
}

- (void)zoomToFit {
    if (!_imageView.image) return;
    const NSSize img = _imageView.image.size;
    const NSSize area = [self contentSize];
    if (img.width <= 0 || img.height <= 0) return;
    const CGFloat m = MIN(area.width / img.width, area.height / img.height);
    [self setMagnification:MAX([self minMagnification], MIN(m, 1.0 * 8))];
}

- (void)zoomToActual {
    // 画像の1画素を画面の1画素に（Retina では 0.5 倍）。
    const CGFloat scale = self.window ? self.window.backingScaleFactor : 1.0;
    NSRect visible = [[self contentView] documentVisibleRect];
    const NSPoint center = NSMakePoint(NSMidX(visible), NSMidY(visible));
    [self setMagnification:1.0 / scale centeredAtPoint:center];
}

- (void)zoomBy:(CGFloat)factor {
    NSRect visible = [[self contentView] documentVisibleRect];
    const NSPoint center = NSMakePoint(NSMidX(visible), NSMidY(visible));
    [self setMagnification:[self magnification] * factor centeredAtPoint:center];
}

- (void)magnifyWithEvent:(NSEvent*)event {
    [super magnifyWithEvent:event];
    [_imageView setNeedsDisplay:YES];
}

- (void)scrollWheel:(NSEvent*)event {
    // ⌘＋スクロールで拡大縮小。
    if ([event modifierFlags] & NSEventModifierFlagCommand) {
        const CGFloat dy = [event scrollingDeltaY];
        const NSPoint p = [_imageView convertPoint:[event locationInWindow] fromView:nil];
        [self setMagnification:[self magnification] * (dy > 0 ? 1.1 : 1.0 / 1.1) centeredAtPoint:p];
        [_imageView setNeedsDisplay:YES];
        return;
    }
    [super scrollWheel:event];
}

@end
