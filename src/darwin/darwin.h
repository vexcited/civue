#ifndef CIVUE_DARWIN_H
#define CIVUE_DARWIN_H

#include "../internal.h"

#include <civue/metal.h>

// the short names civue's macos code is written with.
#define SEL_ CIVUE_SEL
#define CLS CIVUE_CLS
#define msg0 CIVUE_MSG0
#define msg1 CIVUE_MSG1
#define msg2 CIVUE_MSG2
#define msg3 CIVUE_MSG3
#define msg4 CIVUE_MSG4
#define msg5 CIVUE_MSG5
#define msg6 CIVUE_MSG6
#define msg7 CIVUE_MSG7
#define NEW CIVUE_NEW
#define AUTORELEASE CIVUE_AUTORELEASE

static inline id retain(id o) {
  return civue_retain(o);
}

static inline void release(id o) {
  civue_release(o);
}

static inline id autorelease(id o) {
  return civue_autorelease(o);
}

#define ns_str civue_ns_str
#define ns_strn civue_ns_strn
#define ns_cstr civue_ns_cstr
#define ns_dup civue_ns_dup
#define ns_url civue_ns_url
#define ns_data civue_ns_data
#define ns_data_nocopy civue_ns_data_nocopy
#define ns_num civue_ns_num
#define ns_int civue_ns_int
#define ns_count civue_ns_count
#define ns_at civue_ns_at
#define class_method civue_class_method
#define class_new civue_class_new
#define class_done civue_class_done
#define obj_ctx civue_obj_ctx
#define obj_set_ctx civue_obj_set_ctx

#endif
