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
#include <cstring>

#include <string>
#include <string_view>
#include <atomic>
#include <cstdint>

#if ((defined(__has_builtin) && __has_builtin(__builtin_expect))||(__GNUC__ >= 3) || (__INTEL_COMPILER >= 800) || defined(__clang__))
#define likely(x)   __builtin_expect(!!(x), 1)
#define unlikely(x) __builtin_expect(!!(x), 0)
#else
#define likely(x)   (x)
#define unlikely(x) (x)
#endif

/* ------------------------------------------------------------------------
 * SBO writer
 * ------------------------------------------------------------------------ */

struct mrb_msgpack_sbo_writer {
  mrb_msgpack_sbo_writer(mrb_state* mrb)
    : mrb(mrb), heap_str(mrb_nil_value()) {}

  void write(const char* buf, size_t len) {
    if (!using_heap) {
      if (len <= STACK_CAP - size) {
        std::memcpy(stack_buf + size, buf, len);
        size += len;
        return;
      }

      using_heap = true;
      heap_str = mrb_str_new_capa(mrb, (mrb_int)(size + len) * 2);
      mrb_str_cat(mrb, heap_str, stack_buf, size);
    }

    mrb_str_cat(mrb, heap_str, buf, len);
  }

  mrb_value result() {
    if (!using_heap) {
      return mrb_str_new(mrb, stack_buf, size);
    }
    return heap_str;
  }

private:
  mrb_state* mrb;

  static constexpr size_t STACK_CAP = 8 * 1024;
  char   stack_buf[STACK_CAP];
  size_t size      = 0;
  bool   using_heap = false;

  mrb_value heap_str;
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

#define MRB_MSGPACK_CONTEXT(mrb) (static_cast<mrb_msgpack_ctx*>(mrb_cptr(mrb_gv_get((mrb), MRB_SYM(__msgpack__ctx)))))

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
  if (!mrb_nil_p(reg)) return reg;

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
  if (!mrb_nil_p(ctxv)) return ctxv;

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

static inline mrb_value
ext_packers_hash(mrb_state *mrb)
{
  mrb_value registry = ensure_ext_registry(mrb);
  return mrb_hash_get(mrb, registry, mrb_symbol_value(MRB_SYM(packers)));
}

static inline mrb_value
ext_unpackers_hash(mrb_state *mrb)
{
  mrb_value registry = ensure_ext_registry(mrb);
  return mrb_hash_get(mrb, registry, mrb_symbol_value(MRB_SYM(unpackers)));
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
  ensure_ext_registry(mrb);
  if (type < 0 || type > 127) mrb_raise(mrb, E_RANGE_ERROR, "ext type out of range");
  if (mrb_nil_p(proc) || mrb_type(proc) != MRB_TT_PROC) mrb_raise(mrb, E_TYPE_ERROR, "packer must be a Proc");

  mrb_value packers = ext_packers_hash(mrb);
  mrb_value cfg = mrb_hash_new_capa(mrb, 2);
  mrb_hash_set(mrb, cfg, mrb_symbol_value(MRB_SYM(type)),   mrb_int_value(mrb, type));
  mrb_hash_set(mrb, cfg, mrb_symbol_value(MRB_SYM(packer)), proc);
  mrb_hash_set(mrb, packers, klass, cfg);
}

MRB_API void
mrb_msgpack_register_unpack_type_value(mrb_state *mrb, int8_t type, mrb_value proc)
{
  ensure_ext_registry(mrb);
  if (type < 0 || type > 127) mrb_raise(mrb, E_RANGE_ERROR, "ext type out of range");
  if (mrb_nil_p(proc) || mrb_type(proc) != MRB_TT_PROC) mrb_raise(mrb, E_TYPE_ERROR, "unpacker must be a Proc");

  mrb_value unpackers = ext_unpackers_hash(mrb);
  mrb_hash_set(mrb, unpackers, mrb_int_value(mrb, type), proc);
}

MRB_API void
mrb_msgpack_register_pack_type_cfunc(mrb_state *mrb,
                                     int8_t type,
                                     struct RClass *klass,
                                     mrb_func_t cfunc,
                                     mrb_int argc,
                                     const mrb_value *argv)
{
  ensure_ext_registry(mrb);
  if (!klass) mrb_raise(mrb, E_ARGUMENT_ERROR, "klass is NULL");
  if (cfunc == NULL) {
    mrb_raise(mrb, E_ARGUMENT_ERROR, "pack callback cannot be NULL");
  }

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
  ensure_ext_registry(mrb);
  if (cfunc == NULL) {
    mrb_raise(mrb, E_ARGUMENT_ERROR, "unpack callback cannot be NULL");
  }

  struct RProc *rproc =
      mrb_proc_new_cfunc_with_env(mrb, cfunc, argc, argv);

  mrb_value proc = mrb_obj_value(rproc);

  mrb_msgpack_register_unpack_type_value(mrb, type, proc);
}


MRB_API void
mrb_msgpack_teardown(mrb_state *mrb)
{
  mrb_value ctxv = mrb_gv_get(mrb, MRB_SYM(__msgpack__ctx));
  if (!mrb_nil_p(ctxv)) {
    void *p = mrb_cptr(ctxv);
    if (p) mrb_free(mrb, p);
  }

  mrb_gv_set(mrb, MRB_SYM(__msgpack_ext_registry__), mrb_nil_value());
  mrb_gv_set(mrb, MRB_SYM(__msgpack__ctx), mrb_nil_value());
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
  /* use GV-backed registry instead of MessagePack::_ExtPackers constant */
  mrb_value ext_packers = ext_packers_hash(mrb);

  if (unlikely(!mrb_hash_p(ext_packers))) {
    mrb_raise(mrb, E_TYPE_ERROR, "ext packers is not a hash");
  }

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
mrb_msgpack_pack_ext_value(mrb_state* mrb, mrb_value self, msgpack::packer<mrb_msgpack_sbo_writer>& pk)
{
  mrb_int arena_index = mrb_gc_arena_save(mrb);

  mrb_value ext_config = mrb_msgpack_get_ext_config(mrb, self);
  if (!mrb_hash_p(ext_config)) {
    mrb_gc_arena_restore(mrb, arena_index);
    return FALSE;
  }

  mrb_value packer = mrb_hash_get(mrb, ext_config, mrb_symbol_value(MRB_SYM(packer)));
  if (unlikely(mrb_type(packer) != MRB_TT_PROC)) {
    mrb_gc_arena_restore(mrb, arena_index);
    mrb_raise(mrb, E_TYPE_ERROR, "malformed packer");
  }

  mrb_value packed = mrb_yield(mrb, packer, self);
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

  mrb_hash_set(mrb, ext_config, mrb_symbol_value(MRB_SYM(type)),   mrb_int_value(mrb, type));
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
        mrb_raisef(mrb, E_MSGPACK_ERROR, "Cannot unpack ext type %S", ext_type);
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
      return mrb_int_value(mrb, (mrb_int)off);
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
unescape_json_pointer_sv(std::string_view s, std::string &scratch)
{
  scratch.clear();
  scratch.reserve(s.size());

  for (size_t i = 0; i < s.size(); ++i) {
    if (s[i] == '~' && i + 1 < s.size()) {
      if (s[i + 1] == '0') { scratch.push_back('~'); ++i; continue; }
      if (s[i + 1] == '1') { scratch.push_back('/'); ++i; continue; }
    }
    scratch.push_back(s[i]);
  }

  return std::string_view(scratch);
}

static mrb_value
mrb_msgpack_object_handle_at_pointer(mrb_state *mrb, mrb_value self)
{
  mrb_value str;
  mrb_get_args(mrb, "S", &str);

  std::string_view pointer(RSTRING_PTR(str), RSTRING_LEN(str));

  auto* handle = (msgpack_object_handle*)mrb_data_get_ptr(mrb, self, &msgpack_object_handle_type);
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
  pointer.remove_prefix(1); /* skip leading '/' */

  std::string scratch;

  while (!pointer.empty()) {
    size_t pos = pointer.find('/');
    std::string_view token_view =
      (pos == std::string_view::npos) ? pointer : pointer.substr(0, pos);

    token_view = unescape_json_pointer_sv(token_view, scratch);

    if (current->type == msgpack::type::MAP) {
      bool found = false;

      for (uint32_t i = 0; i < current->via.map.size; ++i) {
        const auto &kv = current->via.map.ptr[i];

        if (kv.key.type == msgpack::type::STR &&
            token_view == std::string_view(kv.key.via.str.ptr,
                                           kv.key.via.str.size)) {
          current = &kv.val;
          found = true;
          break;
        }
      }

      if (!found) {
        mrb_raise(mrb, E_KEY_ERROR,
                  ("Key not found: " + std::string(token_view)).c_str());
        return mrb_undef_value();
      }
    }
    else if (current->type == msgpack::type::ARRAY) {
      long idx = 0;

      for (char c : token_view) {
        if (c < '0' || c > '9') {
          mrb_raise(mrb, E_INDEX_ERROR,
                    ("Invalid array index: " + std::string(token_view)).c_str());
          return mrb_undef_value();
        }
        idx = idx * 10 + (c - '0');
      }

      if (idx < 0 || (size_t)idx >= current->via.array.size) {
        mrb_raise(mrb, E_INDEX_ERROR,
                  ("Invalid array index: " + std::string(token_view)).c_str());
        return mrb_undef_value();
      }

      current = &current->via.array.ptr[idx];
    }
    else {
      mrb_raise(mrb, E_TYPE_ERROR, "Cannot navigate into non-container");
      return mrb_undef_value();
    }

    if (pos == std::string_view::npos) break;
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
               mrb_int_value(mrb, type),
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
                   mrb_int_value(mrb, type))
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
    mrb_ary_push(mrb, ary, mrb_int_value(mrb, ctx->ext_type));
    return ary;
  }

  if (ctx->sym_unpacker == mrb_msgpack_unpack_symbol_as_int) {
    mrb_value ary = mrb_ary_new_capa(mrb, 2);
    mrb_ary_push(mrb, ary, mrb_symbol_value(MRB_SYM(int)));
    mrb_ary_push(mrb, ary, mrb_int_value(mrb, ctx->ext_type));
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

    auto name_error = [&](string_view msg) {
        mrb_raisef(mrb, E_NAME_ERROR, msg.data(), str);
    };

    // Reject empty
    if (full.empty()) {
        name_error("wrong constant name %S");
    }

    // Reject "::"
    if (full == "::") {
        name_error("wrong constant name %S");
    }

    // Start from Object
    mrb_value current = mrb_obj_value(mrb->object_class);

    // Handle leading "::"
    if (full.size() >= 2 && full.substr(0, 2) == "::") {
        full.remove_prefix(2);
        if (full.empty()) {
            name_error("wrong constant name %S");
        }
    }

    // Split into segments, preserving empty ones
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

    // Reject empty segments (including final)
    for (auto seg : segments) {
        if (seg.empty()) {
            name_error("wrong constant name %S");
        }
    }

    // Resolve each segment
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
                      mrb_str_new_lit(mrb, MSGPACK_VERSION));

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
}

void
mrb_mruby_simplemsgpack_gem_final(mrb_state* mrb)
{
  /* Clean up GV-backed ctx/registry */
  mrb_msgpack_teardown(mrb);
}
MRB_END_DECL
