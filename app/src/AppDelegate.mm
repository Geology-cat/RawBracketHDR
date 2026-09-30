#import "AppDelegate.h"

#import "MainWindowController.h"

@implementation AppDelegate {
    MainWindowController* _controller;
    // ウインドウができる前に Finder から渡されたファイル（ドックのアイコンへのドロップなど）。
    NSMutableArray<NSURL*>* _pending;
}

- (instancetype)init {
    self = [super init];
    if (self) _pending = [NSMutableArray array];
    return self;
}

- (void)applicationDidFinishLaunching:(NSNotification*)notification {
    (void)notification;
    [self buildMenu];
    _controller = [[MainWindowController alloc] init];
    [_controller showWindow:nil];
    [[_controller window] makeKeyAndOrderFront:nil];
    [NSApp activateIgnoringOtherApps:YES];
    if (_pending.count) {
        [_controller addFiles:_pending];
        [_pending removeAllObjects];
    }
    // 検証用: RBH_OPEN（改行区切りのパス）を開き、RBH_SNAPSHOT にウインドウの中身を PNG で書いて終了する。
    [_controller runAutomationFromEnvironment];
}

- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication*)sender {
    (void)sender;
    return YES;
}

- (void)application:(NSApplication*)sender openURLs:(NSArray<NSURL*>*)urls {
    (void)sender;
    if (_controller) {
        [_controller addFiles:urls];
    } else {
        [_pending addObjectsFromArray:urls];
    }
}

- (void)buildMenu {
    NSMenu* bar = [[NSMenu alloc] init];

    NSMenuItem* appItem = [[NSMenuItem alloc] init];
    NSMenu* appMenu = [[NSMenu alloc] initWithTitle:@"RawBracketHDR"];
    [appMenu addItemWithTitle:@"RawBracketHDR について" action:@selector(orderFrontStandardAboutPanel:) keyEquivalent:@""];
    [appMenu addItem:[NSMenuItem separatorItem]];
    [appMenu addItemWithTitle:@"RawBracketHDR を隠す" action:@selector(hide:) keyEquivalent:@"h"];
    NSMenuItem* hideOthers = [appMenu addItemWithTitle:@"ほかを隠す" action:@selector(hideOtherApplications:) keyEquivalent:@"h"];
    [hideOthers setKeyEquivalentModifierMask:NSEventModifierFlagCommand | NSEventModifierFlagOption];
    [appMenu addItemWithTitle:@"すべてを表示" action:@selector(unhideAllApplications:) keyEquivalent:@""];
    [appMenu addItem:[NSMenuItem separatorItem]];
    [appMenu addItemWithTitle:@"RawBracketHDR を終了" action:@selector(terminate:) keyEquivalent:@"q"];
    [appItem setSubmenu:appMenu];
    [bar addItem:appItem];

    NSMenuItem* fileItem = [[NSMenuItem alloc] init];
    NSMenu* fileMenu = [[NSMenu alloc] initWithTitle:@"ファイル"];
    [fileMenu addItemWithTitle:@"RAW を開く…" action:@selector(openDocument:) keyEquivalent:@"o"];
    [fileMenu addItemWithTitle:@"DNG を書き出す…" action:@selector(exportDNG:) keyEquivalent:@"e"];
    [fileMenu addItem:[NSMenuItem separatorItem]];
    [fileMenu addItemWithTitle:@"すべて取り除く" action:@selector(clearFrames:) keyEquivalent:@""];
    [fileMenu addItemWithTitle:@"閉じる" action:@selector(performClose:) keyEquivalent:@"w"];
    [fileItem setSubmenu:fileMenu];
    [bar addItem:fileItem];

    NSMenuItem* editItem = [[NSMenuItem alloc] init];
    NSMenu* editMenu = [[NSMenu alloc] initWithTitle:@"編集"];
    [editMenu addItemWithTitle:@"取り消す" action:@selector(undo:) keyEquivalent:@"z"];
    [editMenu addItem:[NSMenuItem separatorItem]];
    [editMenu addItemWithTitle:@"カット" action:@selector(cut:) keyEquivalent:@"x"];
    [editMenu addItemWithTitle:@"コピー" action:@selector(copy:) keyEquivalent:@"c"];
    [editMenu addItemWithTitle:@"ペースト" action:@selector(paste:) keyEquivalent:@"v"];
    [editMenu addItemWithTitle:@"すべてを選択" action:@selector(selectAll:) keyEquivalent:@"a"];
    [editItem setSubmenu:editMenu];
    [bar addItem:editItem];

    NSMenuItem* viewItem = [[NSMenuItem alloc] init];
    NSMenu* viewMenu = [[NSMenu alloc] initWithTitle:@"表示"];
    [viewMenu addItemWithTitle:@"全体を表示" action:@selector(zoomToFit:) keyEquivalent:@"0"];
    [viewMenu addItemWithTitle:@"実寸（100%）" action:@selector(zoomToActual:) keyEquivalent:@"1"];
    [viewMenu addItemWithTitle:@"拡大" action:@selector(zoomIn:) keyEquivalent:@"+"];
    [viewMenu addItemWithTitle:@"縮小" action:@selector(zoomOut:) keyEquivalent:@"-"];
    [viewItem setSubmenu:viewMenu];
    [bar addItem:viewItem];

    NSMenuItem* windowItem = [[NSMenuItem alloc] init];
    NSMenu* windowMenu = [[NSMenu alloc] initWithTitle:@"ウインドウ"];
    [windowMenu addItemWithTitle:@"しまう" action:@selector(performMiniaturize:) keyEquivalent:@"m"];
    [windowMenu addItemWithTitle:@"拡大／縮小" action:@selector(performZoom:) keyEquivalent:@""];
    [windowItem setSubmenu:windowMenu];
    [bar addItem:windowItem];
    [NSApp setWindowsMenu:windowMenu];

    [NSApp setMainMenu:bar];
}

@end
