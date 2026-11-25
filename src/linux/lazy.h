#ifndef CIVUE_LINUX_LAZY_H
#define CIVUE_LINUX_LAZY_H

#include "../internal.h"

/* ========================================================================== */
/* libraries taken when they are first used, not when the app starts. a web   */
/* view's brings a hundred others with it, each mapped, relocated and looked  */
/* through for every symbol any library asks for, in an app that may show a   */
/* web view once a week.                                                      */
/*                                                                            */
/* a function of such a library keeps its name and its header: it is defined  */
/* here as a jump through a slot, which civue__lazy_load fills from the       */
/* library. one that is called and not listed fails the link.                 */
/* ========================================================================== */

#if defined(__x86_64__)
#define CIVUE__LAZY_JUMP(name) \
  "  endbr64\n  jmp *civue__lazy_" #name "(%rip)\n"
#elif defined(__aarch64__)
// x16 is the linker's own between a call and what it calls. the hint is
// bti c.
#define CIVUE__LAZY_JUMP(name)                    \
  "  hint #34\n  adrp x16, civue__lazy_" #name "\n" \
  "  ldr x16, [x16, :lo12:civue__lazy_" #name "]\n  br x16\n"
#else
#error "civue: no lazy jump for this processor"
#endif

// defines `name` as a jump to where its slot says.
#define CIVUE_LAZY_FUNCTION(name)                                       \
  [[gnu::visibility("hidden"), gnu::used]] void *civue__lazy_##name;    \
  __asm__(".pushsection .text\n"                                        \
          ".hidden " #name "\n.globl " #name "\n.type " #name           \
          ", %function\n" #name ":\n" CIVUE__LAZY_JUMP(name) ".size "   \
          #name ", . - " #name "\n.popsection\n");

typedef struct {
  const char *name;
  void **slot;
} civue__lazy_symbol;

#define CIVUE_LAZY_SYMBOL(name) {#name, &civue__lazy_##name},

// opens `libraries`, a null ended list of sonames, and fills the slots of
// `symbols` from them. false when a library or a function is missing, none
// of the functions may be called then.
bool civue__lazy_load(const char *const *libraries,
                      const civue__lazy_symbol *symbols, usize count);

#endif
