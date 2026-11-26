# civue

*A window and a frame.* A native GUI toolkit for C23, part of CiKo: Metal and AppKit on macOS, Vulkan and Wayland
on Linux, no other toolkit underneath. Its widgets are headless, like Base UI's or GPUI's: civue handles the pointer,
the keys, the focus and the geometry, and the app paints them.

```c
#include <civue/dsl.h>

typedef struct {
  int count;
  char *name;
  civue_field_look look;                                       // the text field's colors, the app's own
} Hello;

static void frame(void *ctx, civue_ui *ui) {                   // runs when input, a timer or an animation asks
  Hello *app = ctx;
  div(size(full), p(6), text_size(px(13)), line_height(px(20))) {      // the frame's ui, never passed around
    div(flex, flex_col, gap(4), w(80), p(6), rounded(xl), bg(CIVUE_WHITE)) {    // laid out like css flexbox
      text("Hello from civue", text_size(px(17)), font_weight(semibold));
      div(key(id("add")), flex, items_center, justify_center, w(24), h(7), rounded(md),
          bg(rgb(0x0d99ff)), active(bg(rgb(0x0768cf))), text_color(CIVUE_WHITE)) {
        if (button().activated) app->count++;                  // the widget answers over the node
        text("Click me");
      }
      div(key(id("name")), w(px(200)), h(7)) {
        text_field(&app->name, "Your name", app->look);
      }
    }
  }
}

static civue_app make(civue_ui *ui, void *arg) { return (civue_app){arg, frame}; }

int main(void) {
  Hello hello = {};
  civue_window_options o = {.title = "Hello", .width = 368, .height = 228};
  civue_window_run(&o, make, &hello);                         // returns when the window closes
}
```

`examples/hello` is the whole app, with a dark switch, a slider and a tooltip, painting its own look without a single coordinate written by hand. `examples/layout` shows grids, baselines, collapsing margins and a list with scroll bars of its own. `examples/bench` is the app of the comparison below, a counter, a list of a hundred thousand rows, a grid of live numbers and a document rewrapped every frame. Each example is a CMake project of its own, its components in files of their own, taking civue from this checkout where an app would fetch it.

| Dear ImGui | civue |
| --- | --- |
| `ImGui::NewFrame`, `ImGui::Render` | `civue_begin_frame`, `civue_end_frame`, or `civue_window_run` doing both |
| `ImGui::GetID("x")`, `PushID` | `civue_id_of("x")`, `civue_ids(parent, "x")`, `civue_idn(parent, n)` |
| `ImGui::SameLine`, `BeginGroup`, `SetCursorPos` | `div(flex, ...) { ... }`, nodes laid out like css blocks, flexbox and grid |
| `ImGui::Button`, `Checkbox`, `SliderFloat` | `civue_button`, `civue_toggle`, `civue_slider`, answering with a state to paint |
| `ImGui::RadioButton` | `civue_radio_group`, the arrows moving the choice while it has the focus |
| `ImGui::InputText` | `civue_text_field`, `civue_text_area`, with input methods and the clipboard |
| `ImGui::BeginPopup`, `BeginMenu` | `civue_menu` with submenus, `civue_select_menu`, their rows painted by the app |
| `ImGui::SetTooltip` | `civue_tooltip`, true after half a second of hover, `civue_tooltip_at` placing it |
| `ImGui::BeginChild` with scrolling | `div(overflow_y_scroll, ...)`, or `CIVUE_SCROLLING(ui, id, r, height, content)` over a rect |
| `ImDrawList` | `civue_draw_list`, instanced quads with clips and turns |
| `ImGui::GetStateStorage` | `civue_memory(ui, id, size, drop)`, forgotten with `civue_forget` |
| a backend per platform | built in: `civue_window_run` on macOS and Wayland |

The short names come from `<civue/dsl.h>`: `id`, `rgb`, `point`, `xywh`, the blocks `clipped(r)`, `overlay()` and `scrolling(...)`, `div`, `text` and `icon`, `fill`, `stroke` and `label`, and the widgets over nodes.
None of them takes the ui: `civue_begin_frame` makes its ui the one of the thread, `civue_ui_current()`, and they work on it, so an app's own functions don't pass a ui around either. An app that would rather name it defines `CIVUE_DSL_UI` as a variable of its own, like `ui`, before including it. Reading the thread's ui costs nothing measurable, and the `civue_` functions all still take theirs.
They are function like macros, so a variable called `label` still works, and `<civue/dsl_end.h>` drops them.
gpui's style names aren't macros of their own: `div` and `text` paste each of their arguments after `CIVUE__S_`, so `flex`, `h` or `truncate` mean nothing anywhere else and no header or variable of the app clashes with them.
`<civue/civue.h>` alone has only the `civue_` and `CIVUE_` names.

## Modules

| header | what it has |
| --- | --- |
| `base.h` | points, rects and colors, growable arrays, hashing, utf-8, the clock |
| `gpu.h` | the process's gpu device, an `id<MTLDevice>` on macOS and `vulkan.h`'s device on Linux |
| `draw.h`, `gfx.h` | draw lists and atlases, and the renderer drawing them, offscreen too with `civue_gfx_snapshot` |
| `raster.h` | draw lists drawn on the cpu, and what changed between two of them |
| `text.h` | layout and rendering in the system's faces, or faces the app sets from font files |
| `icons.h` | svg icons from any iconify set or the app's own, tinted or in their own colors |
| `images.h` | images from urls the app fetches, cached in memory and on disk, decoded off the main thread |
| `ui.h` | frames, input, ids, interaction, focus, the light or dark appearance, animation, painting snapped to pixels |
| `widgets.h`, `field.h`, `numfield.h`, `rich.h` | headless controls, text fields, number fields, rich text editing |
| `layout.h` | nodes laid out like css blocks, flexbox and grid, painted like gpui's divs |
| `window.h` | the app, the window and the main loop, the keyboard and input methods, cursors, menus |
| `webview.h` | a web page of the system's shown over a rect of the frame, messages to and from it, macOS only for now |
| `system.h` | the clipboard and what other apps copy, file panels, spell checking |

The modules stack in that order and each only uses the ones above it. An app that draws its own way can stop at
`gfx.h`: `civue_gfx_new(civue_gpu_device())` and `civue_gfx_encode` into its own command buffers.

## How a frame works

- **Frames on demand.** The window builds a frame when input arrives, when `civue_request_repaint` or
  `civue_request_repaint_after` asked, or while an animation runs, then sleeps. On macOS the view's `CADisplayLink`
  drives it and pauses after eight idle refreshes. On Wayland the compositor's frame callback paces
  it, two frames in flight. No callback comes for a frame that changed nothing, so the frames `civue_window_wake`
  asks for after one come a display's frame apart, then further while they keep changing nothing, a tenth of a
  second at most: an app told a hundred times a second of what its window does not show builds few frames for it.
  Live resizes draw each size inside the resize's transaction, so nothing stretches.
- **Interaction from the last frame.** `civue_interact` registers a rect and answers with what the pointer did,
  hovered from the topmost region of the previous frame, so menus and popovers drawn later still win. Presses,
  drags, double clicks, focus, a keyboard focus ring and repaint requests all come from that.
- **Input moves in.** `civue_begin_frame` takes the window's `civue_input` and gives back what carries over:
  buttons held, the pointer and the modifiers. Keys are code points or `CIVUE_KEY_*`, digits come from the digit
  row on any layout, typed text and an input method's preedit arrive apart, and a stylus brings its pressure and
  every point it moved through.
- **State by id.** `civue_memory(ui, id, size, drop)` keeps a widget's state across frames, `civue_animate` eases a
  value and asks for frames while it moves.

## Layout

- **Nodes.** `div(...) { ... }` opens a node styled with gpui's names, and `text("...", ...)` adds a node of text. A style is a compound literal of designated initializers built at compile time, so a misspelled name or a step off the scale fails to compile, and a name given twice keeps the last as css does. `style(...)` takes any `civue_node_style` field as it is, like `style(.justify = dark ? CIVUE_PLACE_END : CIVUE_PLACE_START)`.
- **Blocks.** A node without `flex` or `grid` stacks its children like a css block, their widths filling it unless set, `mx(auto)` centering one. Vertical margins collapse as in css: between siblings, and through a block's top and bottom into its parent's when no padding or border holds them, unless it clips.
- **Flexbox.** `flex` with rows, columns and their reverses, wrapping, grow, shrink and basis, `gap`, `justify_*`, `items_*`, `self_*` and `content_*`, `auto` margins, min and max sizes and aspect ratios. `items_baseline` and `self_baseline` line items up on their first baselines, the box's own being its first item's. An item doesn't shrink under its content unless it clips, as in css.
- **Grid.** `grid` with `grid_cols(n)` and `grid_rows(n)`, every track 1fr as gpui's, `gap`, `col_span(n)`, `col_span_full`, `row_span(n)`, and `col_start`, `col_end`, `row_start` and `row_end` on lines counted from 1. Items without lines are placed row by row in the first cells free, rows past the explicit ones follow their content, and items stretch over their cells unless `items_*` or `self_*` say otherwise, baselines included.
- **Placing.** Sizes are border boxes. Padding, margins, `absolute` nodes placed by `top`, `right`, `bottom` and `left` in their parent's padding box, and `relative` for the rest.
- **Lengths.** gpui's scale: `p(4)` is a rem, 16 points unless `civue_set_rem_size` says otherwise, `p(0p5)` an eighth of one, `w(1_2)` half of the parent, then `w(full)`, `w(auto)` and `p(px)` for a single point. `w(px(200))`, `w(rems(1.5))` and `w(relative(.3))` take any value.
- **Text.** A text node inherits its size, line height, weight, color, alignment and wrapping from the nodes around it, and wraps to the width it gets, each line aligned on its own. `truncate` cuts it with an ellipsis and `whitespace_nowrap` keeps it on one line.
- **Paint.** `bg`, `border` and `border_color`, borders of their own width per side with `border_t` and its kin and rounded inner corners, `rounded`, `shadow` and `text_color`, with `hover(...)` and `active(...)` for the colors while the pointer is over a node or presses it, and `cursor_pointer` and its kin. Inside a node the app still paints with `fill` and the rest, over `here()` or `content()`, which is how the example's slider draws its knob.
- **Scrolling.** `overflow_y_scroll`, `overflow_x_scroll` and `overflow_scroll` make a node a scroll area over the content laid out in it, and `overflow_hidden` clips it. Like gpui's, a scroll area has no bars at all until the app designs them: a `civue_scrollbar_look` sets how wide they are, where their thumbs sit, how long they stay after a scroll and how they paint, for one node with `scrollbars(&look)` or for every scroll area with `ui->scrollbars`.
- **Widgets over nodes.** `button()`, `toggle(on)`, `slider(...)` and `text_field(...)` take the open node's id and rect, a slider's track being the node's content box.
- **Overlays.** A node opened in an `overlay()` block is a root over the window placed by its insets, which is how a tooltip goes anywhere, and `last_size(id)` gives the size to place it from.
- **Blocks end however they're left.** `div`, `clipped`, `overlay` and `scrolling` close at their end, a `break`, a `continue` or a `return` alike, through the cleanup attribute.
- **Measures are kept by what can change them.** A node of a set size is measured once however much room is around it, one with no fraction of its containing block once in any of them, and a flex item's least size only when it has to shrink.
- **One solve a frame.** A node paints and answers the pointer from where the last solve put it, as interaction already works from the last frame. `civue_end_frame` solves the whole tree once with each measure cached, and when anything moved the window runs the frame again before it shows, without the input, so nothing flickers and nothing is clicked twice. Steady frames solve once and the layout allocates nothing, its arrays reused from frame to frame: about 230 nanoseconds a node on Apple silicon with grids and baselines, painting included, growing linearly with the nodes.

## Doing less

- **Frames the window shows already are not drawn.** A frame's quads are compared with the last frame's, and one that draws the same over the same color at the same size is neither drawn nor presented: an app that asks for frames while nothing moves costs a tenth of a millisecond each. A frame drawing a texture the app registered itself is always drawn, since the app may have drawn into it.
- **Only what changed is drawn.** The comparison also tells which quads changed, marked on a grid of tiles a cache line wide. A hover, a caret or a typed letter draws a few tiles, on the cpu, into a surface of the window's own that a layer shows as it is, without a copy.
- **The gpu is left alone until it is needed.** Its driver keeps about 120 MB for as long as it is given work, however little, and for seconds after, and no call, public or private, makes it keep less. So the cpu draws every frame it can, scrolls and animations over the whole window included. A frame goes to the gpu when what changed holds an image, when it would take the fill of 32 million pixels, or when an animation's frames come late three times in a row with the drawing taking most of their time. An app without those never makes a Metal device. On Linux the frames go to `wl_shm` buffers the same way, and a Vulkan swapchain takes the surface for the frames that need the gpu, a texture the app registered among them, until two and a half seconds after the last one. An app without those never loads a Vulkan driver, and `CIVUE_DRAW=gpu` or `cpu` says who draws whatever the frame.
- **Three surfaces take turns.** The window server still reads the last frame while the next is drawn, so a frame goes into another surface, which remembers the tiles it missed while it waited and has them drawn again with the new ones. Tiles an opaque rect covers whole are not cleared first, and the rows about to be written are asked of memory four rows ahead, which takes a third off the time of fills scattered over a window. The surfaces not shown are freed two seconds after the last frame.
- **Rows.** `rows(count, row_height, i) { ... }` in a node that scrolls builds only the rows in view, two more past each end, each a node `row_height` tall known by its index. A hundred thousand rows cost what a screen of them does, and a row scrolled into view was laid out the frame before, so scrolling runs no frame twice. Rows are all of one height.
- **Memos.** `memo(id, inputs) { ... }` keeps the nodes its block builds, their layout, what they draw and what takes the pointer, and puts them back without running the block while `inputs` stays the same: a hash of everything the block reads, as `civue_hash` makes one. What is put back moves with what holds it, by whole pixels, so the rows of a scrolling list are built once.
- **A memo runs again by itself** when the pointer is over what it drew, when it holds the focus or what is pressed, on any key or button, when its block asked for another frame or scrolls, and when the text style, the scale, the look or the place around it changed. What only `inputs` can tell is what the block reads from the app: leave something out and the block shows what it showed.
- **What can't be skipped.** civue has no signals: a block is C code, and nothing tells what it read. A memo is where the app says so, and outside of them a frame is built whole: the 1,200 nodes of the benchmark's grid, 600 of them texts shaped anew, take under two milliseconds, their pixels included.

## Compared

`examples/bench` has the scenes of [MyGo's comparison](https://mygo.run/benchmarks) and speaks to its runner, `benchmarks/run` of [egoist/mygo](https://github.com/egoist/mygo), which measures the whole process from outside. These are medians of 5 runs of 600 frames on one Mac, an M4 Pro on macOS 27.2 with a 240 Hz display, each window brought to the front since one behind another is drawn a few times a second. Lower is better.

| | civue | MyGo 0.9.0 | GPUI 0.2.2 | GPUI Fast 0.1.13 | egui 0.37.0 |
| --- | --- | --- | --- | --- | --- |
| App size | 0.35 MB | 10.0 MB | 3.7 MB | 6.3 MB | 8.8 MB |
| Startup | 162 ms | 245 ms | 197 ms | 214 ms | 223 ms |
| Memory at rest | 22.7 MB | 40.5 MB | 44.2 MB | 38.9 MB | 62.3 MB |
| Counter, peak memory | 22.9 MB | 173 MB | 166 MB | 159 MB | 275 MB |
| List, CPU per frame | 0.37 ms | 0.79 ms | 0.64 ms | 0.44 ms | 0.77 ms |
| List, peak memory | 50.0 MB | 206 MB | 184 MB | 196 MB | 286 MB |
| Grid, CPU per frame | 1.61 ms | 5.91 ms | 16.8 ms | 2.14 ms | 5.64 ms |
| Grid, peak memory | 56.4 MB | 234 MB | 197 MB | 204 MB | 293 MB |
| Document, CPU per frame | 0.60 ms | 1.41 ms | 0.84 ms | 0.73 ms | 2.63 ms |
| Document, peak memory | 54.2 MB | 231 MB | 188 MB | 196 MB | 310 MB |

civue draws the four scenes on the cpu at the display's 240 frames a second, so the gpu's driver holds nothing and its 120 MB, most of every other figure, are not there. Its CPU figures count the pixels it fills, which the others leave to the gpu: the same scenes drawn by civue's gpu path take 0.24 ms, 1.00 ms and 0.41 ms a frame, at 170 MB. The counter's peak is what the runner reads for the process once it has rested, a figure the runner has but does not print.

The list's rows are in memos, as GPUI Fast's are cached. The grid and the document change every frame, so nothing is kept there: the grid's cost is 600 numbers shaped and drawn anew, and the document's is cutting 40 paragraphs into lines again. The runner says its CPU figures moved by a third between runs of one build, so small gaps are ties.

## Icons

- **Any iconify set.** Like unplugin-icons, an app names the icons it uses and the build fetches them: `icon(mdi, account)` in a node, or `CIVUE_ICON(lucide, sticky_note)` for a `civue_icon`, dashes written as underscores. Every set on [iconify](https://icon-sets.iconify.design) works, over 200 of them.
- **At build time.** `civue_icons(app)` in CMake scans the target's sources for the icons they name, comments left out, and adds the ones listed in `ICONS` for names the code builds itself. Each is fetched once from iconify's api into a cache shared by every project, `~/.cache/civue/icons` unless `CACHE` says otherwise, so later builds work offline and a project may keep its cache in the repository. The icons a set lacks come together through the set's json, a request for many, so a build that names a hundred new icons stays under iconify's rate limit. A busy or failing api is tried again a few times before the build stops, and the build says why it stopped: an icon iconify doesn't have, or what iconify answered. Only the icons used end up in the binary, embedded with `#embed`, and the ui starts with them.
- **Typos fail the build.** An icon iconify doesn't have stops the build with its name, and a name the build didn't generate doesn't compile.
- **The app's own.** `COLLECTIONS app=icons` makes `icon(app, logo)` read `icons/logo.svg`, rebuilt when the folder changes.
- **Colors.** An icon painted in `currentColor` alone takes the text color of its node, hover included, from the coverage atlas. Any other paint, like a logo's, keeps its own colors from the color atlas.
- **Sizes.** An icon node is as tall as its text unless sized, and as wide as its view box makes it, so `icon(lucide, plus)` sits in a line of text as in a browser. `size(5)`, `w` or `h` size it like any node.

```cmake
civue_icons(app ICONS mdi:home COLLECTIONS app=${CMAKE_CURRENT_SOURCE_DIR}/icons)
```

## Headless widgets

- **State, not pixels.** A widget is a call per frame taking an id and a rect, or those of the open node, and answering with a state: a
  button's `held` and `activated`, a slider's value and where it sits along its track, a scroll area's tracks and
  thumbs and how far its bars show, a group's choice. The app paints from that, before or after, with the painting
  helpers of `ui.h`.
- **Looks are callbacks.** What a widget can't answer in a struct, the app passes in: a menu measures and paints its
  card and rows through a `civue_menu_look`, a number field paints itself idle through a `civue_num_look`, a text
  field takes every color of a `civue_field_look`, rich text a `civue_rich_colors`. None has a default.
- **The keys.** `ui->focus` is the widget the keys go to: enter and space activate a button, the arrows step a
  slider or move a group's choice, a menu runs on the arrows and enter. A press doesn't move the focus, the app
  does, and `ui->ring` with `civue_press_ring` is its keyboard navigation's ring. Text fields take the focus when
  pressed, as editors do.
- **Light or dark.** `ui->appearance` asks for the system's look, light or dark, and `ui->dark` says which the frame
  has. The window's title bar follows it, and the desktop's setting arrives through the settings portal on Linux.
- **Faces.** Text is set in the system's ui font and its monospace one, Core Text's on macOS and fontconfig's
  `sans-serif` and `monospace` on Linux, until `civue_text_set_face` sets the app's own from a font file.

## How it draws

- **One pass of instanced quads.** Rounded rects with borders, gaussian shadows, antialiased lines, glyph masks,
  color glyphs, images and pixel grids are all one quad layout, drawn in order in a single render pass and batched
  by the image they sample. A frame's quads go to one of three buffers kept for reuse, so steady frames allocate
  nothing on the gpu.
- **Atlases.** Glyphs and icons go into 2048 pixel atlases packed in shelves. On macOS their pixels are memory the gpu reads in place, so nothing is uploaded and nothing is kept twice. Colors blend on srgb encoded values like browsers do.
- **On the cpu.** `civue_raster` draws a list as the shader does: rects and shadows are filled in rows but for their curved corners, which ask the shader's math a pixel at a time, translucent rows are blended four pixels at a time and glyphs sixteen. That is NEON on arm64, macOS and Linux alike. On x86 the same paths are SSE2, eight pixels at a time where the build has AVX2, and a clear or an opaque rect larger than the caches near a core is written past them, which arm64 cores do by themselves. A scaled image is read between its texels four channels at a time on both. Its frames differ from the gpu's by two steps of a color at most, on a few dozen pixels of a window.
- **Text.** Core Text lays text out on macOS, HarfBuzz, FriBidi and fontconfig on Linux, FreeType rasterizes there.
  Layouts are cached per text, style and scale and forgotten after 600 frames unused, and once there are thousands of them after 4 on macOS and 30 on Linux. Text wrapped to a new width is not shaped again: on macOS plain ascii is cut from the line it was set on, where Core Text would break it, and its wrapped blocks go two frames after they were last asked. A block remembers where its glyphs are in the atlas, and they go to the list in one run. A line of plain ascii is not given to Core Text's lines at all on macOS: its glyphs and advances are kept per font and placed by one private call, `CTFontShapeGlyphs`, six times faster. The first texts of a font, then one in 256, are held against the line Core Text makes, and the font goes back to lines for good once they differ or where the call is missing. Glyphs are rasterized at four
  subpixel positions on demand.
- **Vulkan.** Vulkan 1.3 with push descriptors and timeline semaphores. The ui and the app's renderers get queues
  of their own when the device has two, `civue_vk_handoff` passes textures from one to the other, and memory is
  carved out of 64 MiB blocks.
- **Your own types.** An app with its own point, rect, color or array types of the same members defines
  `CIVUE_VEC2`, `CIVUE_RECT`, `CIVUE_COLOR`, `CIVUE_BYTES`, `CIVUE_U32S` or `CIVUE_STRS` as them before including
  civue, and passes them straight through, which is how vektra keeps its own names. `CIVUE_UI_FIELDS` adds fields of
  its own at the end of `civue_ui`, its theme for one, and `civue_ui_new` and `civue_window_run` size the ui with
  them.
- **Your own shaders.** `civue_embed_shaders(target PREFIX name SOURCES ...)` in CMake compiles `.metal` files to
  metallibs on macOS and `.vert`, `.frag` and `.comp` ones to spir-v on Linux, and embeds them with `#embed`.

## The window

- **macOS.** An `NSWindow` with a `CAMetalLayer` and a layer over it for the cpu's frames, written in Objective-C. The first frame is drawn before the app runs and the window is sent to the window server with it, tens of milliseconds before the app has finished launching. An app's own title
  bar goes under a transparent system one with the window's material showing through, and comes back inside the
  system's bar in full screen. Input methods go through `NSTextInputClient`, files dropped on the window arrive in
  `civue_input.drops`, and `install_menus` builds the menu bar.
- **Linux.** Wayland only: xdg shell with client side decorations drawn by the app when it has a title bar,
  fractional scaling with viewporter, text-input-v3 for input methods, tablets, pinch gestures, cursor shapes,
  xdg-activation and xdg-foreign for portals. Its rounded corners are cut out of the frame, on the cpu or the gpu. The
  desktop's dark mode and button layout come from the settings portal. `civue_watch_fd` drives another event loop
  from civue's, like libdispatch's main queue.
- **Cursors.** System cursors by kind, and the app's own images by kind and angle through
  `civue_window_options.cursor_image`, made once at each scale.

## Web views

- **A page over a rect.** `civue_webview_new` makes a web view of the system's, a `WKWebView` on macOS and WPE WebKit on Linux, and `civue_webview_show(ui, w, r)` shows it over `r` for the frame, as a widget is called: a frame that does not call it hides it, and the keys go back to the window when the page had them. The app paints what is around it, a card or a spinner, and with `transparent` the ui shows through where the page paints nothing.
- **Messages.** A page calls `window.civue.postMessage(text)`, the same on every platform, and the app's `message` gets the text on the main thread. `civue_webview_eval` runs javascript in the page for the other way. Only the page's main frame is heard, not the frames it embeds.
- **Its own origin.** `civue_webview_load_html` takes the origin the page believes it is on, which is what a captcha or a login widget checks, and cookies are kept between runs as a browser keeps them.
- **Links leave.** A link to a new window, or one the user clicks to another site, opens in the system's browser.

```c
static void message(void *ctx, const char *text) { civue_window_wake(); }     // on the main thread

civue_webview *web = civue_webview_new(&(civue_webview_options){.message = message, .transparent = true});
civue_webview_load_html(web, "<button onclick=\"civue.postMessage('hi')\">Hi</button>", "https://example.com");
civue_webview_show(ui, web, civue_rect_xywh(24, 24, 400, 600));                // each frame it is wanted
```

## Memory

civue allocates what it keeps to itself through [cilo](../ciko-cilo): the ui, the text system and its layouts, the
icon registry and widget state. What it hands over comes from `malloc` and is the caller's to `free`: strings read
from the clipboard, paths, text field values and the arrays in `civue_input`. Arrays are `CIVUE_VEC(T)`,
`{v, len, cap}` grown with `realloc`, so the app may keep growing them.

On Linux the system's font files are mapped, not read: a CJK or an emoji face is tens of megabytes, of which a line
of text touches a few pages. What frames free goes back to the system once the window is about to wait a quarter of
a second or more, ten seconds apart at most, where glibc would keep it in its heap.

## Building

It depends on cilo, fetched by CMake unless `FETCHCONTENT_SOURCE_DIR_CILO` points at a checkout. C23 with the GNU
extensions, clang 19 or later or Apple clang 21, CMake 3.29 and Ninja.

- **macOS 14 or later**, Apple silicon, with Xcode's Metal toolchain: `xcodebuild -downloadComponent MetalToolchain`.
- **Linux** with glslc, Vulkan, Wayland with wayland-scanner, xkbcommon, FreeType, HarfBuzz, fontconfig, FriBidi,
  librsvg, cairo, GIO and enchant. GStreamer plays videos and WPE WebKit shows web views where they are installed,
  both are left out otherwise, and each is loaded by the first video or web view an app makes, never at launch.
  Vulkan is only loaded once a frame needs the gpu. PNG, JPEG, WebP and GIF images are decoded in the process by
  libpng, libjpeg-turbo 3 and libwebp where they are installed, with Little CMS for the ones in a color profile of
  their own. gdk-pixbuf reads the other formats, and those too without the libraries. Set `CC=clang`, CMake picks
  gcc otherwise. On Ubuntu:

```sh
sudo apt install clang cmake ninja-build pkg-config glslc libvulkan-dev libwayland-dev libwayland-bin \
  libxkbcommon-dev libfreetype-dev libharfbuzz-dev libfontconfig-dev libfribidi-dev librsvg2-dev libcairo2-dev \
  libglib2.0-dev libenchant-2-dev libpng-dev libturbojpeg0-dev libwebp-dev liblcms2-dev \
  libgstreamer1.0-dev libgstreamer-plugins-base1.0-dev libwpewebkit-2.0-dev
```

On Arch:

```sh
sudo pacman -S clang cmake ninja pkgconf shaderc vulkan-headers vulkan-icd-loader wayland xkbcommon freetype2 \
  harfbuzz fontconfig fribidi librsvg cairo glib2 enchant libpng libjpeg-turbo libwebp lcms2 \
  gstreamer gst-plugins-base gst-plugins-good gst-libav wpewebkit
```

Linux is x86-64 or arm64. A build for the machine it runs on adds `-DCMAKE_C_FLAGS=-march=native`, `-mcpu=native`
on arm64: the cpu draws the frames on Linux, with AVX2 where an x86-64 build may count on it and NEON on arm64.

```sh
cmake -S . -B build -G Ninja -DFETCHCONTENT_SOURCE_DIR_CILO=../ciko-cilo
ninja -C build
build/examples/hello/hello
build/examples/layout/layout
build/examples/bench/bench list
```

An example builds on its own too, as an app would, against this checkout:

```sh
cmake -S examples/layout -B build-layout -G Ninja -DFETCHCONTENT_SOURCE_DIR_CILO=$PWD/../ciko-cilo
ninja -C build-layout
```

```cmake
target_link_libraries(app PRIVATE civue::civue)
```

## Limits

- One window per process for now, and Wayland only on Linux.
- On Linux a web view's page is painted by WPE WebKit into memory and drawn in the frame as an image, so the app's overlays go over it. It takes the pointer and the keys the frame gets while it has the focus, without input methods, and nothing is dragged out of it. On macOS one is a view of the system's over the frame, so nothing civue draws goes over it, menus and tooltips included, and a transparent one asks `WKWebView` not to paint its background through a key that is not public.
- On Linux, overlays that go past the window's edges are drawn in an `xdg_popup` over the screen. Wayland does not tell a window where it is, so civue finds the screen's corners from two popups the compositor slides back onto it, never shown. A compositor that does not slide popups keeps the overlays in the window. Layer surfaces always do.
- A video on Linux is decoded to rgba by GStreamer and uploaded each picture, on the cpu, which a 4K one is slow for.
- On macOS a frame whose changes hold an image is drawn by the gpu, as are the frames of a window in full screen with its own title bar, and the driver's 120 MB come with them until two and a half seconds after the last.
- `CTFontShapeGlyphs` is private to Core Text and found by name at run time, so a store review may refuse an app for it.
- Menus keep 8 points around their rows: geometry the pointer depends on, set by civue while the app paints them.
- Grid tracks are all 1fr, as gpui's are: no fixed or auto sized explicit tracks, no named lines or areas, and no dense packing. Clips are rectangles, so content inside a rounded node clips square at its corners.
