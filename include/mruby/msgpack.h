#ifndef MRUBY_MSGPACK_H
#define MRUBY_MSGPACK_H

#include <mruby.h>

MRB_BEGIN_DECL

#define E_MSGPACK_ERROR (mrb_class_get_under(mrb, mrb_module_get(mrb, "MessagePack"), "Error"))
MRB_API mrb_value mrb_msgpack_pack(mrb_state *mrb, mrb_value object);
MRB_API mrb_value mrb_msgpack_unpack(mrb_state *mrb, mrb_value data);

MRB_END_DECL

#endif
