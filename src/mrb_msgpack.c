#if MRB_MSGPACK_PROC_EXT < 0||MRB_MSGPACK_PROC_EXT > 127
 #error "MRB_MSGPACK_PROC_EXT must be between 0 and 127 inclusive"
#endif
#include "msgpack.h"
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
#include <mruby/numeric.h>
#include "mruby/msgpack.h"
#include <mruby/dump.h>
#include <mruby/proc.h>

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

MRB_INLINE void
mrb_msgpack_pack_fixnum_value(mrb_state *mrb, mrb_value self, msgpack_packer* pk)
{
#ifdef MRB_INT16
    int rc = msgpack_pack_int16(pk, mrb_fixnum(self));
#elif defined(MRB_INT64)
    int rc = msgpack_pack_int64(pk, mrb_fixnum(self));
#else
    int rc = msgpack_pack_int32(pk, mrb_fixnum(self));
#endif
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
mrb_msgpack_pack_proc_value(mrb_state *mrb, mrb_value proc_val, msgpack_packer *pk)
{
    struct RProc *proc = mrb_proc_ptr(proc_val);
    if (unlikely(MRB_PROC_CFUNC_P(proc))) {
        mrb_raise(mrb, E_TYPE_ERROR, "cannot pack C proc");
    }

    uint8_t *bin = NULL;
    size_t bin_size = 0;
    int result = mrb_dump_irep(mrb, proc->body.irep, DUMP_ENDIAN_LIL, &bin, &bin_size);
    if (unlikely(result != MRB_DUMP_OK)) {
        mrb_raise(mrb, E_MSGPACK_ERROR, "cannot dump irep");
    }

    struct mrb_jmpbuf* prev_jmp = mrb->jmp;
    struct mrb_jmpbuf c_jmp;

    MRB_TRY(&c_jmp)
    {
        mrb->jmp = &c_jmp;
        int rc = msgpack_pack_ext(pk, bin_size, MRB_MSGPACK_PROC_EXT);
        if (likely(rc == 0))
            rc = msgpack_pack_ext_body(pk, bin, bin_size);
        if (unlikely(rc < 0))
            mrb_raise(mrb, E_MSGPACK_ERROR, "cannot pack proc");

        mrb_free(mrb, bin);
        mrb->jmp = prev_jmp;
    }
    MRB_CATCH(&c_jmp)
    {
        mrb->jmp = prev_jmp;
        mrb_free(mrb, bin);
        MRB_THROW(mrb->jmp);
    }
    MRB_END_EXC(&c_jmp);
}

MRB_INLINE void
mrb_msgpack_pack_string_value(mrb_state *mrb, mrb_value self, msgpack_packer* pk)
{
    int rc = -1;
    if (mrb_str_is_utf8(self)) {
        rc = msgpack_pack_str(pk, RSTRING_LEN(self));
        if (likely(rc == 0))
            rc = msgpack_pack_str_body(pk, RSTRING_PTR(self), RSTRING_LEN(self));
    } else {
        rc = msgpack_pack_bin(pk, RSTRING_LEN(self));
        if (likely(rc == 0))
            rc = msgpack_pack_bin_body(pk, RSTRING_PTR(self), RSTRING_LEN(self));
    }
    if (unlikely(rc < 0)) {
        mrb_raise(mrb, E_MSGPACK_ERROR, "cannot pack string");
    }
}

MRB_INLINE mrb_value
mrb_msgpack_get_ext_config(mrb_state* mrb, mrb_value obj)
{
    mrb_value ext_packers = mrb_const_get(mrb,
        mrb_obj_value(mrb_module_get(mrb, "MessagePack")),
        mrb_intern_lit(mrb, "_ExtPackers"));
    mrb_value obj_class = mrb_obj_value(mrb_obj_class(mrb, obj));
    mrb_value ext_config = mrb_hash_get(mrb, ext_packers, obj_class);

    if (mrb_test(ext_config)) {
        return ext_config;
    }

    int arena_index = mrb_gc_arena_save(mrb);
    mrb_value ext_type_classes = mrb_hash_keys(mrb, ext_packers);
    mrb_int classes_count = RARRAY_LEN(ext_type_classes);

    mrb_int i;
    for (i = 0; i < classes_count; i += 1) {
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

MRB_INLINE mrb_bool
mrb_msgpack_pack_ext_value(mrb_state* mrb, mrb_value self, msgpack_packer* pk)
{
    mrb_value ext_config = mrb_msgpack_get_ext_config(mrb, self);

    if (mrb_nil_p(ext_config)) {
        return FALSE;
    }

    mrb_value packed = mrb_yield(mrb, mrb_hash_get(mrb, ext_config, mrb_symbol_value(mrb_intern_lit(mrb, "packer"))), self);

    if (unlikely(!mrb_string_p(packed))) {
        mrb_raise(mrb, E_TYPE_ERROR, "no string returned by ext type packer");
    }

    int rc = msgpack_pack_ext(pk, RSTRING_LEN(packed), mrb_fixnum(mrb_hash_get(mrb, ext_config, mrb_symbol_value(mrb_intern_lit(mrb, "type")))));
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
        case MRB_TT_PROC:
            mrb_msgpack_pack_proc_value(mrb, self, pk);
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
    mrb_int ary_pos;
    for (ary_pos = 0; ary_pos != RARRAY_LEN(self); ary_pos++) {
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
    mrb_int hash_pos;
    for (hash_pos = 0; hash_pos != RARRAY_LEN(keys); hash_pos++) {
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
mrb_msgpack_pack_proc(mrb_state* mrb, mrb_value self)
{
    msgpack_packer pk;
    mrb_msgpack_data data;
    data.mrb = mrb;
    data.buffer = mrb_str_new(mrb, NULL, 0);
    msgpack_packer_init(&pk, &data, mrb_msgpack_data_write);

    mrb_msgpack_pack_proc_value(mrb, self, &pk);

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
    data.mrb = mrb;
    data.buffer = mrb_str_new(mrb, NULL, 0);
    msgpack_packer_init(&pk, &data, mrb_msgpack_data_write);

    int rc = msgpack_pack_true(&pk);
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
    data.mrb = mrb;
    data.buffer = mrb_str_new(mrb, NULL, 0);
    msgpack_packer_init(&pk, &data, mrb_msgpack_data_write);

    int rc = msgpack_pack_false(&pk);
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
    data.mrb = mrb;
    data.buffer = mrb_str_new(mrb, NULL, 0);
    msgpack_packer_init(&pk, &data, mrb_msgpack_data_write);

    int rc = msgpack_pack_nil(&pk);
    if (unlikely(rc < 0)) {
        mrb_raise(mrb, E_MSGPACK_ERROR, "cannot pack nil");
    }

    return data.buffer;
}

MRB_INLINE mrb_value
mrb_unpack_msgpack_proc(mrb_state *mrb, msgpack_object obj)
{
#ifdef MRB_USE_ETEXT_EDATA
    uint8_t *bin = mrb_malloc(mrb, obj.via.ext.size);
    memcpy(bin, obj.via.ext.ptr, obj.via.ext.size);
    mrb_irep *irep = mrb_read_irep(mrb, bin);
    mrb_free(mrb, bin);

    if (!irep) {
        mrb_raise(mrb, E_SCRIPT_ERROR, "irep load error");
    }

    return mrb_obj_value(mrb_proc_new(mrb, irep));
#else
    mrb_raise(mrb, E_RUNTIME_ERROR, "mruby was compiled without MRB_USE_ETEXT_EDATA, cannot unpack procs");
#endif
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
        case MSGPACK_OBJECT_FLOAT32:
            return mrb_float_value(mrb, obj.via.f64);
        case MSGPACK_OBJECT_FLOAT64:
#ifdef MRB_USE_FLOAT
            mrb_raise(mrb, E_RUNTIME_ERROR, "mruby was compiled with MRB_USE_FLOAT, cannot unpack a double");
#else
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
            if (obj.via.ext.type == MRB_MSGPACK_PROC_EXT) {
                return mrb_unpack_msgpack_proc(mrb, obj);
            } else {
                mrb_value unpacker = mrb_hash_get(mrb,
                    mrb_const_get(mrb, mrb_obj_value(mrb_module_get(mrb, "MessagePack")), mrb_intern_lit(mrb, "_ExtUnpackers")),
                    mrb_fixnum_value(obj.via.ext.type));
                if (unlikely(mrb_type(unpacker) != MRB_TT_PROC)) {
                    mrb_raisef(mrb, E_MSGPACK_ERROR, "Cannot unpack ext type %S", mrb_fixnum_value(obj.via.ext.type));
                }

                return mrb_yield(mrb, unpacker, mrb_str_new(mrb, obj.via.ext.ptr, obj.via.ext.size));
            }
        }
    }
}

MRB_INLINE mrb_value
mrb_unpack_msgpack_obj_array(mrb_state* mrb, msgpack_object obj)
{
    if (obj.via.array.size != 0) {
        mrb_value unpacked_array = mrb_ary_new_capa(mrb, obj.via.array.size);
        int arena_index = mrb_gc_arena_save(mrb);
        size_t array_pos;
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
        size_t map_pos;
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

static mrb_value
mrb_msgpack_pack(mrb_state* mrb, mrb_value self)
{
    mrb_value object;

    mrb_get_args(mrb, "o", &object);

    msgpack_packer pk;
    mrb_msgpack_data data;
    data.mrb = mrb;
    data.buffer = mrb_str_new(mrb, NULL, 0);
    msgpack_packer_init(&pk, &data, mrb_msgpack_data_write);

    mrb_msgpack_pack_value(mrb, object, &pk);

    return data.buffer;
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

    if (type == MRB_MSGPACK_PROC_EXT) {
        mrb_raise(mrb, E_ARGUMENT_ERROR, "type is already registered for procs");
    }

    if (type < 0 || type > 127) {
        mrb_raise(mrb, E_ARGUMENT_ERROR, "ext type out of range");
    }

    if (mrb_type(block) != MRB_TT_PROC) {
        mrb_raise(mrb, E_ARGUMENT_ERROR, "no block given");
    }

    ext_packers = mrb_const_get(mrb, self, mrb_intern_lit(mrb, "_ExtPackers"));
    ext_config = mrb_hash_new_capa(mrb, 2);
    mrb_hash_set(mrb, ext_config, mrb_symbol_value(mrb_intern_lit(mrb, "type")), mrb_fixnum_value(type));
    mrb_hash_set(mrb, ext_config, mrb_symbol_value(mrb_intern_lit(mrb, "packer")), block);
    mrb_hash_set(mrb, ext_packers, mrb_class, ext_config);

    return mrb_nil_value();
}

static mrb_value
mrb_msgpack_register_unpack_type(mrb_state* mrb, mrb_value self)
{
    mrb_int type;
    mrb_value block = mrb_nil_value();

    mrb_get_args(mrb, "i&", &type, &block);

    if (type == MRB_MSGPACK_PROC_EXT) {
        mrb_raise(mrb, E_ARGUMENT_ERROR, "type is already registered for procs");
    }

    if (type < 0 || type > 127) {
        mrb_raise(mrb, E_ARGUMENT_ERROR, "ext type out of range");
    }

    if (mrb_type(block) != MRB_TT_PROC) {
        mrb_raise(mrb, E_ARGUMENT_ERROR, "no block given");
    }

    mrb_hash_set(mrb, mrb_const_get(mrb, self, mrb_intern_lit(mrb, "_ExtUnpackers")), mrb_fixnum_value(type), block);

    return mrb_nil_value();
}

void
mrb_mruby_simplemsgpack_gem_init(mrb_state* mrb)
{
    struct RClass* msgpack_mod;

    mrb_define_method(mrb, mrb->object_class, "to_msgpack", mrb_msgpack_pack_object, MRB_ARGS_NONE());
    mrb_define_method(mrb, mrb->proc_class, "to_msgpack", mrb_msgpack_pack_proc, MRB_ARGS_NONE());
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

    const char *version = msgpack_version();
    mrb_define_const(mrb, msgpack_mod, "Version", mrb_str_new_static(mrb, version, strlen(version)));
#ifdef MRB_USE_ETEXT_EDATA
    mrb_define_const(mrb, msgpack_mod, "UNPACK_PROCS", mrb_true_value());
#else
    mrb_define_const(mrb, msgpack_mod, "UNPACK_PROCS", mrb_false_value());
#endif
    mrb_define_const(mrb, msgpack_mod, "_ExtPackers", mrb_hash_new(mrb));
    mrb_define_const(mrb, msgpack_mod, "_ExtUnpackers", mrb_hash_new(mrb));

    mrb_define_module_function(mrb, msgpack_mod, "pack", mrb_msgpack_pack, (MRB_ARGS_REQ(1)));
    mrb_define_module_function(mrb, msgpack_mod, "unpack", mrb_msgpack_unpack, (MRB_ARGS_REQ(1)|MRB_ARGS_BLOCK()));
    mrb_define_module_function(mrb, msgpack_mod, "register_pack_type", mrb_msgpack_register_pack_type, (MRB_ARGS_REQ(2)|MRB_ARGS_BLOCK()));
    mrb_define_module_function(mrb, msgpack_mod, "register_unpack_type", mrb_msgpack_register_unpack_type, (MRB_ARGS_REQ(1)|MRB_ARGS_BLOCK()));
}

void mrb_mruby_simplemsgpack_gem_final(mrb_state* mrb) { }
