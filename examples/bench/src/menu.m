#import <AppKit/AppKit.h>

void install_menus() {
  NSMenu *bar = [NSMenu new];
  NSMenuItem *app = [bar addItemWithTitle:@"" action:nil keyEquivalent:@""];
  NSMenu *menu = [[NSMenu alloc] initWithTitle:@"Benchmark"];

  [menu addItemWithTitle:@"Quit Benchmark"
                  action:@selector(terminate:)
           keyEquivalent:@"q"];

  app.submenu = menu;
  NSApp.mainMenu = bar;
}
