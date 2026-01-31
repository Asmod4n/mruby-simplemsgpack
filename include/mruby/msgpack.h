#ifndef MRUBY_MSGPACK_H
#define MRUBY_MSGPACK_H

#include <mruby.h>

MRB_BEGIN_DECL

#define E_MSGPACK_ERROR (mrb_class_get_under(mrb, mrb_module_get(mrb, "MessagePack"), "Error"))
MRB_API mrb_value mrb_msgpack_pack(mrb_state *mrb, mrb_value object);
MRB_API mrb_value mrb_msgpack_pack_argv(mrb_state *mrb, mrb_value *argv, mrb_int argv_len);
MRB_API mrb_value mrb_msgpack_unpack(mrb_state *mrb, mrb_value data);

MRB_API mrb_value mrb_str_constantize(mrb_state *mrb, mrb_value str);
MRB_API void mrb_msgpack_class_cache_clear(mrb_state *mrb);

MRB_API void mrb_msgpack_ensure(mrb_state *mrb);
MRB_API void mrb_msgpack_register_pack_type_value(mrb_state *mrb, int8_t type, mrb_value klass, mrb_value proc);
MRB_API void mrb_msgpack_register_unpack_type_value(mrb_state *mrb, int8_t type, mrb_value proc);
MRB_API void mrb_msgpack_register_pack_type_cfunc(mrb_state *mrb, int8_t type, struct RClass *klass, mrb_func_t cfunc, mrb_int argc, const mrb_value *argv);
MRB_API void mrb_msgpack_register_unpack_type_cfunc(mrb_state *mrb, int8_t type, mrb_func_t cfunc, mrb_int argc, const mrb_value *argv);
MRB_API void mrb_msgpack_set_symbol_strategy(mrb_state *mrb, mrb_sym which, int8_t ext_type);
MRB_API mrb_value mrb_msgpack_get_symbol_strategy(mrb_state *mrb);

MRB_END_DECL

#endif
