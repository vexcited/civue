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

`examples/hello` is the whole app, with a dark switch, a slider and a tooltip, painting its own look without a single coordinate written by hand. `examples/layout` shows grids, baselines, collapsing margins and a list with scroll bars of its own. Each example is a CMake project of its own, fetching civue as an app would.

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
| `text.h` | layout and rendering in the system's faces, or faces the app sets from font files |
| `icons.h` | svg icons from any iconify set or the app's own, tinted or in their own colors |
| `ui.h` | frames, input, ids, interaction, focus, the light or dark appearance, animation, painting snapped to pixels |
| `widgets.h`, `field.h`, `numfield.h`, `rich.h` | headless controls, text fields, number fields, rich text editing |
| `layout.h` | nodes laid out like css blocks, flexbox and grid, painted like gpui's divs |
| `window.h` | the app, the window and the main loop, the keyboard and input methods, cursors, menus |
| `system.h` | the clipboard and what other apps copy, file panels, spell checking |

The modules stack in that order and each only uses the ones above it. An app that draws its own way can stop at
`gfx.h`: `civue_gfx_new(civue_gpu_device())` and `civue_gfx_encode` into its own command buffers.

## How a frame works

- **Frames on demand.** The window builds a frame when input arrives, when `civue_request_repaint` or
  `civue_request_repaint_after` asked, or while an animation runs, then sleeps. On macOS a `CAMetalDisplayLink`
  drives it a frame ahead and pauses after eight idle refreshes. On Wayland the compositor's frame callback paces
  it, two frames in flight. Live resizes draw each size inside the resize's transaction, so nothing stretches.
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
- **One solve a frame.** A node paints and answers the pointer from where the last solve put it, as interaction already works from the last frame. `civue_end_frame` solves the whole tree once with each measure cached, and when anything moved the window runs the frame again before it shows, without the input, so nothing flickers and nothing is clicked twice. Steady frames solve once and the layout allocates nothing, its arrays reused from frame to frame: about 230 nanoseconds a node on Apple silicon with grids and baselines, painting included, growing linearly with the nodes.

## Icons

- **Any iconify set.** Like unplugin-icons, an app names the icons it uses and the build fetches them: `icon(mdi, account)` in a node, or `CIVUE_ICON(lucide, sticky_note)` for a `civue_icon`, dashes written as underscores. Every set on [iconify](https://icon-sets.iconify.design) works, over 200 of them.
- **At build time.** `civue_icons(app)` in CMake scans the target's sources for the icons they name, comments left out, and adds the ones listed in `ICONS` for names the code builds itself. Each is fetched once from iconify's api into a cache shared by every project, `~/.cache/civue/icons` unless `CACHE` says otherwise, so later builds work offline and a project may keep its cache in the repository. Only the icons used end up in the binary, embedded with `#embed`, and the ui starts with them.
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
  by the image they sample. A frame's quads go to one of four buffers kept for reuse, so steady frames allocate
  nothing on the gpu.
- **Atlases.** Glyphs and icons go into 2048 pixel atlases packed in shelves, uploaded in one blit before the pass.
  Colors blend on srgb encoded values like browsers do.
- **Text.** Core Text lays text out on macOS, HarfBuzz, FriBidi and fontconfig on Linux, FreeType rasterizes there.
  Layouts are cached per text, style and scale and forgotten after 600 frames unused. Glyphs are rasterized at four
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

- **macOS.** An `NSWindow` with a `CAMetalLayer`, written in C through the objective-c runtime. An app's own title
  bar goes under a transparent system one with the window's material showing through, and comes back inside the
  system's bar in full screen. Input methods go through `NSTextInputClient`, files dropped on the window arrive in
  `civue_input.drops`, and `install_menus` builds the menu bar.
- **Linux.** Wayland only: xdg shell with client side decorations drawn by the app when it has a title bar,
  fractional scaling with viewporter, text-input-v3 for input methods, tablets, pinch gestures, cursor shapes,
  xdg-activation and xdg-foreign for portals. Its rounded corners are cut out of the swapchain image. The
  desktop's dark mode and button layout come from the settings portal. `civue_watch_fd` drives another event loop
  from civue's, like libdispatch's main queue.
- **Cursors.** System cursors by kind, and the app's own images by kind and angle through
  `civue_window_options.cursor_image`, made once at each scale.

## Memory

civue allocates what it keeps to itself through [cilo](../ciko-cilo): the ui, the text system and its layouts, the
icon registry and widget state. What it hands over comes from `malloc` and is the caller's to `free`: strings read
from the clipboard, paths, text field values and the arrays in `civue_input`. Arrays are `CIVUE_VEC(T)`,
`{v, len, cap}` grown with `realloc`, so the app may keep growing them.

## Building

It depends on cilo, fetched by CMake unless `FETCHCONTENT_SOURCE_DIR_CILO` points at a checkout. C23 with the GNU
extensions, clang 19 or later or Apple clang 21, CMake 3.29 and Ninja.

- **macOS 14 or later**, Apple silicon, with Xcode's Metal toolchain: `xcodebuild -downloadComponent MetalToolchain`.
- **Linux** with glslc, Vulkan, Wayland with wayland-scanner, xkbcommon, FreeType, HarfBuzz, fontconfig, FriBidi,
  librsvg, cairo, GIO and enchant. On Ubuntu:

```sh
sudo apt install clang cmake ninja-build pkg-config glslc libvulkan-dev libwayland-dev libwayland-bin \
  libxkbcommon-dev libfreetype-dev libharfbuzz-dev libfontconfig-dev libfribidi-dev librsvg2-dev libcairo2-dev \
  libglib2.0-dev libenchant-2-dev
```

```sh
cmake -S . -B build -G Ninja -DFETCHCONTENT_SOURCE_DIR_CILO=../ciko-cilo
ninja -C build
build/examples/hello/hello
build/examples/layout/layout
```

An example builds on its own too, as an app would, here against this checkout:

```sh
cmake -S examples/layout -B build-layout -G Ninja -DFETCHCONTENT_SOURCE_DIR_CIVUE=$PWD \
  -DFETCHCONTENT_SOURCE_DIR_CILO=$PWD/../ciko-cilo
ninja -C build-layout
```

```cmake
target_link_libraries(app PRIVATE civue::civue)
```

## Limits

- One window per process for now, and Wayland only on Linux.
- Menus keep 8 points around their rows: geometry the pointer depends on, set by civue while the app paints them.
- Grid tracks are all 1fr, as gpui's are: no fixed or auto sized explicit tracks, no named lines or areas, and no dense packing. Clips are rectangles, so content inside a rounded node clips square at its corners.
