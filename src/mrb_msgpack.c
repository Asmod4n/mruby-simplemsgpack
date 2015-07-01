#include <msgpack.h>
#include <mruby.h>
#include <mruby/array.h>
#include <mruby/class.h>
#include <mruby/data.h>
#include <mruby/hash.h>
#include <mruby/string.h>
#include <mruby/throw.h>
#include <mruby/variable.h>
#include "mruby/msgpack.h"
#include "is_utf8.h"

typedef struct {
  mrb_state *mrb;
  mrb_value buffer;
} mrb_msgpack_buffer;

static inline int
mrb_msgpack_buffer_write(void *data, const char *buf, size_t len)
{
  mrb_str_cat(((mrb_msgpack_buffer *) data)->mrb, ((mrb_msgpack_buffer *) data)->buffer, buf, len);
  return 0;
}

static inline void
mrb_msgpack_pack_value(mrb_state *mrb, mrb_value self, msgpack_packer *pk)
{
  if (mrb_fixnum_p(self)) {
#if defined(MRB_INT16)
    msgpack_pack_int16(pk, mrb_int(mrb, self));
#elif defined(MRB_INT64)
    msgpack_pack_int64(pk, mrb_int(mrb, self));
#else
    msgpack_pack_int32(pk, mrb_int(mrb, self));
#endif
  }
  else
  if (mrb_nil_p(self))
    msgpack_pack_nil(pk);
  else
  if (mrb_float_p(self)) {
#if defined(MRB_USE_FLOAT)
    msgpack_pack_float(pk, mrb_float(self));
#else
    msgpack_pack_double(pk, mrb_float(self));
#endif
  }
  else
  if (mrb_symbol_p(self)) {
    mrb_int len;
    const char *name = mrb_sym2name_len(mrb, mrb_symbol(self), &len);
    if (is_utf8((unsigned char *) name, (size_t) len) == 0) {
      msgpack_pack_str(pk, (size_t) len);
      msgpack_pack_str_body(pk, name, (size_t) len);
    } else {
      msgpack_pack_bin(pk, (size_t) len);
      msgpack_pack_bin_body(pk, name, (size_t) len);
    }
  }
  else
  if (mrb_array_p(self)) {
    msgpack_pack_array(pk, (size_t) RARRAY_LEN(self));
    for (mrb_int i = 0; i < RARRAY_LEN(self); i++)
      mrb_msgpack_pack_value(mrb, mrb_ary_ref(mrb, self, i), pk);
  }
  else
  if (mrb_string_p(self)) {
    if (is_utf8((unsigned char *) RSTRING_PTR(self), (size_t) RSTRING_LEN(self)) == 0) {
      msgpack_pack_str(pk, (size_t) RSTRING_LEN(self));
      msgpack_pack_str_body(pk, RSTRING_PTR(self), (size_t) RSTRING_LEN(self));
    } else {
      msgpack_pack_bin(pk, (size_t) RSTRING_LEN(self));
      msgpack_pack_bin_body(pk, RSTRING_PTR(self), (size_t) RSTRING_LEN(self));
    }
  }
  else
  if (mrb_hash_p(self)) {
    mrb_value keys = mrb_hash_keys(mrb, self);
    msgpack_pack_map(pk, (size_t) RARRAY_LEN(keys));
    for (mrb_int i = 0; i < RARRAY_LEN(keys); i++) {
      mrb_value key = mrb_ary_ref(mrb, keys, i);
      mrb_msgpack_pack_value(mrb, key, pk);
      mrb_msgpack_pack_value(mrb, mrb_hash_get(mrb, self, key), pk);
    }
  }
  else
  if (mrb_type(self) == MRB_TT_TRUE)
    msgpack_pack_true(pk);
  else
  if (mrb_type(self) == MRB_TT_FALSE)
    msgpack_pack_false(pk);
  else {
    mrb_value s;
    s = mrb_check_convert_type(mrb, self, MRB_TT_ARRAY, "Array", "to_ary");
    if (mrb_array_p(s))
      mrb_msgpack_pack_value(mrb, s, pk);
    else {
      s = mrb_check_convert_type(mrb, self, MRB_TT_HASH, "Hash", "to_hash");
      if (mrb_hash_p(s))
        mrb_msgpack_pack_value(mrb, s, pk);
      else {
        s = mrb_check_convert_type(mrb, self, MRB_TT_FIXNUM, "Fixnum", "to_int");
        if (mrb_fixnum_p(s))
          mrb_msgpack_pack_value(mrb, s, pk);
        else {
          s = mrb_check_convert_type(mrb, self, MRB_TT_STRING, "String", "to_str");
          if (mrb_string_p(s))
            mrb_msgpack_pack_value(mrb, s, pk);
          else {
            s = mrb_convert_type(mrb, self, MRB_TT_STRING, "String", "to_s");
            mrb_msgpack_pack_value(mrb, s, pk);
          }
        }
      }
    }
  }
}

static mrb_value
mrb_msgpack_pack(mrb_state *mrb, mrb_value self)
{
  msgpack_packer pk;
  mrb_msgpack_buffer sbuf;
  sbuf.mrb = mrb;
  sbuf.buffer = mrb_str_new(mrb, NULL, 0);
  msgpack_packer_init(&pk, &sbuf, mrb_msgpack_buffer_write);

  mrb_msgpack_pack_value(mrb, self, &pk);

  return sbuf.buffer;
}

static inline mrb_value
mrb_unpack_msgpack_obj(mrb_state *mrb, msgpack_object obj)
{
  switch(obj.type) {
    case MSGPACK_OBJECT_NIL:
      return mrb_nil_value();
    break;
    case MSGPACK_OBJECT_BOOLEAN: {
      if (obj.via.boolean)
        return mrb_true_value();

      return mrb_false_value();
    }
    break;
    case MSGPACK_OBJECT_POSITIVE_INTEGER: {
      if (obj.via.u64 > MRB_INT_MAX)
        mrb_raise(mrb, E_MSGPACK_ERROR, "Cannot unpack Integer");

      return mrb_fixnum_value(obj.via.u64);
    }
    break;
    case MSGPACK_OBJECT_NEGATIVE_INTEGER: {
      if (obj.via.i64 < MRB_INT_MIN)
        mrb_raise(mrb, E_MSGPACK_ERROR, "Cannot unpack Integer");

      return mrb_fixnum_value(obj.via.i64);
    }
    break;
    case MSGPACK_OBJECT_FLOAT:
      return mrb_float_value(mrb, obj.via.f64);
    break;
    case MSGPACK_OBJECT_STR:
      return mrb_str_new(mrb, obj.via.str.ptr, obj.via.str.size);
    break;
    case MSGPACK_OBJECT_ARRAY: {
      if (obj.via.array.size != 0) {
        mrb_value unpacked_array = mrb_ary_new_capa(mrb, (mrb_int) obj.via.array.size);
        msgpack_object* p = obj.via.array.ptr;
        msgpack_object* const pend = obj.via.array.ptr + obj.via.array.size;
        for(; p < pend; p++)
          mrb_ary_push(mrb, unpacked_array, mrb_unpack_msgpack_obj(mrb, *p));

        return unpacked_array;
      }
      return mrb_ary_new(mrb);
    }
    break;
    case MSGPACK_OBJECT_MAP: {
      if (obj.via.map.size != 0) {
        mrb_value unpacked_hash = mrb_hash_new_capa(mrb, (mrb_int) obj.via.map.size);
        msgpack_object_kv* p = obj.via.map.ptr;
        msgpack_object_kv* const pend = obj.via.map.ptr + obj.via.map.size;
        for(; p < pend; p++)
          mrb_hash_set(mrb, unpacked_hash, mrb_unpack_msgpack_obj(mrb, p->key), mrb_unpack_msgpack_obj(mrb, p->val));

        return unpacked_hash;
      }
      return mrb_hash_new(mrb);
    }
    break;
    case MSGPACK_OBJECT_BIN:
      return mrb_str_new(mrb, obj.via.bin.ptr, obj.via.bin.size);
    break;
    default:
      mrb_raisef(mrb, E_MSGPACK_ERROR, "Cannot unpack type %S", mrb_fixnum_value(obj.type));
  }
}

static mrb_value
mrb_msgpack_unpack(mrb_state *mrb, mrb_value self)
{
  mrb_value data, block;

  mrb_get_args(mrb, "o&", &data, &block);

  if (mrb_nil_p(block))
    mrb_raise(mrb, E_ARGUMENT_ERROR, "no block given");

  data = mrb_str_to_str(mrb, data);

  msgpack_unpacked result;
  size_t off = 0;
  msgpack_unpack_return ret;

  msgpack_unpacked_init(&result);
  ret = msgpack_unpack_next(&result, RSTRING_PTR(data), RSTRING_LEN(data), &off);

  struct mrb_jmpbuf *prev_jmp = mrb->jmp;
  struct mrb_jmpbuf c_jmp;

  MRB_TRY(&c_jmp) {
    mrb->jmp = &c_jmp;
    while (ret == MSGPACK_UNPACK_SUCCESS) {
      mrb_yield(mrb, block, mrb_unpack_msgpack_obj(mrb, result.data));
      ret = msgpack_unpack_next(&result, RSTRING_PTR(data), RSTRING_LEN(data), &off);
    }
    mrb->jmp = prev_jmp;
  } MRB_CATCH(&c_jmp) {
    mrb->jmp = prev_jmp;
    msgpack_unpacked_destroy(&result);
    MRB_THROW(mrb->jmp);
  } MRB_END_EXC(&c_jmp);

  msgpack_unpacked_destroy(&result);

  if (ret == MSGPACK_UNPACK_NOMEM_ERROR) {
    mrb->out_of_memory = TRUE;
    mrb_exc_raise(mrb, mrb_obj_value(mrb->nomem_err));
  }
  if (ret == MSGPACK_UNPACK_PARSE_ERROR)
    mrb_raise(mrb, E_MSGPACK_ERROR, "Invalid data recieved");

  return self;
}

void
mrb_mruby_msgpack_gem_init(mrb_state* mrb) {
  struct RClass *msgpack_mod;

  mrb_define_method(mrb, mrb->object_class, "to_msgpack", mrb_msgpack_pack, MRB_ARGS_NONE());

  msgpack_mod = mrb_define_module(mrb, "MessagePack");
  mrb_define_class_under(mrb, msgpack_mod, "Error", E_RUNTIME_ERROR);
  mrb_define_module_function(mrb, msgpack_mod, "unpack", mrb_msgpack_unpack, (MRB_ARGS_REQ(1)|MRB_ARGS_BLOCK()));
}

void
mrb_mruby_msgpack_gem_final(mrb_state* mrb) {
}
