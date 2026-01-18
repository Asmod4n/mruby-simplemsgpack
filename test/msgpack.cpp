#include <mruby.h>
#include <mruby/msgpack.h>
#include <limits.h>
#include <mruby/num_helpers.hpp>
#include <mruby/class.h>
/* -------------------------------------------------------------
 * Existing tests
 * ------------------------------------------------------------- */

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

/* -------------------------------------------------------------
 * New tests for MRB_API functions
 * ------------------------------------------------------------- */

/* Test constantize */
static mrb_value
mrb_msgpack_test_constantize(mrb_state *mrb, mrb_value self)
{
  mrb_value str;
  mrb_get_args(mrb, "S", &str);
  return mrb_str_constantize(mrb, str);
}

static mrb_value
test_pack_callback(mrb_state *mrb, mrb_value self)
{
  // Always pack as string "C_PACKED"
  return mrb_str_new_lit(mrb, "C_PACKED");
}

static mrb_value
test_unpack_callback(mrb_state *mrb, mrb_value self)
{
  // Always return symbol :c_unpacked
  return mrb_symbol_value(mrb_intern_lit(mrb, "c_unpacked"));
}

static mrb_value
mrb_msgpack_test_register_pack_type_real(mrb_state *mrb, mrb_value self)
{
  mrb_int type;
  mrb_get_args(mrb, "i", &type);

  mrb_msgpack_register_pack_type_cfunc(
      mrb,
      type,
      mrb->object_class,
      test_pack_callback,
      0,
      NULL);

  return mrb_true_value();
}

static mrb_value
mrb_msgpack_test_register_unpack_type_real(mrb_state *mrb, mrb_value self)
{
  mrb_int type;
  mrb_get_args(mrb, "i", &type);

  mrb_msgpack_register_unpack_type_cfunc(
      mrb,
      type,
      test_unpack_callback,
      0,
      NULL);

  return mrb_true_value();
}


static mrb_value
mrb_msgpack_test_sym_strategy_get(mrb_state *mrb, mrb_value self)
{
  return mrb_msgpack_get_symbol_strategy(mrb);
}

static mrb_value
mrb_msgpack_test_sym_strategy_set(mrb_state *mrb, mrb_value self)
{
  mrb_sym which;
  mrb_int ext_type = 0;

  mrb_get_args(mrb, "n|i", &which, &ext_type);

  mrb_msgpack_set_symbol_strategy(mrb, which, ext_type);
  return mrb_nil_value();
}


/* -------------------------------------------------------------
 * Test module initializer
 * ------------------------------------------------------------- */

MRB_BEGIN_DECL
void
mrb_mruby_simplemsgpack_gem_test(mrb_state *mrb)
{
  struct RClass *msgpack_test = mrb_define_module(mrb, "MessagePackTest");

  /* Existing tests */
  mrb_define_module_function(mrb, msgpack_test, "test_pack",
                             mrb_msgpack_test_pack, MRB_ARGS_NONE());
  mrb_define_module_function(mrb, msgpack_test, "test_unpack",
                             mrb_msgpack_test_unpack, MRB_ARGS_REQ(1));

  /* New tests */
  mrb_define_module_function(mrb, msgpack_test, "constantize",
                             mrb_msgpack_test_constantize, MRB_ARGS_REQ(1));

mrb_define_module_function(mrb, msgpack_test,
                           "register_pack_type_real",
                           mrb_msgpack_test_register_pack_type_real,
                           MRB_ARGS_REQ(1));

mrb_define_module_function(mrb, msgpack_test,
                           "register_unpack_type_real",
                           mrb_msgpack_test_register_unpack_type_real,
                           MRB_ARGS_REQ(1));


  mrb_define_module_function(mrb, msgpack_test, "sym_strategy_get",
                             mrb_msgpack_test_sym_strategy_get, MRB_ARGS_NONE());

  mrb_define_module_function(mrb, msgpack_test, "sym_strategy_set",
                             mrb_msgpack_test_sym_strategy_set, MRB_ARGS_REQ(1));

  /* Constants */
  mrb_define_const(mrb, msgpack_test, "FIXNUM_MAX",
                   mrb_int_value(mrb, MRB_INT_MAX));
  mrb_define_const(mrb, msgpack_test, "FIXNUM_MIN",
                   mrb_int_value(mrb, MRB_INT_MIN));

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
MRB_END_DECL
