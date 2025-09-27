#include <mruby.h>
#include <mruby/msgpack.h>
#include <limits.h>

static mrb_value
mrb_msgpack_test_pack(mrb_state *mrb, mrb_value self)
{
  return mrb_msgpack_pack(mrb, mrb_str_new_lit(mrb, "hallo"));
}

static mrb_value
mrb_msgpack_test_unpack(mrb_state *mrb, mrb_value self)
{
  mrb_value hallo;
  mrb_get_args(mrb, "S", &hallo);
  return mrb_msgpack_unpack(mrb, hallo);
}

void
mrb_mruby_simplemsgpack_gem_test(mrb_state *mrb)
{
  struct RClass *msgpack_test = mrb_define_module(mrb, "MessagePackTest");

  mrb_define_module_function(mrb, msgpack_test, "test_pack",
                             mrb_msgpack_test_pack, MRB_ARGS_NONE());
  mrb_define_module_function(mrb, msgpack_test, "test_unpack",
                             mrb_msgpack_test_unpack, MRB_ARGS_REQ(1));

  mrb_define_const(mrb, msgpack_test, "FIXNUM_MAX",
                   mrb_int_value(mrb, MRB_INT_MAX));
  mrb_define_const(mrb, msgpack_test, "FIXNUM_MIN",
                   mrb_int_value(mrb, MRB_INT_MIN));

#ifdef MRB_MSGPACK_SYMBOLS
#ifndef MRB_MSGPACK_SYMBOLS_EXT
#define MRB_MSGPACK_SYMBOLS_EXT 0
#endif
  mrb_define_const(mrb, msgpack_test,
                   "SYMBOLS_ENABLED",
                   mrb_true_value());
  mrb_define_const(mrb, msgpack_test,
                   "SYMBOLS_EXT_TYPE",
                   mrb_int_value(mrb, MRB_MSGPACK_SYMBOLS_EXT));

# ifdef MRB_MSGPACK_SYMBOLS_AS_INT
  mrb_define_const(mrb, msgpack_test,
                   "SYMBOLS_MODE",
                   mrb_str_new_lit(mrb, "int"));
# else
  mrb_define_const(mrb, msgpack_test,
                   "SYMBOLS_MODE",
                   mrb_str_new_lit(mrb, "string"));
# endif
#else
  mrb_define_const(mrb, msgpack_test,
                   "SYMBOLS_ENABLED",
                   mrb_false_value());
#endif

#ifndef MRB_WITHOUT_FLOAT
# ifdef MRB_USE_FLOAT
  mrb_define_const(mrb, msgpack_test, "FLOAT_MAX",
                   mrb_float_value(mrb, FLT_MAX));
  mrb_define_const(mrb, msgpack_test, "FLOAT_MIN",
                   mrb_float_value(mrb, FLT_MIN));
# else
  mrb_define_const(mrb, msgpack_test, "FLOAT_MAX",
                   mrb_float_value(mrb, DBL_MAX));
  mrb_define_const(mrb, msgpack_test, "FLOAT_MIN",
                   mrb_float_value(mrb, DBL_MIN));
# endif
#endif
}
