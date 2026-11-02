# civue

*A window and a frame.* A native GUI toolkit for C23, part of CiKo: Metal and AppKit on macOS, Vulkan and Wayland
on Linux, no other toolkit underneath.

```c
#include <civue/dsl.h>

typedef struct {
  int count;
  char *name;
} Hello;

static void frame(void *ctx, civue_ui *ui) {                   // runs when input, a timer or an animation asks
  Hello *h = ctx;
  civue_floating(ui, xywh(24, 24, 320, 180), CIVUE_RADIUS_LARGE, ui->theme.bg, CIVUE_E200);
  label(ui, "Hello from civue", CIVUE_HEADING_MEDIUM, pt(48, 44), ui->theme.text);
  if (button(ui, id("add"), xywh(48, 84, 96, 28), "Click me", CIVUE_BUTTON_PRIMARY).clicked)
    h->count++;
  text_field(ui, id("name"), xywh(48, 128, 200, 28), &h->name, "Your name", CIVUE_BODY);
  clipped(ui, xywh(48, 170, 200, 20)) { ... }                  // a block, what it draws is clipped
}

static civue_app make(civue_ui *ui, void *arg) { return (civue_app){arg, frame}; }

int main(void) {
  Hello hello = {};
  civue_window_options o = {.title = "Hello", .width = 368, .height = 228};
  civue_window_run(&o, make, &hello);                         // returns when the window closes
}
```

`examples/hello.c` is the whole app, with a theme switch, a slider and a tooltip. It builds with the tests.

| Dear ImGui | civue |
| --- | --- |
| `ImGui::NewFrame`, `ImGui::Render` | `civue_begin_frame`, `civue_end_frame`, or `civue_window_run` doing both |
| `ImGui::GetID("x")`, `PushID` | `civue_id_of("x")`, `civue_ids(parent, "x")`, `civue_idn(parent, n)` |
| `ImGui::Button`, `Checkbox`, `SliderFloat` | `civue_button`, `civue_checkbox`, `civue_slider` |
| `ImGui::InputText` | `civue_text_field`, `civue_text_area`, with input methods and the clipboard |
| `ImGui::BeginPopup`, `BeginMenu` | `civue_menu`, `civue_menu_path` with submenus, `civue_select_menu` |
| `ImGui::SetTooltip` | `civue_tooltip`, shown after half a second of hover |
| `ImGui::BeginChild` with scrolling | `CIVUE_SCROLLING(ui, id, r, height, content) { ... }` |
| `ImDrawList` | `civue_draw_list`, instanced quads with clips and turns |
| `ImGui::GetStateStorage` | `civue_memory(ui, id, size, drop)`, forgotten with `civue_forget` |
| a backend per platform | built in: `civue_window_run` on macOS and Wayland |

The short names come from `<civue/dsl.h>`: `id`, `rgb`, `pt`, `xywh`, the blocks `clipped`, `overlay` and
`scrolling`, and a few widgets. They are function like macros, so a variable called `label` still works, and
`<civue/dsl_end.h>` drops them. `<civue/civue.h>` alone has only the `civue_` and `CIVUE_` names.

## Modules

| header | what it has |
| --- | --- |
| `base.h` | points, rects and colors, growable arrays, hashing, utf-8, the clock |
| `gpu.h` | the process's gpu device, `objc.h` and `metal.h` on macOS, `vulkan.h` on Linux |
| `draw.h`, `gfx.h` | draw lists and atlases, and the renderer drawing them, offscreen too with `civue_gfx_snapshot` |
| `text.h` | layout and rendering, Inter and Roboto Mono built in, faces registered while running |
| `icons.h`, `theme.h` | svg icons tinted from an atlas, the light, dark and high contrast themes and the type scale |
| `ui.h` | frames, input, ids, interaction, focus, animation, painting snapped to pixels |
| `widgets.h`, `field.h`, `numfield.h`, `rich.h` | controls, text fields, number fields, rich text editing |
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
  civue, and passes them straight through, which is how vektra keeps its own names.
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
ctest --test-dir build                                         # needs a gpu, a software one does
build/example_hello
```

```cmake
target_link_libraries(app PRIVATE civue::civue)
```

The tests draw offscreen and read the pixels back: the renderer, text layout and drawing, interaction and ids, and
the widgets with a click, a checkbox and typing. They run on Metal and on Mesa's lavapipe in Docker.

## Limits

- One window per process for now, and Wayland only on Linux.
- The widgets look like figma's ui3, the theme struct changes their colors but not their shapes.
- Inter and Roboto Mono are under the SIL Open Font License, see `assets/fonts/LICENSE.txt`. The tooltip arrows
  come from Phosphor, MIT licensed.
