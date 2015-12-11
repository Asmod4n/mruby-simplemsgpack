#include <msgpack.h>
#include <mruby.h>
#include <mruby/array.h>
#include <mruby/class.h>
#include <mruby/data.h>
#include <mruby/error.h>
#include <mruby/hash.h>
#include <mruby/string.h>
#include <mruby/throw.h>
#include <mruby/variable.h>
#include "mruby/msgpack.h"
#include "is_utf8.h"

typedef struct {
    mrb_state* mrb;
    mrb_value buffer;
} mrb_msgpack_data;

#if (__GNUC__ >= 3) || (__INTEL_COMPILER >= 800) || defined(__clang__)
#define likely(x) __builtin_expect(!!(x), 1)
#define unlikely(x) __builtin_expect(!!(x), 0)
#else
#define likely(x) (x)
#define unlikely(x) (x)
#endif

static inline int
mrb_msgpack_data_write(void* data, const char* buf, size_t len)
{
    mrb_msgpack_data* mrb_data = (mrb_msgpack_data*)data;
    mrb_str_cat(mrb_data->mrb, mrb_data->buffer, buf, len);
    if (unlikely(mrb_data->mrb->exc))
        return -1;

    return 0;
}

static inline void
mrb_msgpack_pack_fixnum_value(mrb_value self, msgpack_packer* pk)
{
#if defined(MRB_INT16)
    msgpack_pack_int16(pk, mrb_fixnum(self));
#elif defined(MRB_INT64)
    msgpack_pack_int64(pk, mrb_fixnum(self));
#else
    msgpack_pack_int32(pk, mrb_fixnum(self));
#endif
}

static inline void
mrb_msgpack_pack_float_value(mrb_value self, msgpack_packer* pk)
{
#if defined(MRB_USE_FLOAT)
    msgpack_pack_float(pk, mrb_float(self));
#else
    msgpack_pack_double(pk, mrb_float(self));
#endif
}

static inline void
mrb_msgpack_pack_symbol_value(mrb_state* mrb, mrb_value self, msgpack_packer* pk)
{
    mrb_int len;
    const char* name = mrb_sym2name_len(mrb, mrb_symbol(self), &len);
    msgpack_pack_ext(pk, len, 1);
    msgpack_pack_ext_body(pk, name, len);
}

static inline void
mrb_msgpack_pack_string_value(mrb_value self, msgpack_packer* pk)
{
    if (is_utf8((unsigned char*)RSTRING_PTR(self), RSTRING_LEN(self)) == 0) {
        msgpack_pack_str(pk, RSTRING_LEN(self));
        msgpack_pack_str_body(pk, RSTRING_PTR(self), RSTRING_LEN(self));
    } else {
        msgpack_pack_bin(pk, RSTRING_LEN(self));
        msgpack_pack_bin_body(pk, RSTRING_PTR(self), RSTRING_LEN(self));
    }
}

static inline void
mrb_msgpack_pack_class_value(mrb_state* mrb, mrb_value self, msgpack_packer* pk)
{
    struct RClass *class = mrb_class_ptr(self);
    const char *classname = mrb_class_name(mrb, class);
    size_t len = strlen(classname);
    if (is_utf8((unsigned char*)classname, len) == 0) {
        msgpack_pack_str(pk, len);
        msgpack_pack_str_body(pk, classname, len);
    } else {
        msgpack_pack_bin(pk, len);
        msgpack_pack_bin_body(pk, classname, len);
    }
}

static inline void
mrb_msgpack_pack_array_value(mrb_state* mrb, mrb_value self, msgpack_packer* pk);

static inline void
mrb_msgpack_pack_hash_value(mrb_state* mrb, mrb_value self, msgpack_packer* pk);

static inline void
mrb_msgpack_pack_value(mrb_state* mrb, mrb_value self, msgpack_packer* pk)
{
    if (mrb_nil_p(self))
        msgpack_pack_nil(pk);
    else if (mrb_type(self) == MRB_TT_FALSE)
        msgpack_pack_false(pk);
    else if (mrb_type(self) == MRB_TT_TRUE)
        msgpack_pack_true(pk);
    else if (mrb_fixnum_p(self))
        mrb_msgpack_pack_fixnum_value(self, pk);
    else if (mrb_symbol_p(self))
       mrb_msgpack_pack_symbol_value(mrb, self, pk);
    else if (mrb_float_p(self))
        mrb_msgpack_pack_float_value(self, pk);
    else if (mrb_type(self) == MRB_TT_CLASS || mrb_type(self) == MRB_TT_MODULE || mrb_type(self) == MRB_TT_SCLASS)
        mrb_msgpack_pack_class_value(mrb, self, pk);
    else if (mrb_array_p(self))
        mrb_msgpack_pack_array_value(mrb, self, pk);
    else if (mrb_hash_p(self))
        mrb_msgpack_pack_hash_value(mrb, self, pk);
    else if (mrb_string_p(self))
        mrb_msgpack_pack_string_value(self, pk);
    else {
        mrb_value s;
        s = mrb_check_convert_type(mrb, self, MRB_TT_HASH, "Hash", "to_hash");
        if (mrb_hash_p(s))
            mrb_msgpack_pack_hash_value(mrb, s, pk);
        else {
            s = mrb_check_convert_type(mrb, self, MRB_TT_ARRAY, "Array", "to_ary");
            if (mrb_array_p(s))
                mrb_msgpack_pack_array_value(mrb, s, pk);
            else {
                s = mrb_check_convert_type(mrb, self, MRB_TT_FIXNUM, "Fixnum", "to_int");
                if (mrb_fixnum_p(s))
                    mrb_msgpack_pack_fixnum_value(s, pk);
                else {
                    s = mrb_check_convert_type(mrb, self, MRB_TT_STRING, "String", "to_str");
                    if (mrb_string_p(s))
                        mrb_msgpack_pack_string_value(s, pk);
                    else {
                        s = mrb_convert_type(mrb, self, MRB_TT_SYMBOL, "String", "to_sym");
                        if (mrb_symbol_p(s)) {
                            mrb_msgpack_pack_symbol_value(mrb, self, pk);
                        } else {
                            s = mrb_convert_type(mrb, self, MRB_TT_STRING, "String", "to_s");
                            mrb_msgpack_pack_string_value(s, pk);
                        }
                    }
                }
            }
        }
    }
}

static inline void
mrb_msgpack_pack_array_value(mrb_state* mrb, mrb_value self, msgpack_packer* pk)
{
    msgpack_pack_array(pk, RARRAY_LEN(self));
    for (mrb_int i = 0; i != RARRAY_LEN(self); i++)
        mrb_msgpack_pack_value(mrb, mrb_ary_ref(mrb, self, i), pk);
}

static inline void
mrb_msgpack_pack_hash_value(mrb_state* mrb, mrb_value self, msgpack_packer* pk)
{
    int ai = mrb_gc_arena_save(mrb);
    mrb_value keys = mrb_hash_keys(mrb, self);
    msgpack_pack_map(pk, RARRAY_LEN(keys));
    for (mrb_int i = 0; i != RARRAY_LEN(keys); i++) {
        mrb_value key = mrb_ary_ref(mrb, keys, i);
        mrb_msgpack_pack_value(mrb, key, pk);
        mrb_msgpack_pack_value(mrb, mrb_hash_get(mrb, self, key), pk);
    }
    mrb_gc_arena_restore(mrb, ai);
}

static mrb_value
mrb_msgpack_pack_object(mrb_state* mrb, mrb_value self)
{
    msgpack_packer pk;
    mrb_msgpack_data data;
    data.mrb = mrb;
    data.buffer = mrb_str_new(mrb, NULL, 0);
    msgpack_packer_init(&pk, &data, mrb_msgpack_data_write);

    mrb_msgpack_pack_value(mrb, self, &pk);

    return data.buffer;
}

static mrb_value
mrb_msgpack_pack_string(mrb_state* mrb, mrb_value self)
{
    msgpack_packer pk;
    mrb_msgpack_data data;
    data.mrb = mrb;
    data.buffer = mrb_str_new(mrb, NULL, 0);
    msgpack_packer_init(&pk, &data, mrb_msgpack_data_write);

    mrb_msgpack_pack_string_value(self, &pk);

    return data.buffer;
}

static mrb_value
mrb_msgpack_pack_array(mrb_state* mrb, mrb_value self)
{
    msgpack_packer pk;
    mrb_msgpack_data data;
    data.mrb = mrb;
    data.buffer = mrb_str_new(mrb, NULL, 0);
    msgpack_packer_init(&pk, &data, mrb_msgpack_data_write);

    mrb_msgpack_pack_array_value(mrb, self, &pk);

    return data.buffer;
}

static mrb_value
mrb_msgpack_pack_hash(mrb_state* mrb, mrb_value self)
{
    msgpack_packer pk;
    mrb_msgpack_data data;
    data.mrb = mrb;
    data.buffer = mrb_str_new(mrb, NULL, 0);
    msgpack_packer_init(&pk, &data, mrb_msgpack_data_write);

    mrb_msgpack_pack_hash_value(mrb, self, &pk);

    return data.buffer;
}

static mrb_value
mrb_msgpack_pack_float(mrb_state* mrb, mrb_value self)
{
    msgpack_packer pk;
    mrb_msgpack_data data;
    data.mrb = mrb;
    data.buffer = mrb_str_new(mrb, NULL, 0);
    msgpack_packer_init(&pk, &data, mrb_msgpack_data_write);

    mrb_msgpack_pack_float_value(self, &pk);

    return data.buffer;
}

static mrb_value
mrb_msgpack_pack_fixnum(mrb_state* mrb, mrb_value self)
{
    msgpack_packer pk;
    mrb_msgpack_data data;
    data.mrb = mrb;
    data.buffer = mrb_str_new(mrb, NULL, 0);
    msgpack_packer_init(&pk, &data, mrb_msgpack_data_write);

    mrb_msgpack_pack_fixnum_value(self, &pk);

    return data.buffer;
}

static mrb_value
mrb_msgpack_pack_true(mrb_state* mrb, mrb_value self)
{
    msgpack_packer pk;
    mrb_msgpack_data data;
    data.mrb = mrb;
    data.buffer = mrb_str_new(mrb, NULL, 1);
    msgpack_packer_init(&pk, &data, mrb_msgpack_data_write);

    msgpack_pack_true(&pk);

    return data.buffer;
}

static mrb_value
mrb_msgpack_pack_false(mrb_state* mrb, mrb_value self)
{
    msgpack_packer pk;
    mrb_msgpack_data data;
    data.mrb = mrb;
    data.buffer = mrb_str_new(mrb, NULL, 1);
    msgpack_packer_init(&pk, &data, mrb_msgpack_data_write);

    msgpack_pack_false(&pk);

    return data.buffer;
}

static mrb_value
mrb_msgpack_pack_nil(mrb_state* mrb, mrb_value self)
{
    msgpack_packer pk;
    mrb_msgpack_data data;
    data.mrb = mrb;
    data.buffer = mrb_str_new(mrb, NULL, 1);
    msgpack_packer_init(&pk, &data, mrb_msgpack_data_write);

    msgpack_pack_nil(&pk);

    return data.buffer;
}

static mrb_value
mrb_msgpack_pack_symbol(mrb_state* mrb, mrb_value self)
{
    msgpack_packer pk;
    mrb_msgpack_data data;
    data.mrb = mrb;
    data.buffer = mrb_str_new(mrb, NULL, 0);
    msgpack_packer_init(&pk, &data, mrb_msgpack_data_write);

    mrb_msgpack_pack_symbol_value(mrb, self, &pk);

    return data.buffer;
}

static mrb_value
mrb_msgpack_pack_class(mrb_state* mrb, mrb_value self)
{
    msgpack_packer pk;
    mrb_msgpack_data data;
    data.mrb = mrb;
    data.buffer = mrb_str_new(mrb, NULL, 0);
    msgpack_packer_init(&pk, &data, mrb_msgpack_data_write);

    mrb_msgpack_pack_class_value(mrb, self, &pk);

    return data.buffer;
}


static inline mrb_value
mrb_unpack_msgpack_obj_array(mrb_state* mrb, msgpack_object obj);

static inline mrb_value
mrb_unpack_msgpack_obj_map(mrb_state* mrb, msgpack_object obj);

static inline mrb_value
mrb_unpack_msgpack_obj(mrb_state* mrb, msgpack_object obj)
{
    switch (obj.type) {
    case MSGPACK_OBJECT_NIL:
        return mrb_nil_value();
        break;
    case MSGPACK_OBJECT_BOOLEAN: {
        if (obj.via.boolean)
            return mrb_true_value();
        else
            return mrb_false_value();
    } break;
    case MSGPACK_OBJECT_POSITIVE_INTEGER: {
        if (MRB_INT_MAX < obj.via.u64)
            mrb_raise(mrb, E_MSGPACK_ERROR, "Cannot unpack Integer");

        return mrb_fixnum_value(obj.via.u64);
    } break;
    case MSGPACK_OBJECT_NEGATIVE_INTEGER: {
        if (obj.via.i64 < MRB_INT_MIN)
            mrb_raise(mrb, E_MSGPACK_ERROR, "Cannot unpack Integer");

        return mrb_fixnum_value(obj.via.i64);
    } break;
    case MSGPACK_OBJECT_FLOAT:
        return mrb_float_value(mrb, obj.via.f64);
        break;
    case MSGPACK_OBJECT_STR:
        return mrb_str_new(mrb, obj.via.str.ptr, obj.via.str.size);
        break;
    case MSGPACK_OBJECT_ARRAY:
        return mrb_unpack_msgpack_obj_array(mrb, obj);
        break;
    case MSGPACK_OBJECT_MAP:
        return mrb_unpack_msgpack_obj_map(mrb, obj);
        break;
    case MSGPACK_OBJECT_BIN:
        return mrb_str_new(mrb, obj.via.bin.ptr, obj.via.bin.size);
        break;
    case MSGPACK_OBJECT_EXT: {
        if (obj.via.ext.type == 1) {
            return mrb_check_intern(mrb, obj.via.ext.ptr, obj.via.ext.size);
        } else {
            mrb_raisef(mrb, E_MSGPACK_ERROR, "Cannot unpack ext type %S", mrb_fixnum_value(obj.via.ext.type));
        }
    } break;
    default:
        mrb_raisef(mrb, E_MSGPACK_ERROR, "Cannot unpack type %S", mrb_fixnum_value(obj.type));
    }
}

static inline mrb_value
mrb_unpack_msgpack_obj_array(mrb_state* mrb, msgpack_object obj)
{
    if (obj.via.array.size != 0) {
        mrb_value unpacked_array = mrb_ary_new_capa(mrb, obj.via.array.size);
        int ai = mrb_gc_arena_save(mrb);
        msgpack_object* p = obj.via.array.ptr;
        msgpack_object* const pend = obj.via.array.ptr + obj.via.array.size;
        for (; p != pend; p++) {
            mrb_value unpacked_obj = mrb_unpack_msgpack_obj(mrb, *p);
            mrb_ary_push(mrb, unpacked_array, unpacked_obj);
            mrb_gc_arena_restore(mrb, ai);
        }

        return unpacked_array;
    }
    else
        return mrb_ary_new(mrb);
}

static inline mrb_value
mrb_unpack_msgpack_obj_map(mrb_state* mrb, msgpack_object obj)
{
    if (obj.via.map.size != 0) {
        mrb_value unpacked_hash = mrb_hash_new_capa(mrb, obj.via.map.size);
        int ai = mrb_gc_arena_save(mrb);
        msgpack_object_kv* p = obj.via.map.ptr;
        msgpack_object_kv* const pend = obj.via.map.ptr + obj.via.map.size;
        for (; p != pend; p++) {
            mrb_value unpacked_key = mrb_unpack_msgpack_obj(mrb, p->key);
            mrb_value unpacked_value = mrb_unpack_msgpack_obj(mrb, p->val);
            mrb_hash_set(mrb, unpacked_hash, unpacked_key, unpacked_value);
            mrb_gc_arena_restore(mrb, ai);
        }

        return unpacked_hash;
    }
    else
        return mrb_hash_new(mrb);
}

static mrb_value
mrb_msgpack_unpack(mrb_state* mrb, mrb_value self)
{
    mrb_value data, block;

    mrb_get_args(mrb, "o&", &data, &block);

    data = mrb_str_to_str(mrb, data);

    msgpack_unpacked result;
    size_t off = 0;
    msgpack_unpack_return ret;

    msgpack_unpacked_init(&result);
    ret = msgpack_unpack_next(&result, RSTRING_PTR(data), RSTRING_LEN(data), &off);

    struct mrb_jmpbuf* prev_jmp = mrb->jmp;
    struct mrb_jmpbuf c_jmp;
    mrb_value unpack_return = self;

    MRB_TRY(&c_jmp)
    {
        mrb->jmp = &c_jmp;
        if (mrb_nil_p(block)) {
            if (ret == MSGPACK_UNPACK_SUCCESS) {
                unpack_return = mrb_unpack_msgpack_obj(mrb, result.data);
            }
        }
        else {
            int ai = mrb_gc_arena_save(mrb);
            while (ret == MSGPACK_UNPACK_SUCCESS) {
                mrb_value unpacked_obj = mrb_unpack_msgpack_obj(mrb, result.data);
                mrb_yield(mrb, block, unpacked_obj);
                mrb_gc_arena_restore(mrb, ai);
                ret = msgpack_unpack_next(&result, RSTRING_PTR(data), RSTRING_LEN(data), &off);
            }
        }
        mrb->jmp = prev_jmp;
    }
    MRB_CATCH(&c_jmp)
    {
        mrb->jmp = prev_jmp;
        msgpack_unpacked_destroy(&result);
        MRB_THROW(mrb->jmp);
    }
    MRB_END_EXC(&c_jmp);

    msgpack_unpacked_destroy(&result);

    if (unlikely(ret == MSGPACK_UNPACK_NOMEM_ERROR)) {
        mrb_sys_fail(mrb, "msgpack_unpack_next");
    }
    if (ret == MSGPACK_UNPACK_PARSE_ERROR)
        mrb_raise(mrb, E_MSGPACK_ERROR, "Invalid data recieved");

    return unpack_return;
}

void mrb_mruby_simplemsgpack_gem_init(mrb_state* mrb)
{
    struct RClass* msgpack_mod;

    mrb_define_method(mrb, mrb->object_class, "to_msgpack", mrb_msgpack_pack_object, MRB_ARGS_NONE());
    mrb_define_method(mrb, mrb->string_class, "to_msgpack", mrb_msgpack_pack_string, MRB_ARGS_NONE());
    mrb_define_method(mrb, mrb->array_class, "to_msgpack", mrb_msgpack_pack_array, MRB_ARGS_NONE());
    mrb_define_method(mrb, mrb->hash_class, "to_msgpack", mrb_msgpack_pack_hash, MRB_ARGS_NONE());
    mrb_define_method(mrb, mrb->float_class, "to_msgpack", mrb_msgpack_pack_float, MRB_ARGS_NONE());
    mrb_define_method(mrb, mrb->fixnum_class, "to_msgpack", mrb_msgpack_pack_fixnum, MRB_ARGS_NONE());
    mrb_define_method(mrb, mrb->true_class, "to_msgpack", mrb_msgpack_pack_true, MRB_ARGS_NONE());
    mrb_define_method(mrb, mrb->false_class, "to_msgpack", mrb_msgpack_pack_false, MRB_ARGS_NONE());
    mrb_define_method(mrb, mrb->nil_class, "to_msgpack", mrb_msgpack_pack_nil, MRB_ARGS_NONE());
    mrb_define_method(mrb, mrb->symbol_class, "to_msgpack", mrb_msgpack_pack_symbol, MRB_ARGS_NONE());
    mrb_define_method(mrb, mrb->class_class, "to_msgpack", mrb_msgpack_pack_class, MRB_ARGS_NONE());
    mrb_define_method(mrb, mrb->module_class, "to_msgpack", mrb_msgpack_pack_class, MRB_ARGS_NONE());

    msgpack_mod = mrb_define_module(mrb, "MessagePack");
    mrb_define_class_under(mrb, msgpack_mod, "Error", E_RUNTIME_ERROR);
    mrb_define_module_function(mrb, msgpack_mod, "unpack", mrb_msgpack_unpack, (MRB_ARGS_REQ(1) | MRB_ARGS_BLOCK()));
}

void mrb_mruby_simplemsgpack_gem_final(mrb_state* mrb)
{
}
