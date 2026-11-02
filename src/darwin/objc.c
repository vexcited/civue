#include "darwin.h"

enum { NSUTF8StringEncoding = 4, NSISOLatin1StringEncoding = 5 };

id civue_ns_str(const char *s) { return ns_strn(s ? s : "", s ? strlen(s) : 0); }

id civue_ns_strn(const char *s, size_t n) {
  id str = msg3(id, msg0(id, CLS("NSString"), "alloc"), "initWithBytes:length:encoding:", (const void *)s,
                (NSUInteger)n, (NSUInteger)NSUTF8StringEncoding);
  // invalid utf-8 gives nil, latin-1 still shows the text.
  if (!str)
    str = msg3(id, msg0(id, CLS("NSString"), "alloc"), "initWithBytes:length:encoding:", (const void *)s, (NSUInteger)n,
               (NSUInteger)NSISOLatin1StringEncoding);
  return autorelease(str);
}

const char *civue_ns_cstr(id s) {
  if (!s) return "";
  const char *c = msg0(const char *, s, "UTF8String");
  return c ? c : "";
}

char *civue_ns_dup(id s) { return s ? xstrdup(ns_cstr(s)) : nullptr; }

id civue_ns_url(const char *s) { return msg1(id, CLS("NSURL"), "URLWithString:", ns_str(s)); }

id civue_ns_data(const void *p, size_t n) { return msg2(id, CLS("NSData"), "dataWithBytes:length:", p, (NSUInteger)n); }

id civue_ns_data_nocopy(const void *p, size_t n) {
  return msg3(id, CLS("NSData"), "dataWithBytesNoCopy:length:freeWhenDone:", (void *)p, (NSUInteger)n, (bool)false);
}

id civue_ns_num(double v) { return msg1(id, CLS("NSNumber"), "numberWithDouble:", v); }
id civue_ns_int(int64_t v) { return msg1(id, CLS("NSNumber"), "numberWithLongLong:", (long long)v); }
NSUInteger civue_ns_count(id array) { return array ? msg0(NSUInteger, array, "count") : 0; }
id civue_ns_at(id array, NSUInteger i) { return msg1(id, array, "objectAtIndex:", i); }

void civue_class_method(Class cls, const char *sel, void *imp, const char *types) {
  if (!class_addMethod(cls, sel_registerName(sel), (IMP)imp, types))
    class_replaceMethod(cls, sel_registerName(sel), (IMP)imp, types);
}

Class civue_class_new(const char *name, const char *super) {
  Class existing = objc_getClass(name);
  if (existing) return existing;
  Class cls = objc_allocateClassPair(objc_getClass(super), name, 0);
  class_addIvar(cls, "civue_ctx", sizeof(void *), 3, "^v");
  return cls;
}

void civue_class_done(Class cls) {
  if (!objc_getClass(class_getName(cls)) || cls != objc_getClass(class_getName(cls))) objc_registerClassPair(cls);
}

void *civue_obj_ctx(id obj) {
  Ivar iv = class_getInstanceVariable(object_getClass(obj), "civue_ctx");
  return iv ? *(void **)((char *)obj + ivar_getOffset(iv)) : nullptr;
}

void civue_obj_set_ctx(id obj, void *ctx) {
  Ivar iv = class_getInstanceVariable(object_getClass(obj), "civue_ctx");
  if (iv) *(void **)((char *)obj + ivar_getOffset(iv)) = ctx;
}
