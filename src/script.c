/* Goxel 3D voxels editor
 *
 * copyright (c) 2023-present Guillaume Chereau <guillaume@noctua-software.com>
 *
 * Goxel is free software: you can redistribute it and/or modify it under the
 * terms of the GNU General Public License as published by the Free Software
 * Foundation, either version 3 of the License, or (at your option) any later
 * version.
 *
 * Goxel is distributed in the hope that it will be useful, but WITHOUT ANY
 * WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE.  See the GNU General Public License for more
 * details.
 *
 * You should have received a copy of the GNU General Public License along with
 * goxel.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "goxel.h"

#include "file_format.h"

#include "../ext_src/quickjs/quickjs.h"
#include "../ext_src/quickjs/quickjs-libc.h"

#define STB_DS_IMPLEMENTATION
#include "../ext_src/stb/stb_ds.h"

static JSRuntime *g_rt = NULL;
static JSContext *g_ctx = NULL;

typedef struct klass klass_t;
typedef struct attribute attribute_t;

/*
 * Represent a bound attribute of a class.
 */
struct attribute {
    const char *name;
    klass_t *klass;
    int flags;

    struct {
        int offset;
        int size;
    } member;

    JSValue (*get)(JSContext *ctx, JSValueConst this_val, int magic);
    JSValue (*set)(JSContext *ctx, JSValueConst this_val, JSValueConst val,
                   int magic);
    JSCFunction *fn;
    int magic;
};

#define MEMBER(k, m) .member = {offsetof(k, m), sizeof(((k*)0)->m)}

/*
 * Info struct for all the bound classes.
 */
struct klass {
    JSClassID id;
    JSClassDef def;
    JSCFunction *ctor;
    JSValue (*ctor_from_ptr)(
            JSContext * ctx, JSValueConst owner, void *ptr, size_t size);
    attribute_t attributes[];
};

/*
 * Base struct for all reference counted objects.
 */
typedef struct {
    int ref;
} obj_t;

/*
 * Pre declaration of all the registered classes.
 */
static klass_t vec_klass;
static klass_t box_klass;
static klass_t image_klass;
static klass_t layer_klass;
static klass_t volume_klass;
static klass_t goxel_klass;

typedef struct {
    char name[128];
    JSValue execute_fn;
} script_t;

// stb array of registered scripts
static script_t *g_scripts = NULL;

typedef struct {
    int size;
    float *values;
    JSValue owner;
} vec_t;

typedef struct {
    float (*mat)[4];
    JSValue owner;
} box_t;

static void get_vec_int(JSContext *ctx, JSValue val, int size, int *out,
                        int default_val)
{
    int i;
    JSValue v;
    vec_t *vec;

    vec = JS_GetOpaque2(ctx, val, vec_klass.id);
    if (vec) {
        for (i = 0; i < size; i++) {
            out[i] = i < vec->size ? vec->values[i] : default_val;
        }
        return;
    }

    for (i = 0; i < size; i++) {
        v = JS_GetPropertyUint32(ctx, val, i);
        JS_ToInt32(ctx, &out[i], v);
    }
}

static void get_vec_uint8(JSContext *ctx, JSValue val, int size, uint8_t *out,
                          uint8_t default_val)
{
    int buf[4];
    int i;
    get_vec_int(ctx, val, size, buf, default_val);
    for (i = 0; i < size; i++) out[i] = buf[i];
}

static JSValue js_vec_ctor(JSContext *ctx, JSValueConst new_target,
                           int argc, JSValueConst *argv)
{
    int i;
    double v;
    JSValue ret;
    vec_t *vec;

    vec = calloc(1, sizeof(*vec));
    vec->size = argc;
    vec->values = calloc(argc, sizeof(float));
    for (i = 0; i < argc; i++) {
        JS_ToFloat64(ctx, &v, argv[i]);
        vec->values[i] = v;
    }
    ret = JS_NewObjectClass(ctx, vec_klass.id);
    JS_SetOpaque(ret, vec);
    return ret;
}

static void js_vec_finalizer(JSRuntime *rt, JSValue val)
{
    vec_t *vec = JS_GetOpaque(val, vec_klass.id);
    if (JS_IsUndefined(vec->owner)) js_free_rt(rt, vec->values);
    JS_FreeValueRT(rt, vec->owner);
    js_free_rt(rt, vec);
}

static JSValue js_vec_from_ptr(
        JSContext *ctx, JSValueConst owner, void *ptr, size_t size)
{
    JSValue ret;
    vec_t *vec;

    assert(ptr != NULL);
    vec = js_mallocz(ctx, sizeof(*vec));
    vec->size = size / sizeof(float);
    assert(!JS_IsUndefined(owner));
    vec->owner = JS_DupValue(ctx, owner);
    vec->values = ptr;
    ret = JS_NewObjectClass(ctx, vec_klass.id);
    JS_SetOpaque(ret, vec);
    return ret;
}

static JSValue new_js_vec3(JSContext *ctx, float x, float y, float z)
{
    JSValue ret;
    vec_t *vec;
    vec = js_mallocz(ctx, sizeof(*vec));
    vec->owner = JS_UNDEFINED;
    vec->size = 3;
    vec->values = calloc(3, sizeof(float));
    vec->values[0] = x;
    vec->values[1] = y;
    vec->values[2] = z;
    ret = JS_NewObjectClass(ctx, vec_klass.id);
    JS_SetOpaque(ret, vec);
    return ret;
}

static JSValue new_js_vec4(JSContext *ctx, float x, float y, float z, float w)
{
    JSValue ret;
    vec_t *vec;
    vec = js_mallocz(ctx, sizeof(*vec));
    vec->owner = JS_UNDEFINED;
    vec->size = 4;
    vec->values = calloc(4, sizeof(float));
    vec->values[0] = x;
    vec->values[1] = y;
    vec->values[2] = z;
    vec->values[3] = w;
    ret = JS_NewObjectClass(ctx, vec_klass.id);
    JS_SetOpaque(ret, vec);
    return ret;
}

static JSValue js_vec_get(JSContext *ctx, JSValueConst this_val, int idx)
{
    vec_t *vec = JS_GetOpaque2(ctx, this_val, vec_klass.id);
    if (!vec)
        return JS_EXCEPTION;
    if (idx >= vec->size)
        return JS_EXCEPTION;
    return JS_NewFloat64(ctx, vec->values[idx]);
}

static JSValue js_vec_set(
        JSContext *ctx, JSValueConst this_val, JSValue val, int idx)
{
    vec_t *vec;
    double v;

    vec = JS_GetOpaque2(ctx, this_val, vec_klass.id);
    if (!vec)
        return JS_EXCEPTION;
    if (idx >= vec->size)
        return JS_EXCEPTION;
    if (JS_ToFloat64(ctx, &v, val))
        return JS_EXCEPTION;
    vec->values[idx] = v;
    return JS_UNDEFINED;
}

static klass_t vec_klass = {
    .def.class_name = "Vec",
    .def.finalizer = js_vec_finalizer,
    .ctor = js_vec_ctor,
    .ctor_from_ptr = js_vec_from_ptr,
    .attributes = {
        {"x", .get=js_vec_get, .set=js_vec_set, .magic=0},
        {"y", .get=js_vec_get, .set=js_vec_set, .magic=1},
        {"z", .get=js_vec_get, .set=js_vec_set, .magic=2},
        {"w", .get=js_vec_get, .set=js_vec_set, .magic=3},
        {"r", .get=js_vec_get, .set=js_vec_set, .magic=0},
        {"g", .get=js_vec_get, .set=js_vec_set, .magic=1},
        {"b", .get=js_vec_get, .set=js_vec_set, .magic=2},
        {"a", .get=js_vec_get, .set=js_vec_set, .magic=3},
        {}
    }
};

static JSValue js_box_from_ptr(
        JSContext *ctx, JSValueConst owner, void *ptr, size_t size)
{
    JSValue ret;
    box_t *box;
    if (ptr == NULL) return JS_NULL;
    assert(size == 16 * sizeof(float));
    box = js_mallocz(ctx, sizeof(*box));
    box->owner = JS_DupValue(ctx, owner);
    if (JS_IsUndefined(owner)) {
        box->mat = calloc(1, size);
        memcpy(box->mat, ptr, size);
    } else {
        box->mat = ptr;
    }
    ret = JS_NewObjectClass(ctx, box_klass.id);
    JS_SetOpaque(ret, box);
    return ret;
}

static JSValue js_box_iterVoxels(JSContext *ctx, JSValueConst this_val,
                                 int argc, JSValueConst *argv)
{
    float inv[4][4];
    int aabb[2][3];
    int x, y, z;
    float pos[3], localpos[3];
    box_t *box;
    JSValue js_pos, val;
    const float EPS = 1e-8;
    const float L = 1 + EPS;

    if (argc != 1) return JS_EXCEPTION;
    box = JS_GetOpaque(this_val, box_klass.id);
    mat4_invert(box->mat, inv);
    box_get_aabb(box->mat, aabb);
    for (z = aabb[0][2]; z < aabb[1][2]; z++) {
        for (y = aabb[0][1]; y < aabb[1][1]; y++) {
            for (x = aabb[0][0]; x < aabb[1][0]; x++) {
                vec3_set(pos, x, y, z);
                mat4_mul_vec3(inv, pos, localpos);
                if (    localpos[0] < -L || localpos[0] > +L ||
                        localpos[1] < -L || localpos[1] > +L ||
                        localpos[2] < -L || localpos[2] > +L)
                    continue;
                js_pos = new_js_vec3(ctx, x, y, z);
                val = JS_Call(ctx, argv[0], JS_NULL, 1, &js_pos);
                JS_FreeValue(ctx, js_pos);
                if (JS_IsException(val))
                    return val;
                JS_FreeValue(ctx, val);
            }
        }
    }
    return JS_UNDEFINED;
}

static JSValue js_box_worldToLocal(JSContext *ctx, JSValueConst this_val,
                                   int argc, JSValueConst *argv)
{
    return JS_EXCEPTION;
}

static void js_box_finalizer(JSRuntime *rt, JSValue val)
{
    box_t *box = JS_GetOpaque(val, box_klass.id);
    if (JS_IsUndefined(box->owner)) js_free_rt(rt, box->mat);
    JS_FreeValueRT(rt, box->owner);
    js_free_rt(rt, box);
}

static klass_t box_klass = {
    .def.class_name = "Box",
    .def.finalizer = js_box_finalizer,
    .ctor_from_ptr = js_box_from_ptr,
    .attributes = {
        {"iterVoxels", .fn=js_box_iterVoxels},
        {"worldToLocal", .fn=js_box_worldToLocal},
        {}
    },
};

static JSValue js_volume_ctor(JSContext *ctx, JSValueConst new_target,
                            int argc, JSValueConst *argv)
{
    JSValue ret;
    volume_t *volume;
    volume = volume_new();
    ret = JS_NewObjectClass(ctx, volume_klass.id);
    JS_SetOpaque(ret, volume);
    return ret;
}

static void js_volume_finalizer(JSRuntime *ctx, JSValue this_val)
{
    volume_t *volume;
    volume = JS_GetOpaque(this_val, volume_klass.id);
    volume_delete(volume);
}

static JSValue js_volume_copy(JSContext *ctx, JSValueConst this_val,
                              int argc, JSValueConst *argv)
{
    JSValue ret;
    volume_t *volume;
    volume = JS_GetOpaque(this_val, volume_klass.id);
    volume = volume_copy(volume);
    ret = JS_NewObjectClass(ctx, volume_klass.id);
    JS_SetOpaque(ret, volume);
    return ret;
}

static JSValue js_volume_iter(JSContext *ctx, JSValueConst this_val,
                            int argc, JSValueConst *argv)
{
    volume_t *volume;
    JSValue args[2];
    volume_iterator_t iter;
    int pos[3];
    uint8_t value[4];

    volume = JS_GetOpaque(this_val, volume_klass.id);
    iter = volume_get_iterator(volume,
            VOLUME_ITER_VOXELS | VOLUME_ITER_SKIP_EMPTY);
    while (volume_iter(&iter, pos)) {
        volume_get_at(volume, &iter, pos, value);
        if (value[3] == 0) continue;
        args[0] = new_js_vec3(ctx, pos[0], pos[1], pos[2]);
        args[1] = new_js_vec4(ctx, value[0], value[1], value[2], value[3]);
        JS_Call(ctx, argv[0], JS_UNDEFINED, 2, args);
        JS_FreeValue(ctx, args[0]);
        JS_FreeValue(ctx, args[1]);
    };
    return JS_UNDEFINED;
}

static JSValue js_volume_setAt(JSContext *ctx, JSValueConst this_val,
                             int argc, JSValueConst *argv)
{
    volume_t *volume;
    int pos[3];
    uint8_t v[4];

    get_vec_int(ctx, argv[0], 3, pos, 0);
    get_vec_uint8(ctx, argv[1], 4, v, 255);
    volume = JS_GetOpaque2(ctx, this_val, volume_klass.id);
    volume_set_at(volume, NULL, pos, v);
    return JS_UNDEFINED;
}

static JSValue js_volume_save(JSContext *ctx, JSValueConst this_val,
                            int argc, JSValueConst *argv)
{
    volume_t *volume;
    image_t *img;
    const char *path, *format = NULL;
    const file_format_t *f;
    int err;

    volume = JS_GetOpaque2(ctx, this_val, volume_klass.id);
    path = JS_ToCString(ctx, argv[0]);
    if (argc > 1)
        format = JS_ToCString(ctx, argv[1]);

    f = file_format_get(path, format, "w");
    if (!f) {
        fprintf(stderr, "Cannot find format for file %s\n", path);
        return JS_EXCEPTION;
    }
    img = image_new();
    volume_set(img->active_layer->volume, volume);
    err = f->export_func(f, img, path);
    if (err) {
        fprintf(stderr, "Internal error saving file %s\n", path);
        return JS_EXCEPTION;
    }
    image_delete(img);

    JS_FreeCString(ctx, path);
    JS_FreeCString(ctx, format);
    return JS_UNDEFINED;
}

static klass_t volume_klass = {
    .def.class_name = "Volume",
    .def.finalizer = js_volume_finalizer,
    .ctor = js_volume_ctor,
    .attributes = {
        {"copy", .fn=js_volume_copy},
        {"iter", .fn=js_volume_iter},
        {"setAt", .fn=js_volume_setAt},
        {"save", .fn=js_volume_save},
        {}
    }
};

static void js_image_finalizer(JSRuntime *ctx, JSValue this_val)
{
    image_t *image;
    image = JS_GetOpaque(this_val, image_klass.id);
    image_delete(image);
}

static JSValue js_image_addLayer(
        JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    image_t *image;
    layer_t *layer;
    JSValue ret;

    image = JS_GetOpaque(this_val, image_klass.id);
    layer = image_add_layer(image, NULL);
    ret = JS_NewObjectClass(ctx, layer_klass.id);
    layer->ref++;
    JS_SetOpaque(ret, (void*)layer);
    return ret;
}

static JSValue js_image_getLayersVolume(
        JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    JSValue ret;
    volume_t *volume;
    image_t *image;

    image = JS_GetOpaque(this_val, image_klass.id);
    volume = volume_copy(goxel_get_layers_volume(image));
    ret = JS_NewObjectClass(ctx, volume_klass.id);
    JS_SetOpaque(ret, (void*)volume);
    return ret;
}

static klass_t image_klass = {
    .def.class_name = "Image",
    .def.finalizer = js_image_finalizer,
    .attributes = {
        {"addLayer", .fn=js_image_addLayer},
        {"activeLayer", .klass=&layer_klass, MEMBER(image_t, active_layer)},
        {"getLayersVolume", .fn=js_image_getLayersVolume},
        {"selectionBox", .klass=&box_klass, MEMBER(image_t, selection_box)},
        {}
    }
};

static klass_t layer_klass = {
    .def.class_name = "Layer",
    .attributes = {
        {"volume", .klass=&volume_klass, MEMBER(layer_t, volume)},
        {}
    }
};

typedef struct {
    const char *name;
    JSValue execute_fn;
} script_t;

// stb array of registered scripts
static script_t *g_scripts = NULL;

static JSValue js_file_format_register(JSContext *ctx, JSValueConst this_val,
                                      int argc, JSValueConst *argv)
{
    const char *name;
    JSValue data, import_fn, export_fn;
    file_format_t f = {};

    if (argc != 1) return JS_EXCEPTION;
    data = argv[0];
    name = JS_GetPropertyStr(ctx, data, "name");
    name = JS_ToCString(ctx, name);
    if (!name) return JS_EXCEPTION;
    snprintf(f.name, sizeof(f.name), "%s", name);
    f.import_func = NULL;
    f.export_func = NULL;
    import_fn = JS_GetPropertyStr(ctx, data, "import");
    if (!JS_IsUndefined(import_fn)) {
        f.import_func = js_file_format_import;
        JS_DupValue(ctx, import_fn);
        hmput(g_file_format_import_funcs, f.name, import_fn);
    }
    export_fn = JS_GetPropertyStr(ctx, data, "export");
    if (!JS_IsUndefined(export_fn)) {
        f.export_func = js_file_format_export;
        JS_DupValue(ctx, export_fn);
        hmput(g_file_format_export_funcs, f.name, export_fn);
    }
    file_format_register(&f);
    JS_FreeCString(ctx, name);
    return JS_UNDEFINED;
}

static JSValue js_script_register(JSContext *ctx, JSValueConst this_val,
                                      int argc, JSValueConst *argv)
{
    const char *name, *description;
    JSValue data;
    script_t script = {};

    if (argc != 1) return JS_EXCEPTION;
    data = argv[0];
    name = JS_GetPropertyStr(ctx, data, "name");
    name = JS_ToCString(ctx, name);
    if (!name) return JS_EXCEPTION;
    description = JS_GetPropertyStr(ctx, data, "description");
    description = JS_ToCString(ctx, description);
    snprintf(script.name, sizeof(script.name), "%s", name);
    script.execute_fn = JS_GetPropertyStr(ctx, data, "onExecute");
    arrput(g_scripts, script);
    JS_FreeCString(ctx, name);
    return JS_UNDEFINED;
}

static JSValue js_goxel_version(JSContext *ctx, JSValueConst this_val, int magic)
{
    return JS_NewString(ctx, GOXEL_VERSION);
}

static klass_t goxel_klass = {
    .def.class_name = "Goxel",
    .attributes = {
        {"registerFormat", .fn=js_file_format_register},
        {"registerScript", .fn=js_script_register},
        {"image", .klass=&image_klass},
        {"version", .get=js_goxel_version},
    }
};

static JSValue js_palette_get_color(JSContext *ctx, JSValueConst this_val, int magic)
{
    uint8_t *color = goxel.painter.color;
    return new_js_vec4(ctx, color[0], color[1], color[2], color[3]);
}

static klass_t palette_klass = {
    .def.class_name = "Palette",
    .attributes = {
        {"color", .get=js_palette_get_color},
        {}
    }
};

static void on_script(const char *path, const char *data, int size, void *user)
{
    JSValue val, func;
    int err;

    val = JS_Eval(g_ctx, data, size, path, JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(val)) {
        js_std_dump_error(g_ctx);
        return;
    }
    JS_FreeValue(g_ctx, val);
}

static void on_dir(const char *path, void *user)
{
    assets_list(path, NULL, on_script);
}

int script_init(void)
{
    JSValue global_obj, obj, obj_class;
    const char *str;

    g_rt = JS_NewRuntime();
    g_ctx = JS_NewContext(g_rt);

    /* loader for ES6 modules */
    js_std_init_handlers(g_rt);
    /* setmodule loader base name */
    str = ".";
    JS_SetModuleLoader(g_rt, NULL, js_module_loader, str);
    js_std_add_helpers(g_ctx);

    /* system functions */
    global_obj = JS_GetGlobalObject(g_ctx);
    JS_SetPropertyStr(g_ctx, global_obj, "std", js_std_new_namespace(g_ctx));
    JS_FreeValue(g_ctx, global_obj);

    /* add goxel object */
    obj = JS_NewObjectClass(g_ctx, goxel_klass.id);
    JS_SetOpaque(obj, &goxel);
    obj_class = JS_NewClass(g_rt, goxel_klass.id, &goxel_klass.def);
    global_obj = JS_GetGlobalObject(g_ctx);
    JS_SetPropertyStr(g_ctx, global_obj, goxel_klass.def.class_name, obj_class);
    JS_FreeValue(g_ctx, global_obj);

    global_obj = JS_GetGlobalObject(g_ctx);
    JS_SetPropertyStr(g_ctx, global_obj, "goxel", obj);
    JS_FreeValue(g_ctx, global_obj);

    JS_FreeValue(g_ctx, obj);
    JS_FreeValue(g_ctx, obj_class);

    /* add vec object */
    obj_class = JS_NewClass(g_rt, vec_klass.id, &vec_klass.def);
    JS_SetClassProto(g_ctx, vec_klass.id,
                     JS_NewCFunction(g_ctx, vec_klass.ctor, vec_klass.def.class_name, 0));
    JS_FreeValue(g_ctx, obj_class);

    /* add box object */
    obj_class = JS_NewClass(g_rt, box_klass.id, &box_klass.def);
    JS_FreeValue(g_ctx, obj_class);

    /* add image object */
    obj_class = JS_NewClass(g_rt, image_klass.id, &image_klass.def);
    JS_FreeValue(g_ctx, obj_class);

    /* add layer object */
    obj_class = JS_NewClass(g_rt, layer_klass.id, &layer_klass.def);
    JS_FreeValue(g_ctx, obj_class);

    /* add volume object */
    obj_class = JS_NewClass(g_rt, volume_klass.id, &volume_klass.def);
    JS_SetClassProto(g_ctx, volume_klass.id,
                     JS_NewCFunction(g_ctx, volume_klass.ctor, volume_klass.def.class_name, 0));
    JS_FreeValue(g_ctx, obj_class);

    assets_list("data/scripts/", NULL, on_script);
    sys_iter_paths(SYS_LOCATION_CONFIG, SYS_DIR, "scripts", NULL, on_dir);
    return 0;
}

int script_execute(const char *name)
{
    int i;
    script_t *script;
    JSValue val;

    for (i = 0; i < arrlen(g_scripts); i++) {
        script = &g_scripts[i];
        if (strcmp(script->name, name) == 0) break;
    }
    if (i == arrlen(g_scripts)) return -1;
    assert(script);
    val = JS_Call(g_ctx, script->execute_fn, JS_UNDEFINED, 0, NULL);
    if (JS_IsException(val)) {
        js_std_dump_error(g_ctx);
    }
    JS_FreeValue(g_ctx, val);
    return 0;
}

int script_close(void)
{
    int i;
    for (i = 0; i < arrlen(g_scripts); i++) {
        JS_FreeValue(g_ctx, g_scripts[i].execute_fn);
    }
    arrfree(g_scripts);
    JS_FreeContext(g_ctx);
    JS_FreeRuntime(g_rt);
    return 0;
}

static JSValue js_file_format_register(JSContext *ctx, JSValueConst this_val,
                                      int argc, JSValueConst *argv)
{
    const char *name;
    JSValue data, import_fn, export_fn;
    file_format_t f = {};

    if (argc != 1) return JS_EXCEPTION;
    data = argv[0];
    name = JS_GetPropertyStr(ctx, data, "name");
    name = JS_ToCString(ctx, name);
    if (!name) return JS_EXCEPTION;
    snprintf(f.name, sizeof(f.name), "%s", name);
    f.import_func = NULL;
    f.export_func = NULL;
    import_fn = JS_GetPropertyStr(ctx, data, "import");
    if (!JS_IsUndefined(import_fn)) {
        f.import_func = js_file_format_import;
        JS_DupValue(ctx, import_fn);
        hmput(g_file_format_import_funcs, f.name, import_fn);
    }
    export_fn = JS_GetPropertyStr(ctx, data, "export");
    if (!JS_IsUndefined(export_fn)) {
        f.export_func = js_file_format_export;
        JS_DupValue(ctx, export_fn);
        hmput(g_file_format_export_funcs, f.name, export_fn);
    }
    file_format_register(&f);
    JS_FreeCString(ctx, name);
    return JS_UNDEFINED;
}
static JSValue js_script_register(JSContext *ctx, JSValueConst this_val,
                                      int argc, JSValueConst *argv)
{
    const char *name, *description;
    JSValue data;
    script_t script = {};

    if (argc != 1) return JS_EXCEPTION;
    data = argv[0];
    name = JS_GetPropertyStr(ctx, data, "name");
    name = JS_ToCString(ctx, name);
    if (!name) return JS_EXCEPTION;
    description = JS_GetPropertyStr(ctx, data, "description");
    description = JS_ToCString(ctx, description);
    snprintf(script.name, sizeof(script.name), "%s", name);
    script.execute_fn = JS_GetPropertyStr(ctx, data, "onExecute");
    arrput(g_scripts, script);
    JS_FreeCString(ctx, name);
    return JS_UNDEFINED;
}
static JSValue js_goxel_version(JSContext *ctx, JSValueConst this_val, int magic)
{
    return JS_NewString(ctx, GOXEL_VERSION);
}
static JSValue js_palette_get_color(JSContext *ctx, JSValueConst this_val, int magic)
{
    uint8_t *color = goxel.painter.color;
    return new_js_vec4(ctx, color[0], color[1], color[2], color[3]);
}

static klass_t palette_klass = {
    .def.class_name = "Palette",
    .attributes = {
        {"color", .get=js_palette_get_color},
        {}
    }
};

static klass_t goxel_klass = {
    .def.class_name = "Goxel",
    .attributes = {
        {"registerFormat", .fn=js_file_format_register},
        {"registerScript", .fn=js_script_register},
        {"image", .klass=&image_klass},
        {"version", .get=js_goxel_version},
        {"palette", .klass=&palette_klass},
        {}
    }
};
