#include "win.h"

#include <civue/system.h>

#include <initguid.h>

#include <spellcheck.h>

// the system's checker, of the language in use, made on first use.
static struct {
  SRWLOCK lock;
  bool tried;
  ISpellCheckerFactory *factory;
  ISpellChecker *checker;
  // a tag like "en-US".
  wchar_t *language;
} S = {.lock = SRWLOCK_INIT};

static bool supported(const wchar_t *tag) {
  BOOL yes = false;
  return S.factory &&
         SUCCEEDED(ISpellCheckerFactory_IsSupported(S.factory, tag, &yes)) &&
         yes;
}

// the user's language, english, or any the system has.
static wchar_t *default_language() {
  wchar_t user[LOCALE_NAME_MAX_LENGTH];

  if (GetUserDefaultLocaleName(user, countof(user)) && supported(user)) {
    return _wcsdup(user);
  }

  if (supported(L"en-US")) {
    return _wcsdup(L"en-US");
  }

  IEnumString *all = nullptr;
  wchar_t *first = nullptr, *any = nullptr;

  if (SUCCEEDED(ISpellCheckerFactory_get_SupportedLanguages(S.factory, &all))) {
    if (IEnumString_Next(all, 1, &first, nullptr) == S_OK && first) {
      any = _wcsdup(first);
      CoTaskMemFree(first);
    }

    IEnumString_Release(all);
  }

  return any;
}

// the checker, its lock held. null without one.
static ISpellChecker *checker() {
  if (!S.tried) {
    S.tried = true;
    civue__com_init();
    CoCreateInstance(&CLSID_SpellCheckerFactory, nullptr, CLSCTX_INPROC_SERVER,
                     &IID_ISpellCheckerFactory, (void **)&S.factory);
  }

  if (!S.factory) {
    return nullptr;
  }

  if (!S.language) {
    S.language = default_language();
  }

  if (!S.checker && S.language) {
    ISpellCheckerFactory_CreateSpellChecker(S.factory, S.language, &S.checker);
  }

  return S.checker;
}

civue_u32s civue_spell_misspelled(const char *text) {
  civue_u32s out = {};

  if (!text || !*text) {
    return out;
  }

  civue__com_init();
  wchar_t *w = civue__wide(text, -1);
  usize n = wcslen(w);

  // the code point each code unit is in.
  u32 *cp = xmalloc((n + 1) * sizeof *cp);
  u32 at = 0;

  for (usize i = 0; i < n; i++) {
    cp[i] = at;

    if (IS_HIGH_SURROGATE(w[i]) && i + 1 < n && IS_LOW_SURROGATE(w[i + 1])) {
      cp[++i] = at;
    }

    at++;
  }

  cp[n] = at;

  AcquireSRWLockExclusive(&S.lock);
  ISpellChecker *c = checker();
  IEnumSpellingError *errors = nullptr;

  if (c && SUCCEEDED(ISpellChecker_Check(c, w, &errors))) {
    ISpellingError *e = nullptr;

    while (IEnumSpellingError_Next(errors, &e) == S_OK && e) {
      ULONG start = 0, len = 0;
      CORRECTIVE_ACTION action = CORRECTIVE_ACTION_NONE;
      ISpellingError_get_StartIndex(e, &start);
      ISpellingError_get_Length(e, &len);
      ISpellingError_get_CorrectiveAction(e, &action);
      ISpellingError_Release(e);
      e = nullptr;

      // a word repeated is deleted, not misspelled.
      bool wrong = action == CORRECTIVE_ACTION_GET_SUGGESTIONS ||
                   action == CORRECTIVE_ACTION_REPLACE;

      if (wrong && len && start + len <= n) {
        vec_push(&out, cp[start]);
        vec_push(&out, cp[start + len]);
      }
    }

    IEnumSpellingError_Release(errors);
  }

  ReleaseSRWLockExclusive(&S.lock);
  free(cp);
  free(w);
  return out;
}

civue_strs civue_spell_guesses(const char *text, uint32_t a, uint32_t b) {
  civue_strs out = {};

  if (!text || b <= a) {
    return out;
  }

  usize n = strlen(text), i = 0, from = n, to = n;

  for (u32 cp = 0; i <= n; cp++) {
    if (cp == a) {
      from = i;
    }

    if (cp == b) {
      to = i;
      break;
    }

    if (i == n) {
      break;
    }

    utf8_next(text, n, &i);
  }

  if (from >= to) {
    return out;
  }

  civue__com_init();
  wchar_t *word = civue__wide(text + from, (int)(to - from));

  AcquireSRWLockExclusive(&S.lock);
  ISpellChecker *c = checker();
  IEnumString *list = nullptr;

  // s_false is a word it has no guesses for.
  if (c && ISpellChecker_Suggest(c, word, &list) == S_OK && list) {
    wchar_t *s = nullptr;

    while (IEnumString_Next(list, 1, &s, nullptr) == S_OK && s) {
      vec_push(&out, civue__utf8(s, -1));
      CoTaskMemFree(s);
      s = nullptr;
    }
  }

  if (list) {
    IEnumString_Release(list);
  }

  ReleaseSRWLockExclusive(&S.lock);
  free(word);
  return out;
}

// windows' tags are like "en-US", civue's like "en_US".
static void swap_dashes(char *s, char from, char to) {
  for (; *s; s++) {
    if (*s == from) {
      *s = to;
    }
  }
}

char *civue_spell_language() {
  civue__com_init();
  AcquireSRWLockExclusive(&S.lock);
  checker();

  char *code = S.language ? civue__utf8(S.language, -1) : xstrdup("en-US");
  ReleaseSRWLockExclusive(&S.lock);

  swap_dashes(code, '-', '_');
  return code;
}

void civue_spell_set_language(const char *code) {
  if (!code || !*code) {
    return;
  }

  char *tag = xstrdup(code);
  swap_dashes(tag, '_', '-');

  wchar_t *w = civue__wide(tag, -1);
  free(tag);

  civue__com_init();
  AcquireSRWLockExclusive(&S.lock);
  checker();

  if (supported(w)) {
    if (S.checker) {
      ISpellChecker_Release(S.checker);
    }

    S.checker = nullptr;
    free(S.language);
    S.language = w;
    w = nullptr;
  }

  ReleaseSRWLockExclusive(&S.lock);
  free(w);
}
