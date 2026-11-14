#ifndef CIVUE_SYSTEM_H
#define CIVUE_SYSTEM_H

#include <civue/base.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ==========================================================================================================
 */
/* the clipboard. what is read is malloc'd and the caller's. */
/* ==========================================================================================================
 */

// its text, nullptr when it holds none.
char *civue_clipboard_text(void);
// its html, nullptr when it holds none.
char *civue_clipboard_html(void);
void civue_clipboard_set_text(const char *s, size_t n);
// html for apps that read it, with `text` for the others.
void civue_clipboard_set_html(const char *html, const char *text);
// an image, png bytes.
void civue_clipboard_set_png(const uint8_t *png, size_t n);

// turns image bytes the app does not read into png or jpeg ones, their mime
// type set, false when it can't.
typedef bool (*civue_image_convert)(const civue_bytes *in, civue_bytes *out,
                                    const char **mime);

// an image copied in another app: png, jpeg, gif or webp bytes as they are,
// then heic, avif or tiff ones through `convert`, or as they are when it is
// null. `out` and `mime` are the caller's, empty when there is none.
bool civue_pasteboard_image(civue_bytes *out, char **mime,
                            civue_image_convert convert);
// the absolute paths of files copied in the file manager, the caller's.
civue_strs civue_pasteboard_files(void);
// whether the clipboard holds anything, of any kind.
bool civue_pasteboard_holds_other(void);

/* ==========================================================================================================
 */
/* file panels and spell checking. */
/* ==========================================================================================================
 */

// the system's panel picking files to open of the given extensions, or any when
// `n` is 0, waited for with the window still drawn. absolute paths, the
// caller's, empty on cancel.
civue_strs civue_file_panel_open(const char *const *extensions, uint32_t n,
                                 bool multiple);

// the misspelled words of `text` as code point ranges, start then end.
civue_u32s civue_spell_misspelled(const char *text);
// the checker's guesses for the word at code points a to b, the caller's.
civue_strs civue_spell_guesses(const char *text, uint32_t a, uint32_t b);
// the language checked, like "en_US", malloc'd.
char *civue_spell_language(void);
void civue_spell_set_language(const char *code);

#ifdef __cplusplus
}
#endif

#endif
