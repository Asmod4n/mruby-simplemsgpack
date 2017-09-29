#include <msgpack.h>
#if (MSGPACK_VERSION_MAJOR < 1)
 #error "mrb-simplemsgpack needs at least msgpack-c 1"
#endif
#include <mruby.h>
#include <mruby/array.h>
#include <mruby/class.h>
#include <mruby/data.h>
#include <mruby/error.h>
#include <mruby/hash.h>
#include <mruby/string.h>
#include <mruby/throw.h>
#include <mruby/variable.h>
#include <mruby/numeric.h>
#include "mruby/msgpack.h"
#include <string.h>

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

MRB_INLINE int
mrb_msgpack_data_write(void* data, const char* buf, size_t len)
{
    mrb_str_cat(((mrb_msgpack_data*)data)->mrb, ((mrb_msgpack_data*)data)->buffer, buf, len);
    if (unlikely(((mrb_msgpack_data*)data)->mrb->exc)) {
        return -1;
    } else {
        return 0;
    }
}

#define pack_fixnum_helper_(x, pk, self) msgpack_pack_int##x(pk, mrb_fixnum(self))
#define pack_fixnum_helper(x, pk, self) pack_fixnum_helper_(x, pk, self)
#define mrb_msgpack_pack_int(pk, self) pack_fixnum_helper(MRB_INT_BIT, pk, self)

MRB_INLINE void
mrb_msgpack_pack_fixnum_value(mrb_state *mrb, mrb_value self, msgpack_packer* pk)
{
    int rc = mrb_msgpack_pack_int(pk, self);
    if (unlikely(rc < 0)) {
        mrb_raise(mrb, E_MSGPACK_ERROR, "cannot pack fixnum");
    }
}

MRB_INLINE void
mrb_msgpack_pack_float_value(mrb_state *mrb, mrb_value self, msgpack_packer* pk)
{
#ifdef MRB_USE_FLOAT
    int rc = msgpack_pack_float(pk, mrb_float(self));
#else
    int rc = msgpack_pack_double(pk, mrb_float(self));
#endif
    if (unlikely(rc < 0)) {
        mrb_raise(mrb, E_MSGPACK_ERROR, "cannot pack float");
    }
}

MRB_INLINE void
mrb_msgpack_pack_string_value(mrb_state *mrb, mrb_value self, msgpack_packer* pk)
{
    int rc;
    if (memchr(RSTRING_PTR(self), 0, RSTRING_LEN(self))) {
        rc = msgpack_pack_bin(pk, RSTRING_LEN(self));
        if (likely(rc == 0))
            rc = msgpack_pack_bin_body(pk, RSTRING_PTR(self), RSTRING_LEN(self));
    } else {
        rc = msgpack_pack_str(pk, RSTRING_LEN(self));
        if (likely(rc == 0))
            rc = msgpack_pack_str_body(pk, RSTRING_PTR(self), RSTRING_LEN(self));
    }
    if (unlikely(rc < 0)) {
        mrb_raise(mrb, E_MSGPACK_ERROR, "cannot pack string");
    }
}

static mrb_value
mrb_msgpack_get_ext_config(mrb_state* mrb, mrb_value obj)
{
    int arena_index;
    mrb_value ext_type_classes;
    mrb_int classes_count;

    mrb_value ext_packers = mrb_const_get(mrb,
        mrb_obj_value(mrb_module_get(mrb, "MessagePack")),
        mrb_intern_lit(mrb, "_ExtPackers"));
    mrb_value obj_class = mrb_obj_value(mrb_obj_class(mrb, obj));
    mrb_value ext_config = mrb_hash_get(mrb, ext_packers, obj_class);

    if (mrb_hash_p(ext_config)) {
        return ext_config;
    }

    arena_index = mrb_gc_arena_save(mrb);
    ext_type_classes = mrb_hash_keys(mrb, ext_packers);
    classes_count = RARRAY_LEN(ext_type_classes);

    for (mrb_int i = 0; i < classes_count; i += 1) {
        mrb_value ext_type_class = mrb_ary_ref(mrb, ext_type_classes, i);

        if (mrb_obj_is_kind_of(mrb, obj, mrb_class_ptr(ext_type_class))) {
            ext_config = mrb_hash_get(mrb, ext_packers, ext_type_class);
            mrb_hash_set(mrb, ext_packers, obj_class, ext_config);
            mrb_gc_arena_restore(mrb, arena_index);
            return ext_config;
        }
    }

    mrb_gc_arena_restore(mrb, arena_index);

    return mrb_nil_value();
}

static mrb_bool
mrb_msgpack_pack_ext_value(mrb_state* mrb, mrb_value self, msgpack_packer* pk)
{
    mrb_value ext_config, packer, packed, type;
    int rc;

    ext_config = mrb_msgpack_get_ext_config(mrb, self);
    if (!mrb_hash_p(ext_config)) {
        return FALSE;
    }

    packer = mrb_hash_get(mrb, ext_config, mrb_symbol_value(mrb_intern_lit(mrb, "packer")));
    if (unlikely(mrb_type(packer) != MRB_TT_PROC)) {
        mrb_raise(mrb, E_TYPE_ERROR, "malformed packer");
    }

    packed = mrb_yield(mrb, packer, self);
    if (unlikely(!mrb_string_p(packed))) {
        mrb_raise(mrb, E_TYPE_ERROR, "no string returned by ext type packer");
    }

    type = mrb_hash_get(mrb, ext_config, mrb_symbol_value(mrb_intern_lit(mrb, "type")));
    if (unlikely(!mrb_fixnum_p(type))) {
        mrb_raise(mrb, E_TYPE_ERROR, "malformed type");
    }

    rc = msgpack_pack_ext(pk, RSTRING_LEN(packed), mrb_fixnum(type));
    if (likely(rc == 0))
        rc = msgpack_pack_ext_body(pk, RSTRING_PTR(packed), RSTRING_LEN(packed));
    if (unlikely(rc < 0))
        mrb_raise(mrb, E_MSGPACK_ERROR, "cannot pack object");

    return TRUE;
}

MRB_INLINE void
mrb_msgpack_pack_array_value(mrb_state* mrb, mrb_value self, msgpack_packer* pk);

MRB_INLINE void
mrb_msgpack_pack_hash_value(mrb_state* mrb, mrb_value self, msgpack_packer* pk);

MRB_INLINE void
mrb_msgpack_pack_value(mrb_state* mrb, mrb_value self, msgpack_packer* pk)
{
    int rc = 0;
    switch (mrb_type(self)) {
        case MRB_TT_FALSE: {
            if (!mrb_fixnum(self)) {
                rc = msgpack_pack_nil(pk);
            } else {
                rc = msgpack_pack_false(pk);
            }
        } break;
        case MRB_TT_TRUE:
            rc = msgpack_pack_true(pk);
            break;
        case MRB_TT_FIXNUM:
            mrb_msgpack_pack_fixnum_value(mrb, self, pk);
            break;
        case MRB_TT_FLOAT:
            mrb_msgpack_pack_float_value(mrb, self, pk);
            break;
        case MRB_TT_ARRAY:
            mrb_msgpack_pack_array_value(mrb, self, pk);
            break;
        case MRB_TT_HASH:
            mrb_msgpack_pack_hash_value(mrb, self, pk);
            break;
        case MRB_TT_STRING:
            mrb_msgpack_pack_string_value(mrb, self, pk);
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
                            mrb_msgpack_pack_fixnum_value(mrb, try_convert, pk);
                        } else {
                            try_convert = mrb_check_convert_type(mrb, self, MRB_TT_STRING, "String", "to_str");
                            if (mrb_string_p(try_convert)) {
                                mrb_msgpack_pack_string_value(mrb, try_convert, pk);
                            } else {
                                try_convert = mrb_convert_type(mrb, self, MRB_TT_STRING, "String", "to_s");
                                mrb_msgpack_pack_string_value(mrb, try_convert, pk);
                            }
                        }
                    }
                }
            }
        }
    }
    if (unlikely(rc < 0)) {
        mrb_raise(mrb, E_MSGPACK_ERROR, "cannot pack object");
    }
}

MRB_INLINE void
mrb_msgpack_pack_array_value(mrb_state* mrb, mrb_value self, msgpack_packer* pk)
{
    int rc = msgpack_pack_array(pk, RARRAY_LEN(self));
    if (unlikely(rc < 0)) {
        mrb_raise(mrb, E_MSGPACK_ERROR, "cannot pack array");
    }
    for (mrb_int ary_pos = 0; ary_pos < RARRAY_LEN(self); ary_pos++) {
        mrb_msgpack_pack_value(mrb, mrb_ary_ref(mrb, self, ary_pos), pk);
    }
}

MRB_INLINE void
mrb_msgpack_pack_hash_value(mrb_state* mrb, mrb_value self, msgpack_packer* pk)
{
    int arena_index = mrb_gc_arena_save(mrb);
    mrb_value keys = mrb_hash_keys(mrb, self);
    int rc = msgpack_pack_map(pk, RARRAY_LEN(keys));
    if (unlikely(rc < 0)) {
        mrb_raise(mrb, E_MSGPACK_ERROR, "cannot pack hash");
    }
    for (mrb_int hash_pos = 0; hash_pos < RARRAY_LEN(keys); hash_pos++) {
        mrb_value key = mrb_ary_ref(mrb, keys, hash_pos);
        mrb_msgpack_pack_value(mrb, key, pk);
        mrb_msgpack_pack_value(mrb, mrb_hash_get(mrb, self, key), pk);
    }
    mrb_gc_arena_restore(mrb, arena_index);
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

    mrb_msgpack_pack_string_value(mrb, self, &pk);

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

    mrb_msgpack_pack_float_value(mrb, self, &pk);

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

    mrb_msgpack_pack_fixnum_value(mrb, self, &pk);

    return data.buffer;
}

static mrb_value
mrb_msgpack_pack_true(mrb_state* mrb, mrb_value self)
{
    msgpack_packer pk;
    mrb_msgpack_data data;
    int rc;

    data.mrb = mrb;
    data.buffer = mrb_str_new(mrb, NULL, 0);
    msgpack_packer_init(&pk, &data, mrb_msgpack_data_write);

    rc = msgpack_pack_true(&pk);
    if (unlikely(rc < 0)) {
        mrb_raise(mrb, E_MSGPACK_ERROR, "cannot pack true");
    }

    return data.buffer;
}

static mrb_value
mrb_msgpack_pack_false(mrb_state* mrb, mrb_value self)
{
    msgpack_packer pk;
    mrb_msgpack_data data;
    int rc;

    data.mrb = mrb;
    data.buffer = mrb_str_new(mrb, NULL, 0);
    msgpack_packer_init(&pk, &data, mrb_msgpack_data_write);

    rc = msgpack_pack_false(&pk);
    if (unlikely(rc < 0)) {
        mrb_raise(mrb, E_MSGPACK_ERROR, "cannot pack false");
    }

    return data.buffer;
}

static mrb_value
mrb_msgpack_pack_nil(mrb_state* mrb, mrb_value self)
{
    msgpack_packer pk;
    mrb_msgpack_data data;
    int rc;

    data.mrb = mrb;
    data.buffer = mrb_str_new(mrb, NULL, 0);
    msgpack_packer_init(&pk, &data, mrb_msgpack_data_write);

    rc = msgpack_pack_nil(&pk);
    if (unlikely(rc < 0)) {
        mrb_raise(mrb, E_MSGPACK_ERROR, "cannot pack nil");
    }

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
        case MSGPACK_OBJECT_BOOLEAN:
            return mrb_bool_value(obj.via.boolean);
        case MSGPACK_OBJECT_POSITIVE_INTEGER: {
            if (POSFIXABLE(obj.via.u64)) {
              return mrb_fixnum_value(obj.via.u64);
            }
            return mrb_float_value(mrb, obj.via.u64);
        }
        case MSGPACK_OBJECT_NEGATIVE_INTEGER: {
            if (NEGFIXABLE(obj.via.i64)) {
              return mrb_fixnum_value(obj.via.i64);
            }
            return mrb_float_value(mrb, obj.via.i64);
        }
#if (((MSGPACK_VERSION_MAJOR == 2) && (MSGPACK_VERSION_MINOR >= 1)) || (MSGPACK_VERSION_MAJOR > 2))
        case MSGPACK_OBJECT_FLOAT32:
            return mrb_float_value(mrb, obj.via.f64);
        case MSGPACK_OBJECT_FLOAT64:
    #ifndef MRB_USE_FLOAT
            return mrb_float_value(mrb, obj.via.f64);
    #else
            mrb_raise(mrb, E_RUNTIME_ERROR, "mruby was compiled with MRB_USE_FLOAT, cannot unpack a double");
    #endif
#else
        case MSGPACK_OBJECT_FLOAT:
            return mrb_float_value(mrb, obj.via.f64);
#endif
        case MSGPACK_OBJECT_STR:
            return mrb_str_new(mrb, obj.via.str.ptr, obj.via.str.size);
        case MSGPACK_OBJECT_ARRAY:
            return mrb_unpack_msgpack_obj_array(mrb, obj);
        case MSGPACK_OBJECT_MAP:
            return mrb_unpack_msgpack_obj_map(mrb, obj);
        case MSGPACK_OBJECT_BIN:
            return mrb_str_new(mrb, obj.via.bin.ptr, obj.via.bin.size);
        case MSGPACK_OBJECT_EXT: {
            mrb_value unpacker = mrb_hash_get(mrb,
                mrb_const_get(mrb, mrb_obj_value(mrb_module_get(mrb, "MessagePack")), mrb_intern_lit(mrb, "_ExtUnpackers")),
                mrb_fixnum_value(obj.via.ext.type));
            if (mrb_type(unpacker) != MRB_TT_PROC) {
                mrb_raisef(mrb, E_MSGPACK_ERROR, "Cannot unpack ext type %S", mrb_fixnum_value(obj.via.ext.type));
            }

            return mrb_yield(mrb, unpacker, mrb_str_new(mrb, obj.via.ext.ptr, obj.via.ext.size));
        }
    }
}

MRB_INLINE mrb_value
mrb_unpack_msgpack_obj_array(mrb_state* mrb, msgpack_object obj)
{
    if (obj.via.array.size != 0) {
        mrb_value unpacked_array = mrb_ary_new_capa(mrb, obj.via.array.size);
        int arena_index = mrb_gc_arena_save(mrb);
        uint32_t array_pos;
        for (array_pos = 0; array_pos < obj.via.array.size; array_pos++) {
            mrb_ary_push(mrb, unpacked_array, mrb_unpack_msgpack_obj(mrb, obj.via.array.ptr[array_pos]));
            mrb_gc_arena_restore(mrb, arena_index);
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
        int arena_index = mrb_gc_arena_save(mrb);
        uint32_t map_pos;
        for (map_pos = 0; map_pos < obj.via.map.size; map_pos++) {
            mrb_hash_set(mrb, unpacked_hash,
                mrb_unpack_msgpack_obj(mrb, obj.via.map.ptr[map_pos].key),
                mrb_unpack_msgpack_obj(mrb, obj.via.map.ptr[map_pos].val));
            mrb_gc_arena_restore(mrb, arena_index);
        }

        return unpacked_hash;
    } else {
        return mrb_hash_new(mrb);
    }
}

MRB_API mrb_value
mrb_msgpack_pack(mrb_state *mrb, mrb_value object)
{
    msgpack_packer pk;
    mrb_msgpack_data data;
    data.mrb = mrb;
    data.buffer = mrb_str_new(mrb, NULL, 0);
    msgpack_packer_init(&pk, &data, mrb_msgpack_data_write);

    mrb_msgpack_pack_value(mrb, object, &pk);

    return data.buffer;
}

static mrb_value
mrb_msgpack_pack_m(mrb_state* mrb, mrb_value self)
{
    mrb_value object;

    mrb_get_args(mrb, "o", &object);

    return mrb_msgpack_pack(mrb, object);
}

MRB_API mrb_value
mrb_msgpack_unpack(mrb_state *mrb, mrb_value data)
{
    msgpack_unpacked result;
    size_t off;
    msgpack_unpack_return ret;
    mrb_value unpack_return;

    if (unlikely(!mrb->jmp)) {
        mrb_raise(mrb, E_RUNTIME_ERROR, "not called inside a mruby function");
    }

    if (unlikely(!mrb_string_p(data))) {
        mrb_raise(mrb, E_TYPE_ERROR, "can only unpack String data");
    }

    off = 0;
    msgpack_unpacked_init(&result);
    ret = msgpack_unpack_next(&result, RSTRING_PTR(data), RSTRING_LEN(data), &off);
    if (ret == MSGPACK_UNPACK_SUCCESS) {
        struct mrb_jmpbuf* prev_jmp = mrb->jmp;
        struct mrb_jmpbuf c_jmp;

        MRB_TRY(&c_jmp)
        {
            mrb->jmp = &c_jmp;
            unpack_return = mrb_unpack_msgpack_obj(mrb, result.data);
            mrb->jmp = prev_jmp;
        }
        MRB_CATCH(&c_jmp)
        {
            mrb->jmp = prev_jmp;
            msgpack_unpacked_destroy(&result);
            MRB_THROW(mrb->jmp);
        }
        MRB_END_EXC(&c_jmp);
    }

    msgpack_unpacked_destroy(&result);

    switch (ret) {
        case MSGPACK_UNPACK_SUCCESS:
            return unpack_return;
        case MSGPACK_UNPACK_EXTRA_BYTES: //not used
            break;
        case MSGPACK_UNPACK_CONTINUE:
            return mrb_fixnum_value(off);
        case MSGPACK_UNPACK_PARSE_ERROR:
            mrb_raise(mrb, E_MSGPACK_ERROR, "Invalid data received");
        case MSGPACK_UNPACK_NOMEM_ERROR:
            mrb_sys_fail(mrb, "msgpack_unpack_next");
    }

    // cannot be reached because the switch case construct above chatches all cases,
    // but mrb_sys_fail isn't marked like mrb_raise and friends so this is needed.
    // if this gets unlikeley returned the program crashes.
    return mrb_undef_value();
}

static mrb_value
mrb_msgpack_unpack_m(mrb_state* mrb, mrb_value self)
{
    mrb_value data, block = mrb_nil_value();
    msgpack_unpacked result;
    size_t off;
    msgpack_unpack_return ret;
    struct mrb_jmpbuf* prev_jmp;
    struct mrb_jmpbuf c_jmp;
    mrb_value unpack_return;

    mrb_get_args(mrb, "o&", &data, &block);

    data = mrb_str_to_str(mrb, data);

    off = 0;
    msgpack_unpacked_init(&result);
    ret = msgpack_unpack_next(&result, RSTRING_PTR(data), RSTRING_LEN(data), &off);

    prev_jmp = mrb->jmp;
    unpack_return = self;
    MRB_TRY(&c_jmp)
    {
        mrb->jmp = &c_jmp;
        if (mrb_type(block) == MRB_TT_PROC) {
            while (ret == MSGPACK_UNPACK_SUCCESS) {
                mrb_yield(mrb, block, mrb_unpack_msgpack_obj(mrb, result.data));
                ret = msgpack_unpack_next(&result, RSTRING_PTR(data), RSTRING_LEN(data), &off);
            }
        } else if (ret == MSGPACK_UNPACK_SUCCESS) {
            unpack_return = mrb_unpack_msgpack_obj(mrb, result.data);
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

    switch (ret) {
        case MSGPACK_UNPACK_SUCCESS:
            return unpack_return;
        case MSGPACK_UNPACK_EXTRA_BYTES: //not used
            break;
        case MSGPACK_UNPACK_CONTINUE:
            return mrb_fixnum_value(off);
        case MSGPACK_UNPACK_PARSE_ERROR:
            mrb_raise(mrb, E_MSGPACK_ERROR, "Invalid data received");
        case MSGPACK_UNPACK_NOMEM_ERROR:
            mrb_sys_fail(mrb, "msgpack_unpack_next");
    }

    return self;
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

    ext_packers = mrb_const_get(mrb, self, mrb_intern_lit(mrb, "_ExtPackers"));
    ext_config = mrb_hash_new_capa(mrb, 2);
    mrb_hash_set(mrb, ext_config, mrb_symbol_value(mrb_intern_lit(mrb, "type")), mrb_fixnum_value(type));
    mrb_hash_set(mrb, ext_config, mrb_symbol_value(mrb_intern_lit(mrb, "packer")), block);
    mrb_hash_set(mrb, ext_packers, mrb_class, ext_config);

    return mrb_nil_value();
}

static mrb_value
mrb_msgpack_ext_packer_registered(mrb_state *mrb, mrb_value self)
{
    mrb_value mrb_class;
    mrb_get_args(mrb, "C", &mrb_class);

    return mrb_bool_value(mrb_test(mrb_hash_get(mrb, mrb_const_get(mrb, self, mrb_intern_lit(mrb, "_ExtPackers")), mrb_class)));
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

    mrb_hash_set(mrb, mrb_const_get(mrb, self, mrb_intern_lit(mrb, "_ExtUnpackers")), mrb_fixnum_value(type), block);

    return mrb_nil_value();
}

static mrb_value
mrb_msgpack_ext_unpacker_registered(mrb_state *mrb, mrb_value self)
{
    mrb_int type;
    mrb_get_args(mrb, "i", &type);

    return mrb_bool_value(!mrb_nil_p(mrb_hash_get(mrb, mrb_const_get(mrb, self, mrb_intern_lit(mrb, "_ExtUnpackers")), mrb_fixnum_value(type))));
}

void
mrb_mruby_simplemsgpack_gem_init(mrb_state* mrb)
{
    struct RClass* msgpack_mod;
    static const char *mrb_msgpack_version;

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

    mrb_msgpack_version = MSGPACK_VERSION;
    mrb_define_const(mrb, msgpack_mod, "Version", mrb_str_new_static(mrb, mrb_msgpack_version, strlen(mrb_msgpack_version)));
    mrb_define_const(mrb, msgpack_mod, "_ExtPackers", mrb_hash_new(mrb));
    mrb_define_const(mrb, msgpack_mod, "_ExtUnpackers", mrb_hash_new(mrb));

    mrb_define_module_function(mrb, msgpack_mod, "pack", mrb_msgpack_pack_m, (MRB_ARGS_REQ(1)));
    mrb_define_module_function(mrb, msgpack_mod, "unpack", mrb_msgpack_unpack_m, (MRB_ARGS_REQ(1)|MRB_ARGS_BLOCK()));
    mrb_define_module_function(mrb, msgpack_mod, "register_pack_type", mrb_msgpack_register_pack_type, (MRB_ARGS_REQ(2)|MRB_ARGS_BLOCK()));
    mrb_define_module_function(mrb, msgpack_mod, "ext_packer_registered?", mrb_msgpack_ext_packer_registered, MRB_ARGS_REQ(1));
    mrb_define_module_function(mrb, msgpack_mod, "register_unpack_type", mrb_msgpack_register_unpack_type, (MRB_ARGS_REQ(1)|MRB_ARGS_BLOCK()));
    mrb_define_module_function(mrb, msgpack_mod, "ext_unpacker_registered?", mrb_msgpack_ext_unpacker_registered, MRB_ARGS_REQ(1));
}

void mrb_mruby_simplemsgpack_gem_final(mrb_state* mrb) {}
