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
MRB_BEGIN_DECL
#include <mruby/internal.h>
MRB_END_DECL
#include <mruby/gc.h>
#include <mruby/cpp_helpers.hpp>
#include <mruby/num_helpers.hpp>
#include <string>
#include <sstream>

#if ((defined(__has_builtin) && __has_builtin(__builtin_expect))||(__GNUC__ >= 3) || (__INTEL_COMPILER >= 800) || defined(__clang__))
#define likely(x) __builtin_expect(!!(x), 1)
#define unlikely(x) __builtin_expect(!!(x), 0)
#else
#define likely(x) (x)
#define unlikely(x) (x)
#endif

template <typename Packer> static void mrb_msgpack_pack_value(mrb_state* mrb, mrb_value self, Packer& pk);
template <typename Packer> static void mrb_msgpack_pack_array_value(mrb_state* mrb, mrb_value self, Packer& pk);
template <typename Packer> static void mrb_msgpack_pack_hash_value(mrb_state* mrb, mrb_value self, Packer& pk);

#define pack_integer_helper_(x, pk, self) pk.pack_int##x(static_cast<int##x##_t>(mrb_integer(self)))
#define pack_integer_helper(x, pk, self)  pack_integer_helper_(x, pk, self)
#define mrb_msgpack_pack_int(pk, self)    pack_integer_helper(MRB_INT_BIT, pk, self)

template <typename Packer>
static inline void
mrb_msgpack_pack_integer_value(mrb_state *mrb, mrb_value self, Packer& pk)
{
  mrb_msgpack_pack_int(pk, self);
}

#ifndef MRB_WITHOUT_FLOAT
template <typename Packer>
static inline void mrb_msgpack_pack_float_value(mrb_state *mrb, mrb_value self, Packer& pk) {
#ifdef MRB_USE_FLOAT
  pk.pack_float(mrb_float(self));
#else
  pk.pack_double(mrb_float(self));
#endif
}
#endif

template <typename Packer>
static inline void mrb_msgpack_pack_string_value(mrb_state *mrb, mrb_value self, Packer& pk) {
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

#ifdef MRB_MSGPACK_SYMBOLS

#ifndef MRB_MSGPACK_SYMBOLS_EXT
#define MRB_MSGPACK_SYMBOLS_EXT 0
#endif

template <typename Packer>
static inline void
mrb_msgpack_pack_symbol_value(mrb_state* mrb, mrb_value self, Packer& pk)
{
  mrb_sym sym = mrb_symbol(self);

#ifdef MRB_MSGPACK_SYMBOLS_AS_INT
  // Pack the raw mrb_sym value as the ext body
  pk.pack_ext(sizeof(sym), static_cast<int8_t>(MRB_MSGPACK_SYMBOLS_EXT));
  pk.pack_ext_body(reinterpret_cast<const char*>(&sym), sizeof(sym));

#else
  // Pack the symbol’s name as the ext body, using mrb_sym_name_len
  mrb_int len;
  const char* name = mrb_sym_name_len(mrb, sym, &len);

  pk.pack_ext(static_cast<uint32_t>(len),
              static_cast<int8_t>(MRB_MSGPACK_SYMBOLS_EXT));
  pk.pack_ext_body(name, static_cast<size_t>(len));

#endif
}

#endif // MRB_MSGPACK_SYMBOLS

template <typename Packer>
static void mrb_msgpack_pack_array_value(mrb_state* mrb, mrb_value self, Packer& pk) {
  mrb_int n = RARRAY_LEN(self);
  mrb_int arena_index = mrb_gc_arena_save(mrb);
  pk.pack_array(static_cast<uint32_t>(n));
  for (mrb_int i = 0; i < n; ++i) {
    mrb_msgpack_pack_value(mrb, mrb_ary_ref(mrb, self, i), pk);
    mrb_gc_arena_restore(mrb, arena_index);
  }
}

template <typename Packer>
static void mrb_msgpack_pack_hash_value(mrb_state* mrb, mrb_value self, Packer& pk) {
  uint32_t n = static_cast<uint32_t>(mrb_hash_size(mrb, self));
  pk.pack_map(n);

  mrb_int arena_index = mrb_gc_arena_save(mrb);

  struct Ctx {
    mrb_state* mrb;
    Packer* pk;
    mrb_int arena_index;
  } ctx{mrb, &pk, arena_index};

  mrb_hash_foreach(mrb, mrb_hash_ptr(self),
    [](mrb_state* mrb, mrb_value key, mrb_value val, void* data) -> int {
      auto* c = reinterpret_cast<Ctx*>(data);

      mrb_msgpack_pack_value(mrb, key, *c->pk);
      mrb_msgpack_pack_value(mrb, val, *c->pk);

      mrb_gc_arena_restore(mrb, c->arena_index);
      return 0;
    },
    &ctx
  );
}

static mrb_value
mrb_msgpack_get_ext_config(mrb_state* mrb, mrb_value obj)
{
  mrb_value ext_packers = mrb_const_get(
    mrb,
    mrb_obj_value(mrb_module_get_id(mrb, MRB_SYM(MessagePack))),
    MRB_SYM(_ExtPackers)
  );

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
    mrb_value  obj;
    mrb_value  found;
  } ctx = { obj, mrb_nil_value() };

  /* Search for a superclass match */
  mrb_hash_foreach(mrb, mrb_hash_ptr(ext_packers),
    [](mrb_state* mrb, mrb_value key, mrb_value val, void* data) -> int {
      struct Ctx* c = (struct Ctx*)data;
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

template <typename Packer>
static mrb_bool mrb_msgpack_pack_ext_value(mrb_state* mrb, mrb_value self, Packer& pk) {
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

template <typename Packer>
static void mrb_msgpack_pack_value(mrb_state* mrb, mrb_value self, Packer& pk) {
  switch (mrb_type(self)) {
    case MRB_TT_FALSE:
      if (!mrb_integer(self)) pk.pack_nil();
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
#ifdef MRB_MSGPACK_SYMBOLS
    case MRB_TT_SYMBOL:
      mrb_msgpack_pack_symbol_value(mrb, self, pk);
      break;
#endif
    default: {
      if (mrb_msgpack_pack_ext_value(mrb, self, pk)) break;

      mrb_value v;
      v = mrb_type_convert_check(mrb, self, MRB_TT_HASH, MRB_SYM(to_hash));
      if (mrb_hash_p(v)) { mrb_msgpack_pack_hash_value(mrb, v, pk); break; }

      v = mrb_type_convert_check(mrb, self, MRB_TT_ARRAY, MRB_SYM(to_ary));
      if (mrb_array_p(v)) { mrb_msgpack_pack_array_value(mrb, v, pk); break; }

      v = mrb_type_convert_check(mrb, self, MRB_TT_INTEGER, MRB_SYM(to_int));
      if (mrb_integer_p(v)) { mrb_msgpack_pack_integer_value(mrb, v, pk); break; }

      v = mrb_type_convert_check(mrb, self, MRB_TT_STRING, MRB_SYM(to_str));
      if (mrb_string_p(v)) { mrb_msgpack_pack_string_value(mrb, v, pk); break; }

      v = mrb_type_convert(mrb, self, MRB_TT_STRING, MRB_SYM(to_s));
      mrb_msgpack_pack_string_value(mrb, v, pk);
      break;
    }
  }
}

#define DEFINE_MSGPACK_PACKER(FUNC_NAME, PACK_FN)                           \
static mrb_value FUNC_NAME(mrb_state* mrb, mrb_value self) {                \
  bool retried = false;                                                     \
retry:                                                                      \
  try {                                                                     \
    msgpack::sbuffer sbuf;                                                  \
    msgpack::packer<msgpack::sbuffer> pk(&sbuf);                            \
    PACK_FN(mrb, self, pk);                                                 \
    return mrb_str_new(mrb, sbuf.data(), sbuf.size());                      \
  } catch (const std::bad_alloc&) {                                         \
    if (!retried) {                                                         \
      retried = true;                                                       \
      mrb_full_gc(mrb);                                                     \
      goto retry;                                                           \
    }                                                                       \
    mrb_exc_raise(mrb, mrb_obj_value(mrb->nomem_err));                      \
  } catch (const std::exception& e) {                                       \
    mrb_raisef(mrb, E_MSGPACK_ERROR,                                        \
               "cannot pack object: %S",                                    \
               mrb_str_new_cstr(mrb, e.what()));                            \
  }                                                                         \
  return mrb_undef_value();                                                 \
}

DEFINE_MSGPACK_PACKER(mrb_msgpack_pack_object,  mrb_msgpack_pack_value)
DEFINE_MSGPACK_PACKER(mrb_msgpack_pack_string,  mrb_msgpack_pack_string_value)
DEFINE_MSGPACK_PACKER(mrb_msgpack_pack_array,   mrb_msgpack_pack_array_value)
DEFINE_MSGPACK_PACKER(mrb_msgpack_pack_hash,    mrb_msgpack_pack_hash_value)
DEFINE_MSGPACK_PACKER(mrb_msgpack_pack_integer, mrb_msgpack_pack_integer_value)
#ifndef MRB_WITHOUT_FLOAT
DEFINE_MSGPACK_PACKER(mrb_msgpack_pack_float,   mrb_msgpack_pack_float_value)
#endif

static inline void pack_true_fn(mrb_state*, mrb_value, msgpack::packer<msgpack::sbuffer>& pk) {
  pk.pack_true();
}
static inline void pack_false_fn(mrb_state*, mrb_value, msgpack::packer<msgpack::sbuffer>& pk) {
  pk.pack_false();
}
static inline void pack_nil_fn(mrb_state*, mrb_value, msgpack::packer<msgpack::sbuffer>& pk) {
  pk.pack_nil();
}

DEFINE_MSGPACK_PACKER(mrb_msgpack_pack_true,  pack_true_fn)
DEFINE_MSGPACK_PACKER(mrb_msgpack_pack_false, pack_false_fn)
DEFINE_MSGPACK_PACKER(mrb_msgpack_pack_nil,   pack_nil_fn)


// --- Public API: pack helpers ---
MRB_API mrb_value
mrb_msgpack_pack(mrb_state *mrb, mrb_value object)
{
  try {
    msgpack::sbuffer sbuf;
    msgpack::packer<msgpack::sbuffer> pk(&sbuf);
    mrb_msgpack_pack_value(mrb, object, pk);
    return mrb_str_new(mrb, sbuf.data(), sbuf.size());
  }
  catch (const std::exception &e) {
    mrb_raise(mrb, E_MSGPACK_ERROR, e.what());
  }
  return mrb_undef_value(); // not reached
}

MRB_API mrb_value
mrb_msgpack_pack_argv(mrb_state *mrb, mrb_value *argv, mrb_int argv_len)
{
  try {
    msgpack::sbuffer sbuf;
    msgpack::packer<msgpack::sbuffer> pk(&sbuf);
    pk.pack_array(static_cast<uint32_t>(argv_len));
    for (mrb_int i = 0; i < argv_len; ++i)
      mrb_msgpack_pack_value(mrb, argv[i], pk);
    return mrb_str_new(mrb, sbuf.data(), sbuf.size());
  }
  catch (const std::exception &e) {
    mrb_raise(mrb, E_MSGPACK_ERROR, e.what());
  }
  return mrb_undef_value();
}

static mrb_value
mrb_msgpack_pack_m(mrb_state *mrb, mrb_value self)
{
  mrb_value object;
  mrb_get_args(mrb, "o", &object);
  return mrb_msgpack_pack(mrb, object);
}

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

#ifdef MRB_MSGPACK_SYMBOLS
    // Prevent registering ext packers for Symbol when built-in symbol packing is enabled
    if (mrb_class_ptr(mrb_class) == mrb->symbol_class) {
      mrb_raise(mrb, E_ARGUMENT_ERROR, "cannot register ext packer for Symbol when MRB_MSGPACK_SYMBOLS is enabled");
    }
#endif

    ext_packers = mrb_const_get(mrb, self, MRB_SYM(_ExtPackers));
    ext_config = mrb_hash_new_capa(mrb, 2);
    mrb_hash_set(mrb, ext_config, mrb_symbol_value(MRB_SYM(type)), mrb_int_value(mrb, type));
    mrb_hash_set(mrb, ext_config, mrb_symbol_value(MRB_SYM(packer)), block);
    mrb_hash_set(mrb, ext_packers, mrb_class, ext_config);

    return mrb_nil_value();
}

static mrb_value
mrb_msgpack_ext_packer_registered(mrb_state *mrb, mrb_value self)
{
    mrb_value mrb_class;
    mrb_get_args(mrb, "C", &mrb_class);

    return mrb_bool_value(mrb_test(mrb_hash_get(mrb, mrb_const_get(mrb, self, MRB_SYM(_ExtPackers)), mrb_class)));
}

// Forward decls
static mrb_value mrb_unpack_msgpack_obj(mrb_state* mrb, const msgpack::object& obj);
static mrb_value mrb_unpack_msgpack_obj_array(mrb_state* mrb, const msgpack::object& obj);
static mrb_value mrb_unpack_msgpack_obj_map(mrb_state* mrb, const msgpack::object& obj);

// --- Core dispatch ---
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
#ifndef MRB_WITHOUT_FLOAT
    case msgpack::type::FLOAT32:
    case msgpack::type::FLOAT64:
      return mrb_convert_number(mrb, obj.via.f64);
#endif
    case msgpack::type::STR:
      return mrb_str_new(mrb, obj.via.str.ptr, obj.via.str.size);
    case msgpack::type::BIN:
      return mrb_str_new(mrb, obj.via.bin.ptr, obj.via.bin.size);
    case msgpack::type::ARRAY:
      return mrb_unpack_msgpack_obj_array(mrb, obj);
    case msgpack::type::MAP:
      return mrb_unpack_msgpack_obj_map(mrb, obj);
    case msgpack::type::EXT: {
        auto ext_type = mrb_convert_number(mrb, obj.via.ext.type());
#ifdef MRB_MSGPACK_SYMBOLS
        if (ext_type == MRB_MSGPACK_SYMBOLS_EXT) {
# ifdef MRB_MSGPACK_SYMBOLS_AS_INT
            // Body is a raw mrb_sym
            if (unlikely(obj.via.ext.size != sizeof(mrb_sym))) {
              mrb_raise(mrb, E_MSGPACK_ERROR, "invalid symbol ext body size");
            }
            mrb_sym sym;
            std::memcpy(&sym, obj.via.ext.data(), sizeof(sym));
            return mrb_symbol_value(sym);
# else
            // Body is the UTF‑8 symbol name
            return mrb_symbol_value(
                mrb_intern(mrb,
                                obj.via.ext.data(),
                                (size_t)obj.via.ext.size));
# endif
        }
#endif // MRB_MSGPACK_SYMBOLS
        mrb_value unpacker = mrb_hash_get(mrb,
            mrb_const_get(mrb, mrb_obj_value(mrb_module_get_id(mrb, MRB_SYM(MessagePack))), MRB_SYM(_ExtUnpackers)), ext_type);
        if (likely(mrb_type(unpacker) == MRB_TT_PROC)) {
            return mrb_yield(mrb, unpacker, mrb_str_new(mrb, obj.via.ext.data(), obj.via.ext.size));
        } else {
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

MRB_API mrb_value
mrb_msgpack_unpack(mrb_state *mrb, mrb_value data)
{
  data = mrb_str_to_str(mrb, data);
  try {
    msgpack::object_handle oh = msgpack::unpack(RSTRING_PTR(data), RSTRING_LEN(data));
    return mrb_unpack_msgpack_obj(mrb, oh.get());
  }
  catch (const std::exception &e) {
    mrb_raisef(mrb, E_MSGPACK_ERROR, "Can't unpack: %S", mrb_str_new_cstr(mrb, e.what()));
  }
  return mrb_undef_value();
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
    } else {
      msgpack::object_handle oh = msgpack::unpack(buf, len, off);
      return mrb_unpack_msgpack_obj(mrb, oh.get());
    }
  }
  catch (const std::exception &e) {
    mrb_raisef(mrb, E_MSGPACK_ERROR, "Can't unpack: %S", mrb_str_new_cstr(mrb, e.what()));
  }
  return mrb_undef_value();
}

struct msgpack_object_handle {
  msgpack::object_handle oh;
  std::size_t off;

  msgpack_object_handle() : oh(msgpack::object_handle()), off(0) {}

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
  msgpack_object_handle* handle = static_cast<msgpack_object_handle*>(DATA_PTR(self));
  if (unlikely(!handle)) {
    mrb_raise(mrb, E_MSGPACK_ERROR, "ObjectHandle is not initialized");
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
    mrb_value object_handle = mrb_obj_new(mrb, mrb_class_get_under_id(mrb, mrb_class_ptr(self), MRB_SYM(ObjectHandle)), 1, &data);
    msgpack_object_handle* handle = static_cast<msgpack_object_handle*>(DATA_PTR(object_handle));
    msgpack::unpack(handle->oh, RSTRING_PTR(data), RSTRING_LEN(data), handle->off);

    return object_handle;
  }
  catch (const std::exception &e) {
    mrb_raisef(mrb, E_MSGPACK_ERROR, "Can't unpack: %S", mrb_str_new_cstr(mrb, e.what()));
  }
  return mrb_undef_value();
}

static std::string_view
unescape_json_pointer_sv(std::string_view s, std::string &scratch) {
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

    auto *handle = static_cast<msgpack_object_handle*>(DATA_PTR(self));
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
    pointer.remove_prefix(1); // skip leading '/'

    std::string scratch; // reused buffer for unescaping

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
                    token_view == std::string_view(kv.key.via.str.ptr, kv.key.via.str.size)) {
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
            if (idx < 0 || static_cast<size_t>(idx) >= current->via.array.size) {
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

#ifdef MRB_MSGPACK_SYMBOLS
    /* Prevent registering an unpacker for the reserved symbol ext type */
    if (type == MRB_MSGPACK_SYMBOLS_EXT) {
      mrb_raise(mrb, E_ARGUMENT_ERROR,
                "cannot register unpacker for Symbol ext type when MRB_MSGPACK_SYMBOLS is enabled");
    }
#endif

    mrb_hash_set(mrb, mrb_const_get(mrb, self, MRB_SYM(_ExtUnpackers)), mrb_int_value(mrb, type), block);

    return mrb_nil_value();
}

static mrb_value
mrb_msgpack_ext_unpacker_registered(mrb_state *mrb, mrb_value self)
{
    mrb_int type;
    mrb_get_args(mrb, "i", &type);

    return mrb_bool_value(!mrb_nil_p(mrb_hash_get(mrb, mrb_const_get(mrb, self, MRB_SYM(_ExtUnpackers)), mrb_int_value(mrb, type))));
}

MRB_BEGIN_DECL
void
mrb_mruby_simplemsgpack_gem_init(mrb_state* mrb)
{
  struct RClass *msgpack_mod, *mrb_object_handle_class;

  /* to_msgpack methods */
  mrb_define_method_id(mrb, mrb->object_class,
                       MRB_SYM(to_msgpack), mrb_msgpack_pack_object, MRB_ARGS_NONE());

  mrb_define_method_id(mrb, mrb->string_class,
                       MRB_SYM(to_msgpack), mrb_msgpack_pack_string, MRB_ARGS_NONE());

  mrb_define_method_id(mrb, mrb->array_class,
                       MRB_SYM(to_msgpack), mrb_msgpack_pack_array, MRB_ARGS_NONE());

  mrb_define_method_id(mrb, mrb->hash_class,
                       MRB_SYM(to_msgpack), mrb_msgpack_pack_hash, MRB_ARGS_NONE());

#ifndef MRB_WITHOUT_FLOAT
  mrb_define_method_id(mrb, mrb->float_class,
                       MRB_SYM(to_msgpack), mrb_msgpack_pack_float, MRB_ARGS_NONE());
#endif

  mrb_define_method_id(mrb, mrb->integer_class,
                       MRB_SYM(to_msgpack), mrb_msgpack_pack_integer, MRB_ARGS_NONE());

  mrb_define_method_id(mrb, mrb->true_class,
                       MRB_SYM(to_msgpack), mrb_msgpack_pack_true, MRB_ARGS_NONE());

  mrb_define_method_id(mrb, mrb->false_class,
                       MRB_SYM(to_msgpack), mrb_msgpack_pack_false, MRB_ARGS_NONE());

  mrb_define_method_id(mrb, mrb->nil_class,
                       MRB_SYM(to_msgpack), mrb_msgpack_pack_nil, MRB_ARGS_NONE());

  /* MessagePack module */
  msgpack_mod = mrb_define_module_id(mrb, MRB_SYM(MessagePack));

  mrb_define_class_under_id(mrb, msgpack_mod, MRB_SYM(Error), E_RUNTIME_ERROR);

  mrb_object_handle_class =
    mrb_define_class_under_id(mrb, msgpack_mod,
                              MRB_SYM(ObjectHandle), mrb->object_class);

  MRB_SET_INSTANCE_TT(mrb_object_handle_class, MRB_TT_DATA);

  mrb_define_method_id(mrb, mrb_object_handle_class,
                       MRB_SYM(initialize), mrb_msgpack_object_handle_new, MRB_ARGS_REQ(1));

  mrb_define_method_id(mrb, mrb_object_handle_class,
                       MRB_SYM(value), mrb_msgpack_object_handle_value, MRB_ARGS_NONE());

  mrb_define_method_id(mrb, mrb_object_handle_class,
                       MRB_SYM(at_pointer), mrb_msgpack_object_handle_at_pointer, MRB_ARGS_REQ(1));

  /* Constants */
  mrb_define_const_id(mrb, msgpack_mod,
                      MRB_SYM(LibMsgPackCVersion),
                      mrb_str_new_lit(mrb, MSGPACK_VERSION));

  mrb_define_const_id(mrb, msgpack_mod,
                      MRB_SYM(_ExtPackers), mrb_hash_new(mrb));

  mrb_define_const_id(mrb, msgpack_mod,
                      MRB_SYM(_ExtUnpackers), mrb_hash_new(mrb));

  /* Module functions */
  mrb_define_module_function_id(mrb, msgpack_mod,
                                MRB_SYM(pack), mrb_msgpack_pack_m, MRB_ARGS_REQ(1));

  mrb_define_module_function_id(mrb, msgpack_mod,
                                MRB_SYM(register_pack_type),
                                mrb_msgpack_register_pack_type,
                                MRB_ARGS_REQ(2) | MRB_ARGS_BLOCK());

  mrb_define_module_function_id(mrb, msgpack_mod, MRB_SYM_Q(ext_packer_registered), mrb_msgpack_ext_packer_registered, MRB_ARGS_REQ(1));

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

  mrb_define_module_function_id(mrb, msgpack_mod, MRB_SYM_Q(ext_unpacker_registered), mrb_msgpack_ext_unpacker_registered, MRB_ARGS_REQ(1));
}


void mrb_mruby_simplemsgpack_gem_final(mrb_state* mrb) {}
MRB_END_DECL
