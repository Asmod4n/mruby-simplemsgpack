#define MSGPACK_NO_BOOST
#define MSGPACK_DEFAULT_API_VERSION 3

#include <msgpack.hpp>

#include <mruby.h>
#include <mruby/array.h>
#include <mruby/class.h>
#include <mruby/data.h>
#include <mruby/hash.h>
#include <mruby/numeric.h>
#include <mruby/string.h>
#include <mruby/variable.h>
#include <mruby/string_is_utf8.h>
#include <mruby/presym.h>
#include <mruby/msgpack.h>
#include <mruby/proc.h>

MRB_BEGIN_DECL
#include <mruby/internal.h>
MRB_END_DECL

#include <mruby/cpp_helpers.hpp>
#include <mruby/num_helpers.hpp>
#include <mruby/branch_pred.h>
#include <cstring>

#include <string>
#include <string_view>
#include <atomic>
#include <cstdint>


/* ------------------------------------------------------------------------
 * SBO writer
 * ------------------------------------------------------------------------ */
static mrb_int
safe_size_to_mrb_int(mrb_state *mrb, size_t sz)
{
  if (unlikely(sz > (size_t)MRB_INT_MAX)) {
    mrb_raise(mrb, E_ARGUMENT_ERROR, "msgpack: size too large");
  }
  return (mrb_int)sz;
}

static mrb_int
compute_capacity(mrb_state *mrb, size_t stack_size, size_t buf_size)
{
  mrb_int s = safe_size_to_mrb_int(mrb, stack_size);
  mrb_int b = safe_size_to_mrb_int(mrb, buf_size);

  mrb_int sum;
  if (unlikely(mrb_int_add_overflow(s, b, &sum))) {
    mrb_raise(mrb, E_ARGUMENT_ERROR, "msgpack: size overflow");
  }

  mrb_int capa;
  if (unlikely(mrb_int_mul_overflow(sum, 2, &capa))) {
    mrb_raise(mrb, E_ARGUMENT_ERROR, "msgpack: size overflow");
  }

  return capa;
}

struct mrb_msgpack_sbo_writer {
  mrb_msgpack_sbo_writer(mrb_state* mrb)
    : mrb(mrb) {}

  void write(const char* buf, size_t buf_size) {
    if (mrb_string_p(heap_str)) {
      mrb_str_cat(mrb, heap_str, buf, buf_size);
    } else {
      if (likely(buf_size <= STACK_CAP - stack_size)) {
        std::memcpy(stack_buf + stack_size, buf, buf_size);
        stack_size += buf_size;
      } else {
        mrb_int capa = compute_capacity(mrb, stack_size, buf_size);
        heap_str = mrb_str_new_capa(mrb, capa);
        mrb_str_cat(mrb, heap_str, stack_buf, stack_size);
        mrb_str_cat(mrb, heap_str, buf, buf_size);
      }
    }
  }

  mrb_value result() {
    if (mrb_string_p(heap_str)) {
      return heap_str;
    } else {
      return mrb_str_new(mrb, stack_buf, stack_size);
    }
  }

private:
  mrb_state* mrb;

  static constexpr size_t STACK_CAP = 8 * 1024;
  char   stack_buf[STACK_CAP] = {0};
  size_t stack_size      = 0;
  mrb_value heap_str = mrb_undef_value();
};

/* ------------------------------------------------------------------------
 * Forward declarations
 * ------------------------------------------------------------------------ */

struct mrb_msgpack_ctx {
    void (*sym_packer)(mrb_state*, mrb_value, int8_t, msgpack::packer<mrb_msgpack_sbo_writer>&);
    mrb_value (*sym_unpacker)(mrb_state*, const msgpack::object&);
    int8_t ext_type;
};
MRB_CPP_DEFINE_TYPE(mrb_msgpack_ctx, mrb_msgpack_ctx);

/* default symbol strategy type (used by ctx) */
#ifndef MRB_MSGPACK_DEFAULT_SYMBOL_TYPE
#define MRB_MSGPACK_DEFAULT_SYMBOL_TYPE 0U
#endif

#define MRB_MSGPACK_CONTEXT(mrb)                                          \
  ([](mrb_state* _m) -> mrb_msgpack_ctx* {                                \
    mrb_value _v = mrb_gv_get(_m, MRB_SYM(__msgpack__ctx));               \
    mrb_assert(mrb_cptr_p(_v));                                           \
    return static_cast<mrb_msgpack_ctx*>(mrb_cptr(_v));                   \
  }(mrb))

static void mrb_msgpack_pack_value(mrb_state* mrb, mrb_value self, msgpack::packer<mrb_msgpack_sbo_writer>& pk);
static void mrb_msgpack_pack_array_value(mrb_state* mrb, mrb_value self, msgpack::packer<mrb_msgpack_sbo_writer>& pk);
static void mrb_msgpack_pack_hash_value(mrb_state* mrb, mrb_value self, msgpack::packer<mrb_msgpack_sbo_writer>& pk);

static mrb_value mrb_unpack_msgpack_obj(mrb_state* mrb, const msgpack::object& obj);
static mrb_value mrb_unpack_msgpack_obj_array(mrb_state* mrb, const msgpack::object& obj);
static mrb_value mrb_unpack_msgpack_obj_map(mrb_state* mrb, const msgpack::object& obj);

static inline void mrb_msgpack_pack_symbol_value_as_raw(mrb_state* mrb,
                                                        mrb_value self,
                                                        int8_t ext_type,
                                                        msgpack::packer<mrb_msgpack_sbo_writer>& pk);

static mrb_value mrb_msgpack_sym_strategy(mrb_state *mrb, mrb_value self);

/* ------------------------------------------------------------------------
 * GV-backed registry and context helpers (no Ruby namespace pollution)
 * ------------------------------------------------------------------------ */

static mrb_value
ensure_ext_registry(mrb_state *mrb)
{
  mrb_value reg = mrb_gv_get(mrb, MRB_SYM(__msgpack_ext_registry__));
  if (likely(mrb_hash_p(reg))) return reg;

  mrb_value registry   = mrb_hash_new(mrb);
  mrb_value packers    = mrb_hash_new(mrb);
  mrb_value unpackers  = mrb_hash_new(mrb);

  mrb_hash_set(mrb, registry, mrb_symbol_value(MRB_SYM(packers)),   packers);
  mrb_hash_set(mrb, registry, mrb_symbol_value(MRB_SYM(unpackers)), unpackers);

  mrb_gv_set(mrb, MRB_SYM(__msgpack_ext_registry__), registry);
  return registry;
}

static mrb_value
ensure_msgpack_ctx(mrb_state *mrb)
{
  mrb_value ctxv = mrb_gv_get(mrb, MRB_SYM(__msgpack__ctx));
  if (likely(mrb_cptr_p(ctxv))) return ctxv;

  /* Allocate the C context directly and store as a cptr in a GV.
     This avoids creating any Ruby classes/modules and works with mrb_open_core. */
  mrb_msgpack_ctx *ctx = (mrb_msgpack_ctx*)mrb_malloc(mrb, sizeof(mrb_msgpack_ctx));
  ctx->sym_packer   = mrb_msgpack_pack_symbol_value_as_raw;
  ctx->sym_unpacker = nullptr;
  ctx->ext_type     = (int8_t)MRB_MSGPACK_DEFAULT_SYMBOL_TYPE;

  mrb_value cptr = mrb_cptr_value(mrb, (void*)ctx);
  mrb_gv_set(mrb, MRB_SYM(__msgpack__ctx), cptr);
  return cptr;
}

static mrb_value
ext_packers_hash(mrb_state *mrb)
{
  mrb_value registry = ensure_ext_registry(mrb);
  mrb_value packers = mrb_hash_get(mrb, registry, mrb_symbol_value(MRB_SYM(packers)));
  if (likely(mrb_hash_p(packers))) {
    return packers;
  } else {
    mrb_raise(mrb, E_RUNTIME_ERROR, "ext packers is not a hash");
  }
}

static mrb_value
ext_unpackers_hash(mrb_state *mrb)
{
  mrb_value registry = ensure_ext_registry(mrb);
  mrb_value unpackers = mrb_hash_get(mrb, registry, mrb_symbol_value(MRB_SYM(unpackers)));
  if (likely(mrb_hash_p(unpackers))) {
    return unpackers;
  } else {
    mrb_raise(mrb, E_RUNTIME_ERROR, "ext unpackers is not a hash");
  }
}

MRB_BEGIN_DECL
MRB_API void
mrb_msgpack_ensure(mrb_state *mrb)
{
  /* Ensure MessagePack module exists */
  struct RClass *msgpack_mod = mrb_define_module_id(mrb, MRB_SYM(MessagePack));

  /* Ensure MessagePack::Error exists */
  mrb_define_class_under_id(mrb, msgpack_mod, MRB_SYM(Error), E_RUNTIME_ERROR);
  ensure_ext_registry(mrb);
  ensure_msgpack_ctx(mrb);
}

MRB_API void
mrb_msgpack_register_pack_type_value(mrb_state *mrb, int8_t type, mrb_value klass, mrb_value proc)
{
  if (unlikely(type < 0 || type > 127)) mrb_raise(mrb, E_RANGE_ERROR, "ext type out of range");
  if (unlikely(mrb_type(proc) != MRB_TT_PROC)) mrb_raise(mrb, E_TYPE_ERROR, "packer must be a Proc");

  mrb_value packers = ext_packers_hash(mrb);
  mrb_value cfg = mrb_hash_new_capa(mrb, 2);
  mrb_hash_set(mrb, cfg, mrb_symbol_value(MRB_SYM(type)),   mrb_fixnum_value(type));
  mrb_hash_set(mrb, cfg, mrb_symbol_value(MRB_SYM(packer)), proc);
  mrb_hash_set(mrb, packers, klass, cfg);
}

MRB_API void
mrb_msgpack_register_unpack_type_value(mrb_state *mrb, int8_t type, mrb_value proc)
{
  if (unlikely(type < 0 || type > 127)) mrb_raise(mrb, E_RANGE_ERROR, "ext type out of range");
  if (unlikely(mrb_type(proc) != MRB_TT_PROC)) mrb_raise(mrb, E_TYPE_ERROR, "unpacker must be a Proc");

  mrb_value unpackers = ext_unpackers_hash(mrb);
  mrb_hash_set(mrb, unpackers, mrb_fixnum_value(type), proc);
}

MRB_API void
mrb_msgpack_register_pack_type_cfunc(mrb_state *mrb,
                                     int8_t type,
                                     struct RClass *klass,
                                     mrb_func_t cfunc,
                                     mrb_int argc,
                                     const mrb_value *argv)
{
  if (unlikely(klass == NULL)) mrb_raise(mrb, E_ARGUMENT_ERROR, "klass is NULL");
  if (unlikely(cfunc == NULL)) mrb_raise(mrb, E_ARGUMENT_ERROR, "pack callback cannot be NULL");

  struct RProc *rproc =
      mrb_proc_new_cfunc_with_env(mrb, cfunc, argc, argv);

  mrb_value proc = mrb_obj_value(rproc);

  mrb_msgpack_register_pack_type_value(mrb, type, mrb_obj_value(klass), proc);
}

MRB_API void
mrb_msgpack_register_unpack_type_cfunc(mrb_state *mrb,
                                       int8_t type,
                                       mrb_func_t cfunc,
                                       mrb_int argc,
                                       const mrb_value *argv)
{
  if (unlikely(cfunc == NULL)) mrb_raise(mrb, E_ARGUMENT_ERROR, "unpack callback cannot be NULL");

  struct RProc *rproc =
      mrb_proc_new_cfunc_with_env(mrb, cfunc, argc, argv);

  mrb_value proc = mrb_obj_value(rproc);

  mrb_msgpack_register_unpack_type_value(mrb, type, proc);
}

MRB_API void
mrb_msgpack_teardown(mrb_state *mrb)
{
  mrb_value ctxv = mrb_gv_get(mrb, MRB_SYM(__msgpack__ctx));

  mrb_gv_remove(mrb, MRB_SYM(__msgpack_ext_registry__));
  mrb_gv_remove(mrb, MRB_SYM(__msgpack__ctx));

  if (likely(mrb_cptr_p(ctxv))) {
    void *p = mrb_cptr(ctxv);
    mrb_free(mrb, p);
  }
}

MRB_END_DECL
/* ------------------------------------------------------------------------
 * Primitive packers
 * ------------------------------------------------------------------------ */

#define pack_integer_helper_(x, pk, self) pk.pack_int##x(static_cast<int##x##_t>(mrb_integer(self)))
#define pack_integer_helper(x, pk, self)  pack_integer_helper_(x, pk, self)
#define mrb_msgpack_pack_int(pk, self)    pack_integer_helper(MRB_INT_BIT, pk, self)

static inline void
mrb_msgpack_pack_integer_value(mrb_state *mrb, mrb_value self, msgpack::packer<mrb_msgpack_sbo_writer>& pk)
{
  mrb_msgpack_pack_int(pk, self);
}

#ifndef MRB_WITHOUT_FLOAT
static inline void
mrb_msgpack_pack_float_value(mrb_state *mrb, mrb_value self, msgpack::packer<mrb_msgpack_sbo_writer>& pk)
{
#ifdef MRB_USE_FLOAT
  pk.pack_float(mrb_float(self));
#else
  pk.pack_double(mrb_float(self));
#endif
}
#endif

static inline void
mrb_msgpack_pack_string_value(mrb_state *mrb, mrb_value self, msgpack::packer<mrb_msgpack_sbo_writer>& pk)
{
  const char* ptr = RSTRING_PTR(self);
  mrb_int len = RSTRING_LEN(self);

  if (mrb_str_is_utf8(self)) {
    pk.pack_str(static_cast<uint32_t>(len));
    pk.pack_str_body(ptr, static_cast<size_t>(len));
  } else {
    pk.pack_bin(static_cast<uint32_t>(len));
    pk.pack_bin_body(ptr, static_cast<size_t>(len));
  }
}

/* ------------------------------------------------------------------------
 * Symbol packing strategies
 * ------------------------------------------------------------------------ */

static inline void
mrb_msgpack_pack_symbol_value_as_int(mrb_state* mrb, mrb_value self, int8_t ext_type, msgpack::packer<mrb_msgpack_sbo_writer>& pk)
{
  mrb_sym sym = mrb_symbol(self);

  pk.pack_ext(sizeof(sym), ext_type);
  pk.pack_ext_body(reinterpret_cast<const char*>(&sym), sizeof(sym));
}

static inline void
mrb_msgpack_pack_symbol_value_as_string(mrb_state* mrb, mrb_value self, int8_t ext_type, msgpack::packer<mrb_msgpack_sbo_writer>& pk)
{
  mrb_sym sym = mrb_symbol(self);

  mrb_int len;
  const char* name = mrb_sym_name_len(mrb, sym, &len);

  pk.pack_ext(static_cast<uint32_t>(len), ext_type);
  pk.pack_ext_body(name, static_cast<size_t>(len));
}

static inline void
mrb_msgpack_pack_symbol_value_as_raw(mrb_state* mrb,
                                     mrb_value self,
                                     int8_t /*ext_type unused*/,
                                     msgpack::packer<mrb_msgpack_sbo_writer>& pk)
{
  mrb_sym sym = mrb_symbol(self);

  mrb_int len;
  const char* name = mrb_sym_name_len(mrb, sym, &len);

  pk.pack_str(static_cast<uint32_t>(len));
  pk.pack_str_body(name, static_cast<size_t>(len));
}

/* ------------------------------------------------------------------------
 * Ext packer config lookup
 * ------------------------------------------------------------------------ */

static mrb_value
mrb_msgpack_get_ext_config(mrb_state* mrb, mrb_value obj)
{
  mrb_value ext_packers = ext_packers_hash(mrb);
  mrb_value obj_class = mrb_obj_value(mrb_obj_class(mrb, obj));
  mrb_value ext_config = mrb_hash_get(mrb, ext_packers, obj_class);

  /* Fast path: exact class match */
  if (mrb_hash_p(ext_config)) {
    return ext_config;
  }

  struct Ctx {
    mrb_value obj;
    mrb_value found;
  } ctx = { obj, mrb_nil_value() };

  /* Search for a superclass match */
  mrb_hash_foreach(mrb, mrb_hash_ptr(ext_packers),
    [](mrb_state* mrb, mrb_value key, mrb_value val, void* data) -> int {
      auto* c = (Ctx*)data;
      struct RClass* klass = mrb_class_ptr(key);

      if (mrb_obj_is_kind_of(mrb, c->obj, klass)) {
        c->found = val;
        return 1; /* stop */
      }

      return 0; /* continue */
    },
    &ctx
  );

  /* No match found */
  if (mrb_nil_p(ctx.found)) {
    return ctx.found;
  }

  /* Now safe to cache under concrete class */
  mrb_hash_set(mrb, ext_packers, obj_class, ctx.found);

  return ctx.found;
}

static mrb_bool
mrb_msgpack_pack_ext_value(mrb_state* mrb, mrb_value obj, msgpack::packer<mrb_msgpack_sbo_writer>& pk)
{
  mrb_int arena_index = mrb_gc_arena_save(mrb);

  mrb_value ext_config = mrb_msgpack_get_ext_config(mrb, obj);
  if (!mrb_hash_p(ext_config)) {
    mrb_gc_arena_restore(mrb, arena_index);
    return FALSE;
  }

  mrb_value packer = mrb_hash_get(mrb, ext_config, mrb_symbol_value(MRB_SYM(packer)));
  if (unlikely(mrb_type(packer) != MRB_TT_PROC)) {
    mrb_gc_arena_restore(mrb, arena_index);
    mrb_raise(mrb, E_TYPE_ERROR, "malformed packer");
  }

  mrb_value packed = mrb_yield(mrb, packer, obj);
  if (unlikely(!mrb_string_p(packed))) {
    mrb_gc_arena_restore(mrb, arena_index);
    mrb_raise(mrb, E_TYPE_ERROR, "no string returned by ext type packer");
  }

  mrb_value type = mrb_hash_get(mrb, ext_config, mrb_symbol_value(MRB_SYM(type)));
  if (unlikely(!mrb_integer_p(type))) {
    mrb_gc_arena_restore(mrb, arena_index);
    mrb_raise(mrb, E_TYPE_ERROR, "malformed type");
  }

  const char* body = RSTRING_PTR(packed);
  mrb_int len = RSTRING_LEN(packed);
  mrb_int t = mrb_integer(type);

  pk.pack_ext(static_cast<uint32_t>(len), static_cast<int8_t>(t));
  pk.pack_ext_body(body, static_cast<size_t>(len));

  mrb_gc_arena_restore(mrb, arena_index);
  return TRUE;
}

/* ------------------------------------------------------------------------
 * Composite packers (array, hash)
 * ------------------------------------------------------------------------ */

static void
mrb_msgpack_pack_array_value(mrb_state* mrb,
                             mrb_value self,
                             msgpack::packer<mrb_msgpack_sbo_writer>& pk)
{
  mrb_int n = RARRAY_LEN(self);
  mrb_int arena_index = mrb_gc_arena_save(mrb);

  pk.pack_array(static_cast<uint32_t>(n));

  for (mrb_int i = 0; i < n; ++i) {
    mrb_msgpack_pack_value(mrb,
                           mrb_ary_ref(mrb, self, i),
                           pk);
    mrb_gc_arena_restore(mrb, arena_index);
  }
}

static void
mrb_msgpack_pack_hash_value(mrb_state* mrb,
                            mrb_value self,
                            msgpack::packer<mrb_msgpack_sbo_writer>& pk)
{
  uint32_t n = static_cast<uint32_t>(mrb_hash_size(mrb, self));
  pk.pack_map(n);

  mrb_int arena_index = mrb_gc_arena_save(mrb);

  struct Ctx {
    msgpack::packer<mrb_msgpack_sbo_writer>* pk;
    mrb_int arena_index;
  } ctx{ &pk, arena_index };

  mrb_hash_foreach(mrb, mrb_hash_ptr(self),
    [](mrb_state* mrb, mrb_value key, mrb_value val, void *p) -> int {
      Ctx *c = static_cast<Ctx*>(p);
      mrb_msgpack_pack_value(mrb, key, *c->pk);
      mrb_msgpack_pack_value(mrb, val, *c->pk);

      mrb_gc_arena_restore(mrb, c->arena_index);
      return 0;
    },
    &ctx
  );
}

/* ------------------------------------------------------------------------
 * Core pack dispatcher
 * ------------------------------------------------------------------------ */

static void
mrb_msgpack_pack_value(mrb_state* mrb,
                       mrb_value self,
                       msgpack::packer<mrb_msgpack_sbo_writer>& pk)
{
  switch (mrb_type(self)) {
    case MRB_TT_FALSE:
      if (mrb_nil_p(self)) pk.pack_nil();
      else pk.pack_false();
      break;

    case MRB_TT_TRUE:
      pk.pack_true();
      break;

#ifndef MRB_WITHOUT_FLOAT
    case MRB_TT_FLOAT:
      mrb_msgpack_pack_float_value(mrb, self, pk);
      break;
#endif

    case MRB_TT_INTEGER:
      mrb_msgpack_pack_integer_value(mrb, self, pk);
      break;

    case MRB_TT_HASH:
      mrb_msgpack_pack_hash_value(mrb, self, pk);
      break;

    case MRB_TT_ARRAY:
      mrb_msgpack_pack_array_value(mrb, self, pk);
      break;

    case MRB_TT_STRING:
      mrb_msgpack_pack_string_value(mrb, self, pk);
      break;

    case MRB_TT_SYMBOL:  {
      mrb_msgpack_ctx *ctx = MRB_MSGPACK_CONTEXT(mrb);
      ctx->sym_packer(mrb, self, ctx->ext_type, pk);
    }  break;

    default: {
      if (mrb_msgpack_pack_ext_value(mrb, self, pk)) break;

      mrb_value v;

      v = mrb_type_convert_check(mrb, self, MRB_TT_HASH, MRB_SYM(to_hash));
      if (mrb_hash_p(v)) {
        mrb_msgpack_pack_hash_value(mrb, v, pk);
        break;
      }

      v = mrb_type_convert_check(mrb, self, MRB_TT_ARRAY, MRB_SYM(to_ary));
      if (mrb_array_p(v)) {
        mrb_msgpack_pack_array_value(mrb, v, pk);
        break;
      }

      v = mrb_type_convert_check(mrb, self, MRB_TT_INTEGER, MRB_SYM(to_int));
      if (mrb_integer_p(v)) {
        mrb_msgpack_pack_integer_value(mrb, v, pk);
        break;
      }

      v = mrb_type_convert_check(mrb, self, MRB_TT_STRING, MRB_SYM(to_str));
      if (mrb_string_p(v)) {
        mrb_msgpack_pack_string_value(mrb, v, pk);
        break;
      }

      v = mrb_type_convert(mrb, self, MRB_TT_STRING, MRB_SYM(to_s));
      mrb_msgpack_pack_string_value(mrb, v, pk);
      break;
    }
  }
}

/* ------------------------------------------------------------------------
 * Ruby-visible pack wrappers
 * ------------------------------------------------------------------------ */

#define DEFINE_MSGPACK_PACKER(FUNC_NAME, PACK_FN)                              \
static mrb_value                                                               \
FUNC_NAME(mrb_state* mrb, mrb_value self) {                                    \
  mrb_msgpack_sbo_writer writer(mrb);                                          \
  using Packer = msgpack::packer<mrb_msgpack_sbo_writer>;                      \
  Packer pk(writer);                                                           \
  PACK_FN(mrb, self, pk);                                                      \
  return writer.result();                                                      \
}

/* to_msgpack entrypoints */
DEFINE_MSGPACK_PACKER(mrb_msgpack_pack_object,  mrb_msgpack_pack_value)
DEFINE_MSGPACK_PACKER(mrb_msgpack_pack_string,          mrb_msgpack_pack_string_value)
DEFINE_MSGPACK_PACKER(mrb_msgpack_pack_array,   mrb_msgpack_pack_array_value)
DEFINE_MSGPACK_PACKER(mrb_msgpack_pack_hash,    mrb_msgpack_pack_hash_value)
DEFINE_MSGPACK_PACKER(mrb_msgpack_pack_integer,         mrb_msgpack_pack_integer_value)
#ifndef MRB_WITHOUT_FLOAT
DEFINE_MSGPACK_PACKER(mrb_msgpack_pack_float,           mrb_msgpack_pack_float_value)
#endif

static inline void
pack_true_fn(mrb_state*, mrb_value, msgpack::packer<mrb_msgpack_sbo_writer>& pk)
{
  pk.pack_true();
}

static inline void
pack_false_fn(mrb_state*, mrb_value, msgpack::packer<mrb_msgpack_sbo_writer>& pk)
{
  pk.pack_false();
}

static inline void
pack_nil_fn(mrb_state*, mrb_value, msgpack::packer<mrb_msgpack_sbo_writer>& pk)
{
  pk.pack_nil();
}

DEFINE_MSGPACK_PACKER(mrb_msgpack_pack_true,  pack_true_fn)
DEFINE_MSGPACK_PACKER(mrb_msgpack_pack_false, pack_false_fn)
DEFINE_MSGPACK_PACKER(mrb_msgpack_pack_nil,   pack_nil_fn)

/* ------------------------------------------------------------------------
 * Public C pack API
 * ------------------------------------------------------------------------ */

MRB_API mrb_value
mrb_msgpack_pack(mrb_state *mrb, mrb_value object)
{
  mrb_msgpack_sbo_writer writer(mrb);
  msgpack::packer<mrb_msgpack_sbo_writer> pk(writer);

  mrb_msgpack_pack_value(mrb, object, pk);

  return writer.result();
}

MRB_API mrb_value
mrb_msgpack_pack_argv(mrb_state *mrb, mrb_value *argv, mrb_int argv_len)
{
  mrb_msgpack_sbo_writer writer(mrb);
  msgpack::packer<mrb_msgpack_sbo_writer> pk(writer);

  pk.pack_array(static_cast<uint32_t>(argv_len));

  for (mrb_int i = 0; i < argv_len; ++i) {
    mrb_msgpack_pack_value(mrb, argv[i], pk);
  }

  return writer.result();
}

static mrb_value
mrb_msgpack_pack_m(mrb_state *mrb, mrb_value self)
{
  mrb_value object;
  mrb_get_args(mrb, "o", &object);
  return mrb_msgpack_pack(mrb, object);
}

/* ------------------------------------------------------------------------
 * Ext packer registration
 * ------------------------------------------------------------------------ */

static mrb_value
mrb_msgpack_register_pack_type(mrb_state* mrb, mrb_value self)
{
  mrb_value ext_packers;
  mrb_int type;
  mrb_value mrb_class;
  mrb_value block = mrb_nil_value();
  mrb_value ext_config;

  mrb_get_args(mrb, "iC&", &type, &mrb_class, &block);

  if (type < 0 || type > 127) {
    mrb_raise(mrb, E_RANGE_ERROR, "ext type out of range");
  }
  if (mrb_nil_p(block)) {
    mrb_raise(mrb, E_ARGUMENT_ERROR, "no block given");
  }
  if (mrb_type(block) != MRB_TT_PROC) {
    mrb_raise(mrb, E_TYPE_ERROR, "not a block");
  }

  if (mrb_class_ptr(mrb_class) == mrb->symbol_class) {
    mrb_raise(mrb, E_ARGUMENT_ERROR,
              "cannot register ext packer for Symbols, use the new MessagePack.sym_strategy function.");
  }

  ext_packers = ext_packers_hash(mrb);
  ext_config = mrb_hash_new_capa(mrb, 2);

  mrb_hash_set(mrb, ext_config, mrb_symbol_value(MRB_SYM(type)),   mrb_convert_number(mrb, type));
  mrb_hash_set(mrb, ext_config, mrb_symbol_value(MRB_SYM(packer)), block);
  mrb_hash_set(mrb, ext_packers, mrb_class, ext_config);

  return mrb_nil_value();
}


static mrb_value
mrb_msgpack_ext_packer_registered(mrb_state *mrb, mrb_value self)
{
  mrb_value mrb_class;
  mrb_get_args(mrb, "C", &mrb_class);

  return mrb_bool_value(
    mrb_test(
      mrb_hash_get(mrb,
                   ext_packers_hash(mrb),
                   mrb_class)
    )
  );
}

/* ------------------------------------------------------------------------
 * Symbol unpack strategies
 * ------------------------------------------------------------------------ */

static inline mrb_value
mrb_msgpack_unpack_symbol_as_int(mrb_state* mrb, const msgpack::object& obj)
{
  if (unlikely(obj.via.ext.size != sizeof(mrb_sym))) {
    mrb_raise(mrb, E_MSGPACK_ERROR, "invalid symbol ext body size");
  }

  mrb_sym sym;
  std::memcpy(&sym, obj.via.ext.data(), sizeof(sym));
  return mrb_symbol_value(sym);
}

static inline mrb_value
mrb_msgpack_unpack_symbol_as_string(mrb_state* mrb, const msgpack::object& obj)
{
  return mrb_symbol_value(
    mrb_intern(mrb, obj.via.ext.data(), (size_t)obj.via.ext.size)
  );
}

/* ------------------------------------------------------------------------
 * Core unpack dispatch
 * ------------------------------------------------------------------------ */

static mrb_value
mrb_unpack_msgpack_obj(mrb_state* mrb, const msgpack::object& obj)
{
  switch (obj.type) {
    case msgpack::type::NIL:
      return mrb_nil_value();

    case msgpack::type::BOOLEAN:
      return mrb_bool_value(obj.via.boolean);

    case msgpack::type::POSITIVE_INTEGER:
      return mrb_convert_number(mrb, obj.via.u64);

    case msgpack::type::NEGATIVE_INTEGER:
      return mrb_convert_number(mrb, obj.via.i64);

    case msgpack::type::FLOAT32:
    case msgpack::type::FLOAT64:
      return mrb_convert_number(mrb, obj.via.f64);

    case msgpack::type::STR:
      return mrb_str_new(mrb, obj.via.str.ptr, obj.via.str.size);

    case msgpack::type::BIN:
      return mrb_str_new(mrb, obj.via.bin.ptr, obj.via.bin.size);

    case msgpack::type::ARRAY:
      return mrb_unpack_msgpack_obj_array(mrb, obj);

    case msgpack::type::MAP:
      return mrb_unpack_msgpack_obj_map(mrb, obj);

    case msgpack::type::EXT: {
      auto ext_type = obj.via.ext.type();
      mrb_msgpack_ctx* ctx = MRB_MSGPACK_CONTEXT(mrb);
      if (ext_type == ctx->ext_type && ctx->sym_unpacker != nullptr) {
        return ctx->sym_unpacker(mrb, obj);
      }
      mrb_value unpacker = mrb_hash_get(
        mrb,
        ext_unpackers_hash(mrb),
        mrb_convert_number(mrb, ext_type)
      );

      if (likely(mrb_type(unpacker) == MRB_TT_PROC)) {
        return mrb_yield(
          mrb,
          unpacker,
          mrb_str_new(mrb, obj.via.ext.data(), obj.via.ext.size)
        );
      }
      else {
        mrb_raisef(mrb, E_MSGPACK_ERROR, "Cannot unpack ext type %d", ext_type);
      }
    }

    default:
      mrb_raise(mrb, E_MSGPACK_ERROR, "Cannot unpack unknown msgpack type");
  }

  return mrb_undef_value();
}

static mrb_value
mrb_unpack_msgpack_obj_array(mrb_state* mrb, const msgpack::object& obj)
{
  if (obj.via.array.size == 0) return mrb_ary_new(mrb);

  mrb_value ary = mrb_ary_new_capa(mrb, obj.via.array.size);
  mrb_int arena_index = mrb_gc_arena_save(mrb);

  for (uint32_t i = 0; i < obj.via.array.size; i++) {
    mrb_ary_push(mrb, ary, mrb_unpack_msgpack_obj(mrb, obj.via.array.ptr[i]));
    mrb_gc_arena_restore(mrb, arena_index);
  }

  return ary;
}

static mrb_value
mrb_unpack_msgpack_obj_map(mrb_state* mrb, const msgpack::object& obj)
{
  if (obj.via.map.size == 0) return mrb_hash_new(mrb);

  mrb_value hash = mrb_hash_new_capa(mrb, obj.via.map.size);
  mrb_int arena_index = mrb_gc_arena_save(mrb);

  for (uint32_t i = 0; i < obj.via.map.size; i++) {
    mrb_value key = mrb_unpack_msgpack_obj(mrb, obj.via.map.ptr[i].key);
    mrb_value val = mrb_unpack_msgpack_obj(mrb, obj.via.map.ptr[i].val);
    mrb_hash_set(mrb, hash, key, val);
    mrb_gc_arena_restore(mrb, arena_index);
  }

  return hash;
}

/* ------------------------------------------------------------------------
 * Public C unpack API
 * ------------------------------------------------------------------------ */

MRB_API mrb_value
mrb_msgpack_unpack(mrb_state *mrb, mrb_value data)
{
  data = mrb_str_to_str(mrb, data);
  msgpack::object_handle oh =
    msgpack::unpack(RSTRING_PTR(data), RSTRING_LEN(data));
  return mrb_unpack_msgpack_obj(mrb, oh.get());
}

static mrb_value
mrb_msgpack_unpack_m(mrb_state* mrb, mrb_value self)
{
  mrb_value data, block = mrb_nil_value();
  mrb_get_args(mrb, "o&", &data, &block);
  data = mrb_str_to_str(mrb, data);

  const char* buf = RSTRING_PTR(data);
  std::size_t len = RSTRING_LEN(data);
  std::size_t off = 0;

  try {
    if (mrb_type(block) == MRB_TT_PROC) {
      while (off < len) {
        try {
          msgpack::object_handle oh = msgpack::unpack(buf, len, off);
          mrb_yield(mrb, block, mrb_unpack_msgpack_obj(mrb, oh.get()));
        }
        catch (const msgpack::insufficient_bytes&) {
          break;
        }
      }
      return mrb_convert_number(mrb, (mrb_int)off);
    }
    else {
      msgpack::object_handle oh = msgpack::unpack(buf, len, off);
      return mrb_unpack_msgpack_obj(mrb, oh.get());
    }
  }
  catch (const std::exception &e) {
    mrb_raisef(mrb, E_MSGPACK_ERROR,
               "Can't unpack: %S", mrb_str_new_cstr(mrb, e.what()));
  }

  return mrb_undef_value();
}

/* ------------------------------------------------------------------------
 * Lazy unpacking / ObjectHandle
 * ------------------------------------------------------------------------ */

struct msgpack_object_handle {
  msgpack::object_handle oh;
  std::size_t off;

  msgpack_object_handle()
    : oh(msgpack::object_handle()), off(0) {}
};

MRB_CPP_DEFINE_TYPE(msgpack_object_handle, msgpack_object_handle)

static mrb_value
mrb_msgpack_object_handle_new(mrb_state *mrb, mrb_value self)
{
  mrb_value data;
  mrb_get_args(mrb, "S", &data);

  mrb_cpp_new<msgpack_object_handle>(mrb, self);
  mrb_iv_set(mrb, self, MRB_SYM(data), data);

  return self;
}

static mrb_value
mrb_msgpack_object_handle_value(mrb_state *mrb, mrb_value self)
{
  auto* handle = (msgpack_object_handle*)mrb_data_get_ptr(mrb, self, &msgpack_object_handle_type);
  if (unlikely(!handle)) {
    mrb_raise(mrb, E_MSGPACK_ERROR, "ObjectHandle is not initialized");
    return mrb_undef_value();
  }

  return mrb_unpack_msgpack_obj(mrb, handle->oh.get());
}

static mrb_value
mrb_msgpack_unpack_lazy_m(mrb_state *mrb, mrb_value self)
{
  mrb_value data;
  mrb_get_args(mrb, "o", &data);
  data = mrb_str_to_str(mrb, data);

  try {
    mrb_value object_handle =
      mrb_obj_new(mrb,
                  mrb_class_get_under_id(mrb, mrb_class_ptr(self), MRB_SYM(ObjectHandle)),
                  1, &data);

    auto* handle = (msgpack_object_handle*)mrb_data_get_ptr(mrb, object_handle, &msgpack_object_handle_type);
    if (unlikely(!handle)) {
      mrb_raise(mrb, E_MSGPACK_ERROR, "ObjectHandle is not initialized");
      return mrb_undef_value();
    }

    msgpack::unpack(handle->oh,
                    RSTRING_PTR(data),
                    RSTRING_LEN(data),
                    handle->off);

    return object_handle;
  }
  catch (const std::exception &e) {
    mrb_raisef(mrb, E_MSGPACK_ERROR,
               "Can't unpack: %S", mrb_str_new_cstr(mrb, e.what()));
  }

  return mrb_undef_value();
}

/* ------------------------------------------------------------------------
 * JSON Pointer navigation on ObjectHandle
 * ------------------------------------------------------------------------ */

static std::string_view
unescape_json_pointer_sv(std::string_view in, std::string &scratch)
{
  scratch.clear();
  scratch.reserve(in.size());

  for (size_t i = 0; i < in.size(); ++i) {
    char c = in[i];
    if (c == '~' && i + 1 < in.size()) {
      char n = in[i + 1];
      if (n == '0') { scratch.push_back('~'); ++i; continue; }
      if (n == '1') { scratch.push_back('/'); ++i; continue; }
    }
    scratch.push_back(c);
  }

  return std::string_view(scratch.data(), scratch.size());
}

static bool
parse_array_index(std::string_view token, size_t &out_idx, std::string &errmsg)
{
  if (token.empty()) {
    errmsg = "Empty array index";
    return false;
  }

  if (token[0] == '-' || token[0] == '+') {
    errmsg = "Invalid array index: " + std::string(token);
    return false;
  }

  size_t idx = 0;
  for (char c : token) {
    if (c < '0' || c > '9') {
      errmsg = "Invalid array index: " + std::string(token);
      return false;
    }
    size_t digit = static_cast<size_t>(c - '0');

    if (idx > (std::numeric_limits<size_t>::max() - digit) / 10) {
      errmsg = "Array index overflow: " + std::string(token);
      return false;
    }

    idx = idx * 10 + digit;
  }

  out_idx = idx;
  return true;
}

static mrb_value
mrb_msgpack_object_handle_at_pointer(mrb_state *mrb, mrb_value self)
{
  mrb_value str;
  mrb_get_args(mrb, "S", &str);

  std::string_view pointer(RSTRING_PTR(str), RSTRING_LEN(str));

  auto *handle = (msgpack_object_handle*)mrb_data_get_ptr(mrb, self, &msgpack_object_handle_type);
  if (unlikely(!handle)) {
    mrb_raise(mrb, E_MSGPACK_ERROR, "ObjectHandle is not initialized");
    return mrb_undef_value();
  }

  const msgpack::object *current = &handle->oh.get();

  if (pointer.empty() || pointer == "/") {
    return mrb_unpack_msgpack_obj(mrb, *current);
  }

  if (pointer.front() != '/') {
    mrb_raise(mrb, E_ARGUMENT_ERROR, "JSON Pointer must start with '/'");
    return mrb_undef_value();
  }
  pointer.remove_prefix(1);

  std::string scratch;
  std::string errmsg;

  while (!pointer.empty()) {
    size_t pos = pointer.find('/');
    std::string_view raw_token =
      (pos == std::string_view::npos) ? pointer : pointer.substr(0, pos);

    std::string_view token_view = unescape_json_pointer_sv(raw_token, scratch);

    if (current->type == msgpack::type::MAP) {
      bool found = false;

      for (uint32_t i = 0; i < current->via.map.size; ++i) {
        const auto &kv = current->via.map.ptr[i];

        if (kv.key.type == msgpack::type::STR) {
          std::string_view key_view(kv.key.via.str.ptr,
                                    kv.key.via.str.size);
          if (token_view == key_view) {
            current = &kv.val;
            found = true;
            break;
          }
        }
      }

      if (!found) {
        std::string msg = "Key not found: ";
        msg.append(token_view.data(), token_view.size());
        mrb_raise(mrb, E_KEY_ERROR, msg.c_str());
        return mrb_undef_value();
      }
    }
    else if (current->type == msgpack::type::ARRAY) {
      size_t idx = 0;
      errmsg.clear();

      if (!parse_array_index(token_view, idx, errmsg)) {
        mrb_raise(mrb, E_INDEX_ERROR, errmsg.c_str());
        return mrb_undef_value();
      }

      if (idx >= current->via.array.size) {
        std::string msg = "Invalid array index: ";
        msg.append(token_view.data(), token_view.size());
        mrb_raise(mrb, E_INDEX_ERROR, msg.c_str());
        return mrb_undef_value();
      }

      current = &current->via.array.ptr[idx];
    }
    else {
      mrb_raise(mrb, E_TYPE_ERROR, "Cannot navigate into non-container");
      return mrb_undef_value();
    }

    if (pos == std::string_view::npos) {
      break;
    }
    pointer.remove_prefix(pos + 1);
  }

  return mrb_unpack_msgpack_obj(mrb, *current);
}


/* ------------------------------------------------------------------------
 * Ext unpacker registration
 * ------------------------------------------------------------------------ */

static mrb_value
mrb_msgpack_register_unpack_type(mrb_state* mrb, mrb_value self)
{
  mrb_int type;
  mrb_value block = mrb_nil_value();

  mrb_get_args(mrb, "i&", &type, &block);

  if (type < 0 || type > 127) {
    mrb_raise(mrb, E_RANGE_ERROR, "ext type out of range");
  }
  if (mrb_nil_p(block)) {
    mrb_raise(mrb, E_ARGUMENT_ERROR, "no block given");
  }
  if (mrb_type(block) != MRB_TT_PROC) {
    mrb_raise(mrb, E_TYPE_ERROR, "not a block");
  }

  mrb_msgpack_ctx* ctx = MRB_MSGPACK_CONTEXT(mrb);

  // If the user is using an ext-based symbol strategy,
  // forbid overriding the symbol ext type.
  if (ctx->sym_unpacker != nullptr && type == ctx->ext_type) {
    mrb_raise(mrb, E_ARGUMENT_ERROR,
      "cannot register ext unpacker for Symbols, use MessagePack.sym_strategy instead.");
  }

  // Otherwise: safe to register
  mrb_hash_set(mrb,
               ext_unpackers_hash(mrb),
               mrb_convert_number(mrb, type),
               block);

  return mrb_nil_value();
}


static mrb_value
mrb_msgpack_ext_unpacker_registered(mrb_state *mrb, mrb_value self)
{
  mrb_int type;
  mrb_get_args(mrb, "i", &type);

  return mrb_bool_value(
    !mrb_nil_p(
      mrb_hash_get(mrb,
                   ext_unpackers_hash(mrb),
                   mrb_convert_number(mrb, type))
    )
  );
}

MRB_API void
mrb_msgpack_set_symbol_strategy(mrb_state *mrb, mrb_sym which, int8_t ext_type)
{
  mrb_msgpack_ctx *ctx = MRB_MSGPACK_CONTEXT(mrb);

  if (which == MRB_SYM(raw)) {
    ctx->sym_packer   = mrb_msgpack_pack_symbol_value_as_raw;
    ctx->sym_unpacker = NULL;
    ctx->ext_type     = 0;
    return;
  }

  if (which == MRB_SYM(string)) {
    ctx->sym_packer   = mrb_msgpack_pack_symbol_value_as_string;
    ctx->sym_unpacker = mrb_msgpack_unpack_symbol_as_string;
    ctx->ext_type     = ext_type;
    return;
  }

  if (which == MRB_SYM(int)) {
    ctx->sym_packer   = mrb_msgpack_pack_symbol_value_as_int;
    ctx->sym_unpacker = mrb_msgpack_unpack_symbol_as_int;
    ctx->ext_type     = ext_type;
    return;
  }

  mrb_raise(mrb, E_ARGUMENT_ERROR, "unknown symbol strategy");
}

MRB_API mrb_value
mrb_msgpack_get_symbol_strategy(mrb_state *mrb)
{
  mrb_msgpack_ctx *ctx = MRB_MSGPACK_CONTEXT(mrb);

  if (ctx->sym_unpacker == NULL) {
    return mrb_symbol_value(MRB_SYM(raw));
  }

  if (ctx->sym_unpacker == mrb_msgpack_unpack_symbol_as_string) {
    mrb_value ary = mrb_ary_new_capa(mrb, 2);
    mrb_ary_push(mrb, ary, mrb_symbol_value(MRB_SYM(string)));
    mrb_ary_push(mrb, ary, mrb_convert_number(mrb, ctx->ext_type));
    return ary;
  }

  if (ctx->sym_unpacker == mrb_msgpack_unpack_symbol_as_int) {
    mrb_value ary = mrb_ary_new_capa(mrb, 2);
    mrb_ary_push(mrb, ary, mrb_symbol_value(MRB_SYM(int)));
    mrb_ary_push(mrb, ary, mrb_convert_number(mrb, ctx->ext_type));
    return ary;
  }

  mrb_raise(mrb, E_MSGPACK_ERROR, "invalid symbol strategy state");
  return mrb_nil_value();
}


/* ------------------------------------------------------------------------
 * Symbol strategy API (Ruby-visible)
 * ------------------------------------------------------------------------ */

static mrb_value
mrb_msgpack_sym_strategy(mrb_state *mrb, mrb_value self)
{
  mrb_sym which;
  mrb_int ext_type = 0;

  mrb_int argc = mrb_get_args(mrb, "|ni", &which, &ext_type);

  if (argc == 0) {
    return mrb_msgpack_get_symbol_strategy(mrb);
  }

  mrb_msgpack_set_symbol_strategy(mrb, which, ext_type);
  return self;
}

class ClassCacheLfu {
public:
  static constexpr uint16_t NIL       = UINT16_MAX;
  static constexpr uint16_t MAX_FREQ  = 255;
  static constexpr uint16_t MAX_SIZE  = 128;
  static constexpr uint16_t INDEX_CAP = 256; // power-of-two
  static constexpr uint16_t KEY_MAX   = 64;  // max. Länge des Klassennamens

  struct Entry {
    char     key[KEY_MAX]; // eigene Kopie
    uint8_t  key_len;      // 0..64
    uint16_t freq;
    uint16_t prev;
    uint16_t next;
    // ~70–72 Bytes, 128 Einträge ≈ 9 KB
  };

  struct Bucket {
    uint16_t head = NIL;
    uint16_t tail = NIL;
  };

  struct Slot {
    uint32_t hash = 0;
    uint16_t idx  = NIL;
    bool     used = false;
  };


  Entry    entries[MAX_SIZE]{};
  uint16_t count = 0;

  Bucket   buckets[MAX_FREQ + 1]{};
  uint16_t min_freq = 1;

  Slot     index[INDEX_CAP]{};

  // für spätes Löschen im Ruby-Hash
  uint16_t last_evicted_idx = NIL;
  bool     had_eviction     = false;

  ClassCacheLfu()  = default;
  ~ClassCacheLfu() = default;

  void touch(uint16_t idx) {
    uint16_t old_freq = entries[idx].freq;
    bucket_remove(idx);

    if (buckets[old_freq].head == NIL && old_freq == min_freq) {
      if (min_freq < MAX_FREQ) min_freq++;
    }

    uint16_t new_freq = old_freq < MAX_FREQ ? old_freq + 1 : MAX_FREQ;
    bucket_insert(idx, new_freq);
  }

  void insert(const char* key_ptr, uint16_t key_len) {
    if (key_len > KEY_MAX) {
      return; // zu lang → nicht cachen
    }

    uint16_t idx;
    if (count < MAX_SIZE) {
      idx = count++;
    } else {
      idx = evict_one();
      if (idx == NIL) {
        mrb_assert(false && "LFU evict_one() returned NIL in insert");
        return; // Release: kein Cache-Eintrag, aber kein Crash
      }
    }

    Entry &e = entries[idx];
    std::memcpy(e.key, key_ptr, key_len);
    e.key_len = (uint8_t)key_len;
    e.freq    = 1;
    e.prev    = NIL;
    e.next    = NIL;
    min_freq  = 1;

    bucket_insert(idx, 1);
    index_set(e, idx);
  }


  uint16_t find(const char* key_ptr, uint16_t key_len) const {
    if (key_len > KEY_MAX) return NIL;

    uint32_t h    = hash(key_ptr, key_len);
    uint16_t mask = INDEX_CAP - 1;

    for (uint16_t i = 0; i < INDEX_CAP; ++i) {
      uint16_t slot = (h + i) & mask;
      const Slot &s = index[slot];

      if (!s.used) return NIL;
      if (s.hash == h) {
        const Entry &e = entries[s.idx];
        if (e.key_len == key_len &&
            std::memcmp(e.key, key_ptr, key_len) == 0) {
          return s.idx;
        }
      }
    }
    return NIL;
  }

  void evict(mrb_state *mrb, mrb_value class_cache) {
    if (!had_eviction || last_evicted_idx == NIL) return;

    Entry &e = entries[last_evicted_idx];
    mrb_value key_str = mrb_str_new_static(mrb, e.key, e.key_len);
    mrb_hash_delete_key(mrb, class_cache, key_str);

    had_eviction     = false;
    last_evicted_idx = NIL;
  }

private:
  static uint32_t hash(const char* p, uint16_t len) {
    uint32_t h = 2166136261u;
    for (uint16_t i = 0; i < len; ++i) {
      h ^= static_cast<unsigned char>(p[i]);
      h *= 16777619u;
    }
    return h;
  }

  void index_set(const Entry &e, uint16_t idx) {
    uint32_t h    = hash(e.key, e.key_len);
    uint16_t mask = INDEX_CAP - 1;

    int attempts = 0;

    for (;;) {
      for (uint16_t i = 0; i < INDEX_CAP; ++i) {
        uint16_t slot = (h + i) & mask;
        Slot &s = index[slot];

        if (!s.used || (s.hash == h && s.idx == idx)) {
          s.used = true;
          s.hash = h;
          s.idx  = idx;
          return;
        }
      }

      uint16_t victim = evict_one();
      if (victim == NIL || ++attempts > 4) {
        mrb_assert(false && "can't find free slot");
        return;
      }
    }
  }

  void index_erase(const Entry &e) {
    uint32_t h    = hash(e.key, e.key_len);
    uint16_t mask = INDEX_CAP - 1;

    for (uint16_t i = 0; i < INDEX_CAP; ++i) {
      uint16_t slot = (h + i) & mask;
      Slot &s = index[slot];

      if (!s.used) return;
      if (s.hash == h && s.idx == (&e - entries)) {
        s.used = false;
        return;
      }
    }
  }

  void bucket_remove(uint16_t idx) {
    Entry  &e = entries[idx];
    Bucket &b = buckets[e.freq];

    if (e.prev != NIL) entries[e.prev].next = e.next;
    else b.head = e.next;

    if (e.next != NIL) entries[e.next].prev = e.prev;
    else b.tail = e.prev;

    e.prev = e.next = NIL;
  }

  void bucket_insert(uint16_t idx, uint16_t freq) {
    Bucket &b = buckets[freq];
    Entry  &e = entries[idx];

    e.freq = freq;
    e.prev = b.tail;
    e.next = NIL;

    if (b.tail != NIL) entries[b.tail].next = idx;
    else b.head = idx;

    b.tail = idx;
  }

  uint16_t evict_one() {
    // Suche den nächsten nicht-leeren Bucket ab min_freq
    uint16_t f = min_freq;
    while (f <= MAX_FREQ && buckets[f].head == NIL) {
      ++f;
    }

    if (f > MAX_FREQ) {
      // Kein Eintrag zum Evicten → darf eigentlich nicht passieren,
      // aber wir schützen uns defensiv.
      return NIL;
    }

    min_freq = f;

    uint16_t idx = buckets[min_freq].head;
    if (idx == NIL) {
      return NIL; // zusätzliche Sicherung
    }

    Entry &e = entries[idx];

    bucket_remove(idx);
    index_erase(e);

    last_evicted_idx = idx;
    had_eviction     = true;

    return idx;
  }

};

MRB_CPP_DEFINE_TYPE(ClassCacheLfu, class_cache_lfu)

static mrb_value
class_cache_lfu_initialize(mrb_state *mrb, mrb_value self)
{
  mrb_cpp_new<ClassCacheLfu>(mrb, self);
  return self;
}

static inline ClassCacheLfu *
get_lfu(mrb_state *mrb)
{
  mrb_value obj = mrb_gv_get(mrb, MRB_SYM(__mrb_actor_class_lfu__));
  return static_cast<ClassCacheLfu*>(mrb_data_get_ptr(mrb, obj, &class_cache_lfu_type));
}

MRB_BEGIN_DECL
MRB_API mrb_value
mrb_str_constantize(mrb_state* mrb, mrb_value str)
{
  if (unlikely(!mrb_string_p(str))) {
    mrb_raise(mrb, E_TYPE_ERROR, "constant name must be a String");
  }

  using std::string_view;

  const char* ptr = RSTRING_PTR(str);
  mrb_int len     = RSTRING_LEN(str);
  string_view full(ptr, len);

  mrb_value class_cache =
      mrb_gv_get(mrb, MRB_SYM(__mrb_actor_class_cache__));

  ClassCacheLfu *lfu = get_lfu(mrb);

  mrb_value klass = mrb_hash_get(mrb, class_cache, str);

  if (mrb_class_p(klass)) {
    uint16_t idx = lfu->find(ptr, (uint16_t)len);
    if (idx != ClassCacheLfu::NIL) {
      lfu->touch(idx);
    } else {
      lfu->insert(ptr, (uint16_t)len);
    }
    return klass;
  }

  auto name_error = [&](string_view msg) {
    mrb_raisef(mrb, E_NAME_ERROR, msg.data(), str);
  };

  if (full.empty()) {
    name_error("wrong constant name %S");
  }

  if (full == "::") {
    name_error("wrong constant name %S");
  }

  mrb_value current = mrb_obj_value(mrb->object_class);

  if (full.size() >= 2 && full.substr(0, 2) == "::") {
    full.remove_prefix(2);
    if (full.empty()) {
      name_error("wrong constant name %S");
    }
  }

  std::vector<string_view> segments;
  size_t start = 0;

  while (start <= full.size()) {
    size_t pos = full.find("::", start);
    if (pos == string_view::npos) {
      segments.emplace_back(full.substr(start));
      break;
    }
    segments.emplace_back(full.substr(start, pos - start));
    start = pos + 2;
  }

  for (auto seg : segments) {
    if (seg.empty()) {
      name_error("wrong constant name %S");
    }
  }

  for (size_t i = 0; i < segments.size(); ++i) {
    string_view seg = segments[i];

    mrb_sym sym = mrb_intern(mrb, seg.data(),
                             static_cast<mrb_int>(seg.size()));

    if (!mrb_const_defined_at(mrb, current, sym)) {
      name_error("uninitialized constant %S");
    }

    mrb_value cnst = mrb_const_get(mrb, current, sym);

    bool last = (i + 1 == segments.size());
    if (!last) {
      enum mrb_vtype t = mrb_type(cnst);
      bool ok =
          (t == MRB_TT_CLASS  ||
           t == MRB_TT_MODULE ||
           t == MRB_TT_SCLASS ||
           t == MRB_TT_ICLASS);

      if (!ok) {
        mrb_raisef(mrb, E_TYPE_ERROR,
                   "%S does not refer to class/module",
                   mrb_str_new(mrb, seg.data(), seg.size()));
      }

      current = cnst;
    } else {
      mrb_hash_set(mrb, class_cache, str, cnst);

      lfu->insert(ptr, (uint16_t)len);
      lfu->evict(mrb, class_cache);

      return cnst;
    }
  }

  return current; // unreachable
}

/* ------------------------------------------------------------------------
 * Gem init/final: hook into GV-backed ctx/registry (Ruby API + C API)
 * ------------------------------------------------------------------------ */

void
mrb_mruby_simplemsgpack_gem_init(mrb_state* mrb)
{
  struct RClass *msgpack_mod, *mrb_object_handle_class;

  /* to_msgpack methods */
  mrb_define_method_id(mrb, mrb->object_class,
                       MRB_SYM(to_msgpack), mrb_msgpack_pack_object,  MRB_ARGS_NONE());

  mrb_define_method_id(mrb, mrb->string_class,
                       MRB_SYM(to_msgpack), mrb_msgpack_pack_string,  MRB_ARGS_NONE());

  mrb_define_method_id(mrb, mrb->array_class,
                       MRB_SYM(to_msgpack), mrb_msgpack_pack_array,   MRB_ARGS_NONE());

  mrb_define_method_id(mrb, mrb->hash_class,
                       MRB_SYM(to_msgpack), mrb_msgpack_pack_hash,    MRB_ARGS_NONE());

#ifndef MRB_WITHOUT_FLOAT
  mrb_define_method_id(mrb, mrb->float_class,
                       MRB_SYM(to_msgpack), mrb_msgpack_pack_float,   MRB_ARGS_NONE());
#endif

  mrb_define_method_id(mrb, mrb->integer_class,
                       MRB_SYM(to_msgpack), mrb_msgpack_pack_integer, MRB_ARGS_NONE());

  mrb_define_method_id(mrb, mrb->true_class,
                       MRB_SYM(to_msgpack), mrb_msgpack_pack_true,    MRB_ARGS_NONE());

  mrb_define_method_id(mrb, mrb->false_class,
                       MRB_SYM(to_msgpack), mrb_msgpack_pack_false,   MRB_ARGS_NONE());

  mrb_define_method_id(mrb, mrb->nil_class,
                       MRB_SYM(to_msgpack), mrb_msgpack_pack_nil,     MRB_ARGS_NONE());

  /* MessagePack module */
  msgpack_mod = mrb_define_module_id(mrb, MRB_SYM(MessagePack));

  mrb_define_class_under_id(mrb, msgpack_mod, MRB_SYM(Error), E_RUNTIME_ERROR);

  mrb_object_handle_class =
    mrb_define_class_under_id(mrb, msgpack_mod,
                              MRB_SYM(ObjectHandle), mrb->object_class);

  MRB_SET_INSTANCE_TT(mrb_object_handle_class, MRB_TT_DATA);

  mrb_define_method_id(mrb, mrb_object_handle_class,
                       MRB_SYM(initialize),  mrb_msgpack_object_handle_new,   MRB_ARGS_REQ(1));

  mrb_define_method_id(mrb, mrb_object_handle_class,
                       MRB_SYM(value),       mrb_msgpack_object_handle_value, MRB_ARGS_NONE());

  mrb_define_method_id(mrb, mrb_object_handle_class,
                       MRB_SYM(at_pointer),  mrb_msgpack_object_handle_at_pointer, MRB_ARGS_REQ(1));

  /* Constants */
  mrb_define_const_id(mrb, msgpack_mod,
                      MRB_SYM(LibMsgPackCVersion),
                      mrb_str_new_lit_frozen(mrb, MSGPACK_VERSION));

  /* Module functions */
  mrb_define_module_function_id(mrb, msgpack_mod,
                                MRB_SYM(pack),
                                mrb_msgpack_pack_m,
                                MRB_ARGS_REQ(1));

  mrb_define_module_function_id(mrb, msgpack_mod,
                                MRB_SYM(register_pack_type),
                                mrb_msgpack_register_pack_type,
                                MRB_ARGS_REQ(2) | MRB_ARGS_BLOCK());

  mrb_define_module_function_id(mrb, msgpack_mod,
                                MRB_SYM_Q(ext_packer_registered),
                                mrb_msgpack_ext_packer_registered,
                                MRB_ARGS_REQ(1));

  mrb_define_module_function_id(mrb, msgpack_mod,
                                MRB_SYM(unpack),
                                mrb_msgpack_unpack_m,
                                MRB_ARGS_REQ(1) | MRB_ARGS_BLOCK());

  mrb_define_module_function_id(mrb, msgpack_mod,
                                MRB_SYM(unpack_lazy),
                                mrb_msgpack_unpack_lazy_m,
                                MRB_ARGS_REQ(1));

  mrb_define_module_function_id(mrb, msgpack_mod,
                                MRB_SYM(register_unpack_type),
                                mrb_msgpack_register_unpack_type,
                                MRB_ARGS_REQ(1) | MRB_ARGS_BLOCK());

  mrb_define_module_function_id(mrb, msgpack_mod,
                                MRB_SYM_Q(ext_unpacker_registered),
                                mrb_msgpack_ext_unpacker_registered,
                                MRB_ARGS_REQ(1));

  mrb_define_module_function_id(mrb, msgpack_mod,
                                MRB_SYM(sym_strategy),
                                mrb_msgpack_sym_strategy,
                                MRB_ARGS_ARG(0,2));

  mrb_define_method_id(mrb,
                mrb->string_class,
                MRB_SYM(constantize),
                mrb_str_constantize,
                MRB_ARGS_NONE());

  /* Ensure GV-backed registry + ctx are initialized so Ruby and C APIs share state */
  mrb_msgpack_ensure(mrb);

  mrb_value class_cache = mrb_hash_new(mrb);
  mrb_gv_set(mrb, MRB_SYM(__mrb_actor_class_cache__), class_cache);

  struct RClass *lfu_class =
      mrb_define_class_under_id(mrb, msgpack_mod, MRB_SYM(__ClassCacheLfu), mrb->object_class);

  MRB_SET_INSTANCE_TT(lfu_class, MRB_TT_DATA);

  mrb_define_method_id(
      mrb, lfu_class, MRB_SYM(initialize),
      class_cache_lfu_initialize,
      MRB_ARGS_NONE());

  mrb_value lfu_obj = mrb_obj_new(mrb, lfu_class, 0, NULL);
  mrb_gv_set(mrb, MRB_SYM(__mrb_actor_class_lfu__), lfu_obj);
}

void
mrb_mruby_simplemsgpack_gem_final(mrb_state* mrb)
{
  /* Clean up GV-backed ctx/registry */
  mrb_msgpack_teardown(mrb);
}
MRB_END_DECL
