#include <msgpack.h>
#include <mruby.h>
#include <mruby/array.h>
#include <mruby/class.h>
#include <mruby/data.h>
#include <mruby/error.h>
#include <mruby/hash.h>
#include <mruby/string.h>
#include <mruby/string_is_utf8.h>
#include <mruby/throw.h>
#include <mruby/variable.h>
#include "mruby/msgpack.h"

typedef struct {
    mrb_state* mrb;
    mrb_value buffer;
} mrb_msgpack_data;

static mrb_value pack_ext_registry;
static mrb_value unpack_ext_registry;

#if (__GNUC__ >= 3) || (__INTEL_COMPILER >= 800) || defined(__clang__)
#define likely(x) __builtin_expect(!!(x), 1)
#define unlikely(x) __builtin_expect(!!(x), 0)
#else
#define likely(x) (x)
#define unlikely(x) (x)
#endif

MRB_INLINE int
mrb_msgpack_data_write(void* data, const char* buf, size_t len)
{
    mrb_msgpack_data* mrb_data = (mrb_msgpack_data*)data;
    mrb_str_cat(mrb_data->mrb, mrb_data->buffer, buf, len);
    if (unlikely(mrb_data->mrb->exc)) {
        return -1;
    } else {
        return 0;
    }
}

MRB_INLINE void
mrb_msgpack_pack_fixnum_value(mrb_value self, msgpack_packer* pk)
{
#ifdef MRB_INT16
    msgpack_pack_int16(pk, mrb_fixnum(self));
#elif defined(MRB_INT64)
    msgpack_pack_int64(pk, mrb_fixnum(self));
#else
    msgpack_pack_int32(pk, mrb_fixnum(self));
#endif
}

MRB_INLINE void
mrb_msgpack_pack_float_value(mrb_value self, msgpack_packer* pk)
{
#ifdef MRB_USE_FLOAT
    msgpack_pack_float(pk, mrb_float(self));
#else
    msgpack_pack_double(pk, mrb_float(self));
#endif
}

MRB_INLINE void
mrb_msgpack_pack_string_value(mrb_value self, msgpack_packer* pk)
{
    if (mrb_str_is_utf8(self)) {
        msgpack_pack_str(pk, RSTRING_LEN(self));
        msgpack_pack_str_body(pk, RSTRING_PTR(self), RSTRING_LEN(self));
    } else {
        msgpack_pack_bin(pk, RSTRING_LEN(self));
        msgpack_pack_bin_body(pk, RSTRING_PTR(self), RSTRING_LEN(self));
    }
}

MRB_INLINE mrb_value
mrb_msgpack_get_ext_config(mrb_state* mrb, mrb_value obj)
{
    mrb_value obj_class = mrb_obj_value(mrb_obj_class(mrb, obj));
    return mrb_hash_get(mrb, pack_ext_registry, obj_class);
}

MRB_INLINE int
mrb_msgpack_pack_ext_value(mrb_state* mrb, mrb_value self, msgpack_packer* pk)
{
    mrb_value ext_config = mrb_msgpack_get_ext_config(mrb, self);

    if (mrb_nil_p(ext_config)) {
        return 0;
    }

    mrb_value type = mrb_hash_get(mrb, ext_config, mrb_symbol_value(mrb_intern_lit(mrb, "type")));
    mrb_value packer = mrb_hash_get(mrb, ext_config, mrb_symbol_value(mrb_intern_lit(mrb, "packer")));
    mrb_value packed = mrb_yield(mrb, packer, self);

    const char* body;
    size_t len;

    if (!mrb_string_p(packed)) {
        mrb_raise(mrb, E_MSGPACK_ERROR, "no string returned by ext type packer");
    }

    body = mrb_string_value_ptr(mrb, packed);
    len = strlen(body);

    msgpack_pack_ext(pk, len, mrb_fixnum(type));
    msgpack_pack_ext_body(pk, body, len);

    return 1;
}

MRB_INLINE void
mrb_msgpack_pack_array_value(mrb_state* mrb, mrb_value self, msgpack_packer* pk);

MRB_INLINE void
mrb_msgpack_pack_hash_value(mrb_state* mrb, mrb_value self, msgpack_packer* pk);

MRB_INLINE void
mrb_msgpack_pack_value(mrb_state* mrb, mrb_value self, msgpack_packer* pk)
{
    switch (mrb_type(self)) {
        case MRB_TT_FALSE: {
            if (!mrb_fixnum(self)) {
                msgpack_pack_nil(pk);
            } else {
                msgpack_pack_false(pk);
            }
        } break;
        case MRB_TT_TRUE:
            msgpack_pack_true(pk);
            break;
        case MRB_TT_FIXNUM:
            mrb_msgpack_pack_fixnum_value(self, pk);
            break;
        case MRB_TT_FLOAT:
            mrb_msgpack_pack_float_value(self, pk);
            break;
        case MRB_TT_ARRAY:
            mrb_msgpack_pack_array_value(mrb, self, pk);
            break;
        case MRB_TT_HASH:
            mrb_msgpack_pack_hash_value(mrb, self, pk);
            break;
        case MRB_TT_STRING:
            mrb_msgpack_pack_string_value(self, pk);
            break;
        default: {
            if (!mrb_msgpack_pack_ext_value(mrb, self, pk)) {
                mrb_value try_convert;
                try_convert = mrb_check_convert_type(mrb, self, MRB_TT_HASH, "Hash", "to_hash");
                if (mrb_hash_p(try_convert)) {
                    mrb_msgpack_pack_hash_value(mrb, try_convert, pk);
                } else {
                    try_convert = mrb_check_convert_type(mrb, self, MRB_TT_ARRAY, "Array", "to_ary");
                    if (mrb_array_p(try_convert)) {
                        mrb_msgpack_pack_array_value(mrb, try_convert, pk);
                    } else {
                        try_convert = mrb_check_convert_type(mrb, self, MRB_TT_FIXNUM, "Fixnum", "to_int");
                        if (mrb_fixnum_p(try_convert)) {
                            mrb_msgpack_pack_fixnum_value(try_convert, pk);
                        } else {
                            try_convert = mrb_check_convert_type(mrb, self, MRB_TT_STRING, "String", "to_str");
                            if (mrb_string_p(try_convert)) {
                                mrb_msgpack_pack_string_value(try_convert, pk);
                            } else {
                                try_convert = mrb_convert_type(mrb, self, MRB_TT_STRING, "String", "to_s");
                                mrb_msgpack_pack_string_value(try_convert, pk);
                            }
                        }
                    }
                }
            }
        }
    }
}

MRB_INLINE void
mrb_msgpack_pack_array_value(mrb_state* mrb, mrb_value self, msgpack_packer* pk)
{
    msgpack_pack_array(pk, RARRAY_LEN(self));
    for (mrb_int ary_pos = 0; ary_pos != RARRAY_LEN(self); ary_pos++) {
        mrb_msgpack_pack_value(mrb, mrb_ary_ref(mrb, self, ary_pos), pk);
    }
}

MRB_INLINE void
mrb_msgpack_pack_hash_value(mrb_state* mrb, mrb_value self, msgpack_packer* pk)
{
    int ai = mrb_gc_arena_save(mrb);
    mrb_value keys = mrb_hash_keys(mrb, self);
    msgpack_pack_map(pk, RARRAY_LEN(keys));
    for (mrb_int hash_pos = 0; hash_pos != RARRAY_LEN(keys); hash_pos++) {
        mrb_value key = mrb_ary_ref(mrb, keys, hash_pos);
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
    data.buffer = mrb_str_new(mrb, NULL, 0);
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
    data.buffer = mrb_str_new(mrb, NULL, 0);
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
    data.buffer = mrb_str_new(mrb, NULL, 0);
    msgpack_packer_init(&pk, &data, mrb_msgpack_data_write);

    msgpack_pack_nil(&pk);

    return data.buffer;
}

MRB_INLINE mrb_value
mrb_unpack_msgpack_obj_array(mrb_state* mrb, msgpack_object obj);

MRB_INLINE mrb_value
mrb_unpack_msgpack_obj_map(mrb_state* mrb, msgpack_object obj);

MRB_INLINE mrb_value
mrb_unpack_msgpack_obj(mrb_state* mrb, msgpack_object obj)
{
    switch (obj.type) {
        case MSGPACK_OBJECT_NIL:
            return mrb_nil_value();
            break;
        case MSGPACK_OBJECT_BOOLEAN: {
            if (obj.via.boolean) {
                return mrb_true_value();
            } else {
                return mrb_false_value();
            }
        } break;
        case MSGPACK_OBJECT_POSITIVE_INTEGER: {
            if (MRB_INT_MAX < obj.via.u64) {
                mrb_raise(mrb, E_MSGPACK_ERROR, "Cannot unpack Integer");
            }

            return mrb_fixnum_value(obj.via.u64);
        } break;
        case MSGPACK_OBJECT_NEGATIVE_INTEGER: {
            if (obj.via.i64 < MRB_INT_MIN) {
                mrb_raise(mrb, E_MSGPACK_ERROR, "Cannot unpack Integer");
            }

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
            mrb_value unpacker = mrb_hash_get(mrb, unpack_ext_registry, mrb_fixnum_value(obj.via.ext.type));
            mrb_value data = mrb_str_new_static(mrb, obj.via.ext.ptr, obj.via.ext.size);
            return mrb_yield(mrb, unpacker, data);
        } break;
        default:
            mrb_raisef(mrb, E_MSGPACK_ERROR, "Cannot unpack type %S", mrb_fixnum_value(obj.type));
    }
}

MRB_INLINE mrb_value
mrb_unpack_msgpack_obj_array(mrb_state* mrb, msgpack_object obj)
{
    if (obj.via.array.size != 0) {
        mrb_value unpacked_array = mrb_ary_new_capa(mrb, obj.via.array.size);
        int ai = mrb_gc_arena_save(mrb);
        for (size_t array_pos = 0; array_pos < obj.via.array.size; array_pos++) {
            mrb_value unpacked_obj = mrb_unpack_msgpack_obj(mrb, obj.via.array.ptr[array_pos]);
            mrb_ary_push(mrb, unpacked_array, unpacked_obj);
            mrb_gc_arena_restore(mrb, ai);
        }

        return unpacked_array;
    } else {
        return mrb_ary_new(mrb);
    }
}

MRB_INLINE mrb_value
mrb_unpack_msgpack_obj_map(mrb_state* mrb, msgpack_object obj)
{
    if (obj.via.map.size != 0) {
        mrb_value unpacked_hash = mrb_hash_new_capa(mrb, obj.via.map.size);
        int ai = mrb_gc_arena_save(mrb);
        for (size_t map_pos = 0; map_pos < obj.via.map.size; map_pos++) {
            mrb_value unpacked_key = mrb_unpack_msgpack_obj(mrb, obj.via.map.ptr[map_pos].key);
            mrb_value unpacked_value = mrb_unpack_msgpack_obj(mrb, obj.via.map.ptr[map_pos].val);
            mrb_hash_set(mrb, unpacked_hash, unpacked_key, unpacked_value);
            mrb_gc_arena_restore(mrb, ai);
        }

        return unpacked_hash;
    } else {
        return mrb_hash_new(mrb);
    }
}

static mrb_value
mrb_msgpack_unpack(mrb_state* mrb, mrb_value self)
{
    mrb_value data, block = mrb_nil_value();

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
        } else {
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
    if (ret == MSGPACK_UNPACK_PARSE_ERROR) {
        mrb_raise(mrb, E_MSGPACK_ERROR, "Invalid data recieved");
    }

    return unpack_return;
}

static mrb_value
mrb_msgpack_register_pack_type(mrb_state* mrb, mrb_value self)
{
    mrb_int type;
    mrb_value mrb_class;
    mrb_value block = mrb_nil_value();
    mrb_value ext_config;

    mrb_get_args(mrb, "iC&", &type, &mrb_class, &block);

    if (type < 0 || type > 127) {
        mrb_raise(mrb, E_MSGPACK_ERROR, "ext type out of range");
    }

    if (mrb_nil_p(block)) {
        mrb_raise(mrb, E_MSGPACK_ERROR, "no block given");
    }

    ext_config = mrb_hash_new(mrb);
    mrb_hash_set(mrb, ext_config, mrb_symbol_value(mrb_intern_lit(mrb, "type")), mrb_fixnum_value(type));
    mrb_hash_set(mrb, ext_config, mrb_symbol_value(mrb_intern_lit(mrb, "packer")), block);
    mrb_hash_set(mrb, pack_ext_registry, mrb_class, ext_config);

    return mrb_nil_value();
}

static mrb_value
mrb_msgpack_register_unpack_type(mrb_state* mrb, mrb_value self)
{
    mrb_int type;
    mrb_value block = mrb_nil_value();

    mrb_get_args(mrb, "i&", &type, &block);

    if (type < 0 || type > 127) {
        mrb_raise(mrb, E_MSGPACK_ERROR, "ext type out of range");
    }

    if (mrb_nil_p(block)) {
        mrb_raise(mrb, E_MSGPACK_ERROR, "no block");
    }

    mrb_hash_set(mrb, unpack_ext_registry, mrb_fixnum_value(type), block);

    return mrb_nil_value();
}

void
mrb_mruby_simplemsgpack_ext_gem_init(mrb_state* mrb)
{
    struct RClass* msgpack_mod;

    pack_ext_registry = mrb_hash_new(mrb);
    unpack_ext_registry = mrb_hash_new(mrb);

    mrb_define_method(mrb, mrb->object_class, "to_msgpack", mrb_msgpack_pack_object, MRB_ARGS_NONE());
    mrb_define_method(mrb, mrb->string_class, "to_msgpack", mrb_msgpack_pack_string, MRB_ARGS_NONE());
    mrb_define_method(mrb, mrb->array_class, "to_msgpack", mrb_msgpack_pack_array, MRB_ARGS_NONE());
    mrb_define_method(mrb, mrb->hash_class, "to_msgpack", mrb_msgpack_pack_hash, MRB_ARGS_NONE());
    mrb_define_method(mrb, mrb->float_class, "to_msgpack", mrb_msgpack_pack_float, MRB_ARGS_NONE());
    mrb_define_method(mrb, mrb->fixnum_class, "to_msgpack", mrb_msgpack_pack_fixnum, MRB_ARGS_NONE());
    mrb_define_method(mrb, mrb->true_class, "to_msgpack", mrb_msgpack_pack_true, MRB_ARGS_NONE());
    mrb_define_method(mrb, mrb->false_class, "to_msgpack", mrb_msgpack_pack_false, MRB_ARGS_NONE());
    mrb_define_method(mrb, mrb->nil_class, "to_msgpack", mrb_msgpack_pack_nil, MRB_ARGS_NONE());

    msgpack_mod = mrb_define_module(mrb, "MessagePack");
    mrb_define_class_under(mrb, msgpack_mod, "Error", E_RUNTIME_ERROR);
    mrb_define_module_function(mrb, msgpack_mod, "unpack", mrb_msgpack_unpack, (MRB_ARGS_ARG(1, 0)|MRB_ARGS_BLOCK()));
    mrb_define_module_function(mrb, msgpack_mod, "register_pack_type", mrb_msgpack_register_pack_type, (MRB_ARGS_REQ(2)|MRB_ARGS_BLOCK()));
    mrb_define_module_function(mrb, msgpack_mod, "register_unpack_type", mrb_msgpack_register_unpack_type, (MRB_ARGS_REQ(1)|MRB_ARGS_BLOCK()));
}

void mrb_mruby_simplemsgpack_ext_gem_final(mrb_state* mrb) { }
