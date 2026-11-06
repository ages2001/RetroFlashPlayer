/* abc.h - ActionScript 3 bytecode (ABC) container parser */
#ifndef FLASHNT_ABC_H
#define FLASHNT_ABC_H

#include "flashnt.h"

enum {
    NS_NAMESPACE = 0x08, NS_PACKAGE = 0x16, NS_PACKAGE_INTERNAL = 0x17,
    NS_PROTECTED = 0x18, NS_EXPLICIT = 0x19, NS_STATIC_PROTECTED = 0x1A, NS_PRIVATE = 0x05
};
enum {
    MN_QNAME = 0x07, MN_QNAME_A = 0x0D, MN_RTQNAME = 0x0F, MN_RTQNAME_A = 0x10,
    MN_RTQNAME_L = 0x11, MN_RTQNAME_LA = 0x12, MN_MULTINAME = 0x09, MN_MULTINAME_A = 0x0E,
    MN_MULTINAME_L = 0x1B, MN_MULTINAME_LA = 0x1C, MN_TYPENAME = 0x1D
};
enum { TRAIT_SLOT = 0, TRAIT_METHOD = 1, TRAIT_GETTER = 2, TRAIT_SETTER = 3, TRAIT_CLASS = 4, TRAIT_FUNCTION = 5, TRAIT_CONST = 6 };
enum { TATTR_FINAL = 1, TATTR_OVERRIDE = 2, TATTR_METADATA = 4 };
enum { MF_NEED_ARGUMENTS = 1, MF_NEED_ACTIVATION = 2, MF_NEED_REST = 4, MF_HAS_OPTIONAL = 8, MF_NATIVE = 0x20, MF_SET_DXNS = 0x40, MF_HAS_PARAM_NAMES = 0x80 };
enum { CF_SEALED = 1, CF_FINAL = 2, CF_INTERFACE = 4, CF_PROTECTED_NS = 8, CF_NOINST = 0x100, CF_NOINST_PLAIN = 0x200 };

/* constant kinds (used by optional params / slot defaults) */
enum {
    CK_INT = 0x03, CK_UINT = 0x04, CK_DOUBLE = 0x06, CK_UTF8 = 0x01, CK_TRUE = 0x0B, CK_FALSE = 0x0A,
    CK_NULL = 0x0C, CK_UNDEFINED = 0x00, CK_NAMESPACE = 0x08, CK_PACKAGE_NS = 0x16, CK_PKG_INTERNAL_NS = 0x17,
    CK_PROTECTED_NS = 0x18, CK_EXPLICIT_NS = 0x19, CK_STATIC_PROTECTED_NS = 0x1A, CK_PRIVATE_NS = 0x05
};

typedef struct { u8 kind; u32 name; } AbcNamespace;        /* name: string index */
typedef struct { u32 count; u32 *ns; } AbcNsSet;
typedef struct {
    u8  kind;
    u32 ns;        /* QName / RTQName? : namespace index */
    u32 name;      /* string index (0 = runtime / any) */
    u32 ns_set;    /* Multiname kinds */
    u32 type_base; u32 type_param_count; u32 *type_params;  /* TypeName */
} AbcMultiname;

typedef struct { u32 val; u8 kind; } AbcOption;

typedef struct {
    u32  param_count;
    u32  return_type;
    u32 *param_types;
    u32  name;
    u8   flags;
    u32  option_count; AbcOption *options;
    u32 *param_names;
    s32  body;         /* index into bodies, -1 if none (native/abstract) */
} AbcMethod;

typedef struct {
    u32 name;          /* multiname index */
    u8  kind;          /* low 4 bits */
    u8  attrs;
    u32 slot_id;
    u32 type_name;     /* slot/const */
    u32 vindex; u8 vkind;
    u32 classi;        /* class trait */
    u32 method;        /* method/getter/setter/function */
    u32 disp_id;
    u32 metadata_count; u32 *metadata;
} AbcTrait;

typedef struct {
    u32 name, super_name;
    u8  flags;
    u32 protected_ns;
    u32 interface_count; u32 *interfaces;
    u32 iinit;
    u32 trait_count; AbcTrait *traits;
} AbcInstance;

typedef struct {
    u32 cinit;
    u32 trait_count; AbcTrait *traits;
} AbcClass;

typedef struct {
    u32 init;
    u32 trait_count; AbcTrait *traits;
} AbcScript;

typedef struct { u32 name; u32 item_count; u32 *keys; u32 *vals; } AbcMetadata;
typedef struct { u32 from, to, target, exc_type, var_name; } AbcException;

typedef struct {
    u32 method;
    u32 max_stack, local_count, init_scope_depth, max_scope_depth;
    const u8 *code; u32 code_len;
    u32 exception_count; AbcException *exceptions;
    u32 trait_count; AbcTrait *traits;
} AbcBody;

typedef struct AbcFile {
    u16 minor, major;
    int is_builtin;   /* the player's own ActionScript library (as3/) */
    u32 int_count;    s32 *ints;
    u32 uint_count;   u32 *uints;
    u32 double_count; double *doubles;
    u32 string_count; const char **strings; u32 *string_lens;   /* strings[0] = "" */
    u32 ns_count;     AbcNamespace *namespaces;
    u32 nsset_count;  AbcNsSet *nssets;
    u32 mn_count;     AbcMultiname *multinames;
    u32 method_count; AbcMethod *methods;
    u32 metadata_count; AbcMetadata *metadata;
    u32 class_count;  AbcInstance *instances; AbcClass *classes;
    u32 script_count; AbcScript *scripts;
    u32 body_count;   AbcBody *bodies;
    u8  *owned;       /* copies of strings (NUL terminated) live here */
    size_t owned_len;
    void *rt;         /* runtime per-ABC data (classes created, etc.) */
    s32 dup_body;     /* 1 + index of a method that has two bodies (0 = none) */
    void *movie;      /* SWFMovie the DoABC tag came from (player side) */
} AbcFile;

AbcFile *abc_parse(const u8 *data, size_t len);
extern int abc_error; extern u32 abc_error_arg, abc_error_arg2;   /* VerifyError id of the last failed parse (0 = malformed) */
void     abc_free(AbcFile *a);

/* helpers */
const char *abc_str(const AbcFile *a, u32 idx);        /* "" for 0 */
const char *abc_mn_name(const AbcFile *a, u32 mn);     /* simple name of a multiname ("" if runtime) */
const char *abc_ns_uri(const AbcFile *a, u32 ns);
int         abc_ns_kind(const AbcFile *a, u32 ns);

/* bytecode operand reading */
u32 abc_read_u30(const u8 **p);
s32 abc_read_s24(const u8 **p);

#endif
