#ifndef CIVUE_OBJC_H
#define CIVUE_OBJC_H

#include <civue/base.h>

#include <CoreFoundation/CoreFoundation.h>
#include <CoreGraphics/CoreGraphics.h>
#include <objc/message.h>
#include <objc/runtime.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef long NSInteger;
typedef unsigned long NSUInteger;
typedef CGRect NSRect;
typedef CGPoint NSPoint;
typedef CGSize NSSize;
typedef struct {
  NSUInteger location, length;
} NSRange;

// selectors and classes looked up once per call site.
#define CIVUE_SEL(s)                                                        \
  ({                                                                        \
    static SEL civue_sel_;                                                  \
    if (__builtin_expect(!civue_sel_, 0)) civue_sel_ = sel_registerName(s); \
    civue_sel_;                                                             \
  })
#define CIVUE_CLS(name)                                                         \
  ({                                                                            \
    static id civue_cls_;                                                       \
    if (__builtin_expect(!civue_cls_, 0)) civue_cls_ = (id)objc_getClass(name); \
    civue_cls_;                                                                 \
  })

// typed objc_msgSend casts. every argument's c type must match the method's, a literal 0 is cast to NSUInteger.
#define CIVUE_MSG0(R, o, s) ((R (*)(id, SEL))objc_msgSend)((id)(o), CIVUE_SEL(s))
#define CIVUE_MSG1(R, o, s, a) ((R (*)(id, SEL, typeof(a)))objc_msgSend)((id)(o), CIVUE_SEL(s), a)
#define CIVUE_MSG2(R, o, s, a, b) ((R (*)(id, SEL, typeof(a), typeof(b)))objc_msgSend)((id)(o), CIVUE_SEL(s), a, b)
#define CIVUE_MSG3(R, o, s, a, b, c) \
  ((R (*)(id, SEL, typeof(a), typeof(b), typeof(c)))objc_msgSend)((id)(o), CIVUE_SEL(s), a, b, c)
#define CIVUE_MSG4(R, o, s, a, b, c, d) \
  ((R (*)(id, SEL, typeof(a), typeof(b), typeof(c), typeof(d)))objc_msgSend)((id)(o), CIVUE_SEL(s), a, b, c, d)
#define CIVUE_MSG5(R, o, s, a, b, c, d, e)                                                                           \
  ((R (*)(id, SEL, typeof(a), typeof(b), typeof(c), typeof(d), typeof(e)))objc_msgSend)((id)(o), CIVUE_SEL(s), a, b, \
                                                                                        c, d, e)
#define CIVUE_MSG6(R, o, s, a, b, c, d, e, f)                                                       \
  ((R (*)(id, SEL, typeof(a), typeof(b), typeof(c), typeof(d), typeof(e), typeof(f)))objc_msgSend)( \
      (id)(o), CIVUE_SEL(s), a, b, c, d, e, f)
#define CIVUE_MSG7(R, o, s, a, b, c, d, e, f, g)                                                               \
  ((R (*)(id, SEL, typeof(a), typeof(b), typeof(c), typeof(d), typeof(e), typeof(f), typeof(g)))objc_msgSend)( \
      (id)(o), CIVUE_SEL(s), a, b, c, d, e, f, g)

// alloc then init, the result owned by the caller.
#define CIVUE_NEW(cls) CIVUE_MSG0(id, CIVUE_MSG0(id, CIVUE_CLS(cls), "alloc"), "init")
static inline id civue_retain(id o) { return o ? CIVUE_MSG0(id, o, "retain") : o; }
static inline void civue_release(id o) {
  if (o) CIVUE_MSG0(void, o, "release");
}
static inline id civue_autorelease(id o) { return o ? CIVUE_MSG0(id, o, "autorelease") : o; }

void *objc_autoreleasePoolPush(void);
void objc_autoreleasePoolPop(void *pool);
// runs a block of statements in an autorelease pool of its own.
#define CIVUE_AUTORELEASE(...)                      \
  do {                                              \
    void *civue_pool_ = objc_autoreleasePoolPush(); \
    __VA_ARGS__;                                    \
    objc_autoreleasePoolPop(civue_pool_);           \
  } while (0)

// an autoreleased NSString of utf-8, latin-1 when it is not valid utf-8.
id civue_ns_str(const char *s);
id civue_ns_strn(const char *s, size_t n);
// an NSString's utf-8, valid while the string lives, "" for nil.
const char *civue_ns_cstr(id s);
// a malloc'd copy of an NSString's utf-8, nullptr for nil.
char *civue_ns_dup(id s);
id civue_ns_url(const char *s);
id civue_ns_data(const void *p, size_t n);
id civue_ns_data_nocopy(const void *p, size_t n);
id civue_ns_num(double v);
id civue_ns_int(int64_t v);
NSUInteger civue_ns_count(id array);
id civue_ns_at(id array, NSUInteger i);

// adds a method implemented by a c function to a class, its types in objective-c's encoding.
void civue_class_method(Class cls, const char *sel, void *imp, const char *types);
// a subclass of `super`, or the class of that name made before, registered by civue_class_done once its methods
// are in. its objects carry a pointer for the app, see civue_obj_ctx.
Class civue_class_new(const char *name, const char *super);
void civue_class_done(Class cls);
void *civue_obj_ctx(id obj);
void civue_obj_set_ctx(id obj, void *ctx);

#ifdef __cplusplus
}
#endif

#endif
