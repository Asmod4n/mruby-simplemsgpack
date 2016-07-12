#ifndef MRUBY_MSGPACK_H
#define MRUBY_MSGPACK_H

#ifdef __cplusplus
extern "C" {
#endif

#include <mruby.h>

#define E_MSGPACK_ERROR (mrb_class_get_under(mrb, mrb_module_get(mrb, "MessagePack"), "Error"))

#ifdef __cplusplus
}
#endif

#endif
