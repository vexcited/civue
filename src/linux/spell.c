#include "../internal.h"

#include <civue/system.h>

#include <ctype.h>
#include <enchant.h>
#include <locale.h>
#include <pthread.h>
#include <string.h>
#include <wctype.h>

// enchant's dictionaries, hunspell's and the others it finds, one in use.
static struct {
  pthread_mutex_t lock;
  EnchantBroker *broker;
  EnchantDict *dict;
  char *language;
  locale_t ctype;
} S = {.lock = PTHREAD_MUTEX_INITIALIZER};

static void first_dict(const char *const lang_tag, const char *const name,
                       const char *const desc, const char *const file,
                       void *data) {
  char **out = data;
  if (!*out) {
    *out = xstrdup(lang_tag);
  }
}

// the language of the environment, like "en_US", or any installed one.
static char *default_language() {
  const char *env = getenv("LC_MESSAGES");
  if (!env || !*env) {
    env = getenv("LANG");
  }
  if (env && *env && strcmp(env, "C") && strcmp(env, "POSIX")) {
    usize n = strcspn(env, ".@");
    char *code = xstrndup(env, n);
    if (enchant_broker_dict_exists(S.broker, code)) {
      return code;
    }
    // the language alone, en for en_GB.
    code[strcspn(code, "_")] = 0;
    if (enchant_broker_dict_exists(S.broker, code)) {
      return code;
    }
    free(code);
  }
  if (enchant_broker_dict_exists(S.broker, "en_US")) {
    return xstrdup("en_US");
  }
  char *any = nullptr;
  enchant_broker_list_dicts(S.broker, first_dict, &any);
  return any;
}

// the checker, made on first use, its lock held. nullptr without one.
static EnchantDict *dict() {
  if (!S.broker) {
    S.broker = enchant_broker_init();
    S.ctype = newlocale(LC_CTYPE_MASK, "C.UTF-8", (locale_t)0);
    if (!S.broker) {
      return nullptr;
    }
  }
  if (!S.dict) {
    if (!S.language) {
      S.language = default_language();
    }
    if (S.language) {
      S.dict = enchant_broker_request_dict(S.broker, S.language);
    }
  }
  return S.dict;
}

static bool word_char(u32 c) {
  return S.ctype ? iswalpha_l((wint_t)c, S.ctype) : (c < 128 && isalpha(c));
}

civue_u32s civue_spell_misspelled(const char *text) {
  civue_u32s out = {};
  if (!text || !*text) {
    return out;
  }
  pthread_mutex_lock(&S.lock);
  EnchantDict *d = dict();
  if (d) {
    usize n = strlen(text), i = 0;
    u32 cp = 0;
    while (i < n) {
      usize start = i;
      u32 first = cp, c = utf8_next(text, n, &i);
      cp++;
      if (!word_char(c)) {
        continue;
      }
      // letters, with apostrophes between them like "don't".
      usize end = i;
      u32 last = cp;
      while (i < n) {
        usize at = i;
        u32 k = utf8_next(text, n, &i);
        bool apostrophe = k == '\'' || k == 0x2019;
        if (!word_char(k) && !apostrophe) {
          i = at;
          break;
        }
        cp++;
        if (!apostrophe) {
          end = i, last = cp;
        }
      }
      i = end;
      cp = last;
      if (enchant_dict_check(d, text + start, (ssize_t)(end - start)) > 0) {
        vec_push(&out, first);
        vec_push(&out, last);
      }
    }
  }
  pthread_mutex_unlock(&S.lock);
  return out;
}

civue_strs civue_spell_guesses(const char *text, u32 a, u32 b) {
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
  pthread_mutex_lock(&S.lock);
  EnchantDict *d = dict();
  if (d) {
    size_t count = 0;
    char **list =
      enchant_dict_suggest(d, text + from, (ssize_t)(to - from), &count);
    for (size_t k = 0; k < count; k++) {
      vec_push(&out, xstrdup(list[k]));
    }
    if (list) {
      enchant_dict_free_string_list(d, list);
    }
  }
  pthread_mutex_unlock(&S.lock);
  return out;
}

char *civue_spell_language() {
  pthread_mutex_lock(&S.lock);
  dict();
  char *code = xstrdup(S.language ? S.language : "en_US");
  pthread_mutex_unlock(&S.lock);
  return code;
}

void civue_spell_set_language(const char *code) {
  pthread_mutex_lock(&S.lock);
  dict();
  if (S.broker && enchant_broker_dict_exists(S.broker, code)) {
    if (S.dict) {
      enchant_broker_free_dict(S.broker, S.dict);
    }
    S.dict = nullptr;
    free(S.language);
    S.language = xstrdup(code);
  }
  pthread_mutex_unlock(&S.lock);
}
