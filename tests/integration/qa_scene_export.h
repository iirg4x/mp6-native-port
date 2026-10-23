/*
 * QA-only live HSF scene exporter.
 *
 * This header is included only by the generated widescreen-QA camera probe.
 * It is deliberately not part of the production build.  The exporter walks
 * the same HSF object tree that hsfdraw.c walks, after the widescreen helpers
 * have applied their live mesh changes.  Object indices, rather than names,
 * identify geometry so duplicate HSF names remain unambiguous.
 */
#ifndef MP6_QA_SCENE_EXPORT_H
#define MP6_QA_SCENE_EXPORT_H

#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "game/hu3d.h"
#include "game/hsfformat.h"
#include "game/object.h"
#include "mp6_boot.h"
#include "mp6_events.h"

#define MP6_QA_SCENE_MAX_MODELS 128
#define MP6_QA_SCENE_MAX_OBJECTS 512
#define MP6_QA_SCENE_MAX_MESHES 512
#define MP6_QA_SCENE_MAX_VERTICES 65536
#define MP6_QA_SCENE_MAX_FACES 65536
#define MP6_QA_SCENE_MAX_FACE_CORNERS 262144
#define MP6_QA_SCENE_MAX_DEPTH 256

typedef struct MP6QASceneExportState_s {
    FILE *file;
    HU3D_MODEL *model;
    HSF_DATA *hsf;
    HU3D_MODELID modelId;
    Mtx modelMatrix;
    unsigned char seen[MP6_QA_SCENE_MAX_OBJECTS];
    int objectFirst;
    int cameraFirst;
    int meshCount;
    int vertexCount;
    int faceCount;
    int cornerCount;
    int warnedLimit;
    int modelUnsupported;
} MP6QASceneExportState;

static int mp6_qa_scene_export_done;
static unsigned long mp6_qa_scene_export_seq;
static long mp6_qa_scene_export_start = -1;

static void mp6_qa_scene_json_string(FILE *file, const char *text)
{
    const unsigned char *p = (const unsigned char *)(text ? text : "");
    fputc('"', file);
    while (*p) {
        switch (*p) {
        case '"': fputs("\\\"", file); break;
        case '\\': fputs("\\\\", file); break;
        case '\b': fputs("\\b", file); break;
        case '\f': fputs("\\f", file); break;
        case '\n': fputs("\\n", file); break;
        case '\r': fputs("\\r", file); break;
        case '\t': fputs("\\t", file); break;
        default:
            if (*p < 0x20) fprintf(file, "\\u%04x", (unsigned)*p);
            else fputc(*p, file);
            break;
        }
        ++p;
    }
    fputc('"', file);
}

static void mp6_qa_scene_json_float(FILE *file, float value)
{
    if (!isfinite(value)) fputs("null", file);
    else fprintf(file, "%.9g", (double)value);
}

static void mp6_qa_scene_json_vec3(FILE *file, const HuVecF *value)
{
    fputc('[', file);
    mp6_qa_scene_json_float(file, value->x); fputc(',', file);
    mp6_qa_scene_json_float(file, value->y); fputc(',', file);
    mp6_qa_scene_json_float(file, value->z);
    fputc(']', file);
}

static void mp6_qa_scene_json_matrix(FILE *file, Mtx matrix)
{
    int row;
    fputc('[', file);
    for (row = 0; row < 3; ++row) {
        if (row) fputc(',', file);
        fputc('[', file);
        mp6_qa_scene_json_float(file, matrix[row][0]); fputc(',', file);
        mp6_qa_scene_json_float(file, matrix[row][1]); fputc(',', file);
        mp6_qa_scene_json_float(file, matrix[row][2]); fputc(',', file);
        mp6_qa_scene_json_float(file, matrix[row][3]);
        fputc(']', file);
    }
    fputc(']', file);
}

static const char *mp6_qa_scene_object_type(u32 type)
{
    switch (type) {
    case HSF_OBJ_ROOT: return "root";
    case HSF_OBJ_MESH: return "mesh";
    case HSF_OBJ_JOINT: return "joint";
    case HSF_OBJ_NULL1: return "null1";
    case HSF_OBJ_NULL2: return "null2";
    case HSF_OBJ_NULL3: return "null3";
    case HSF_OBJ_REPLICA: return "replica";
    case HSF_OBJ_MAP: return "map";
    case HSF_OBJ_CAMERA: return "camera";
    case HSF_OBJ_LIGHT: return "light";
    default: return "unknown";
    }
}

static const char *mp6_qa_scene_face_type(u16 type)
{
    switch (type & HSF_FACE_MASK) {
    case HSF_FACE_TRI: return "tri";
    case HSF_FACE_QUAD: return "quad";
    case HSF_FACE_TRISTRIP: return "strip";
    default: return "unsupported";
    }
}

static int mp6_qa_scene_face_storage_count(const HSF_FACE *face)
{
    switch (face->typeSrc & HSF_FACE_MASK) {
    case HSF_FACE_TRI: return 3;
    case HSF_FACE_QUAD: return 4;
    case HSF_FACE_TRISTRIP:
        if (face->strip.count > MP6_QA_SCENE_MAX_FACE_CORNERS - 3) return -1;
        return 3 + (int)face->strip.count;
    default: return 0;
    }
}

/* FaceDraw emits TRI/QUAD as 0,2,1 / 0,2,3,1.  A strip starts with the same
 * three emitted corners, then appends its strip data in authored order. */
static HSF_FACE_INDEX mp6_qa_scene_face_storage_index(const HSF_FACE *face, int index)
{
    if ((face->typeSrc & HSF_FACE_MASK) == HSF_FACE_TRISTRIP && index >= 3)
        return face->strip.data[index - 3];
    return face->index[index];
}

static HSF_FACE_INDEX mp6_qa_scene_face_emission_index(const HSF_FACE *face, int index)
{
    u16 type = face->typeSrc & HSF_FACE_MASK;
    if (type == HSF_FACE_TRI || type == HSF_FACE_TRISTRIP) {
        if (index == 0) return face->index[0];
        if (index == 1) return face->index[2];
        if (index == 2) return face->index[1];
        return face->strip.data[index - 3];
    }
    if (type == HSF_FACE_QUAD) {
        static const int order[4] = { 0, 2, 3, 1 };
        return face->index[order[index]];
    }
    return (HSF_FACE_INDEX){ -1, -1, -1, -1 };
}

static int mp6_qa_scene_valid_index(s16 value, s32 count)
{
    return value >= 0 && value < count;
}

static void mp6_qa_scene_json_index(FILE *file, s16 value)
{
    if (value < 0) fputs("null", file);
    else fprintf(file, "%d", (int)value);
}

static void mp6_qa_scene_json_uv(FILE *file, HSF_BUFFER *buffer, s16 index)
{
    if (!buffer || !buffer->data || !mp6_qa_scene_valid_index(index, buffer->count)) {
        fputs("null", file);
        return;
    }
    {
        const HuVec2f *uv = &((const HuVec2f *)buffer->data)[index];
        fputc('[', file);
        mp6_qa_scene_json_float(file, uv->x); fputc(',', file);
        mp6_qa_scene_json_float(file, uv->y); fputc(']', file);
    }
}

static void mp6_qa_scene_json_color(FILE *file, HSF_BUFFER *buffer, s16 index)
{
    if (!buffer || !buffer->data || !mp6_qa_scene_valid_index(index, buffer->count)) {
        fputs("null", file);
        return;
    }
    {
        const GXColor *color = &((const GXColor *)buffer->data)[index];
        fprintf(file, "[%u,%u,%u,%u]", color->r, color->g, color->b, color->a);
    }
}

static void mp6_qa_scene_json_normal(FILE *file, HSF_DATA *hsf, HSF_BUFFER *buffer, s16 index)
{
    HuVecF normal;
    if (!buffer || !buffer->data || !mp6_qa_scene_valid_index(index, buffer->count)) {
        fputs("null", file);
        return;
    }
    if (hsf->cenvNum == 0) {
        const HSF_S8VEC *packed = &((const HSF_S8VEC *)buffer->data)[index];
        normal.x = packed->x / 64.0f;
        normal.y = packed->y / 64.0f;
        normal.z = packed->z / 64.0f;
    } else {
        normal = ((const HuVecF *)buffer->data)[index];
    }
    mp6_qa_scene_json_vec3(file, &normal);
}

static const char *mp6_qa_scene_validate_faces(HSF_BUFFER *faceBuffer,
                                                int vertexCount, int *cornerCount)
{
    HSF_FACE *faces = (HSF_FACE *)faceBuffer->data;
    *cornerCount = 0;
    for (int i = 0; i < faceBuffer->count; ++i) {
        HSF_FACE *face = &faces[i];
        u16 type = face->typeSrc & HSF_FACE_MASK;
        int storageCount = mp6_qa_scene_face_storage_count(face);
        if (storageCount < 3 ||
            (type != HSF_FACE_TRI && type != HSF_FACE_QUAD && type != HSF_FACE_TRISTRIP))
            return "unsupported_face_primitive";
        if (type == HSF_FACE_TRISTRIP && face->strip.count != 0 && !face->strip.data)
            return "missing_strip_data";
        if (*cornerCount > MP6_QA_SCENE_MAX_FACE_CORNERS - storageCount)
            return "scene_export_face_corner_limit";
        for (int j = 0; j < storageCount; ++j) {
            HSF_FACE_INDEX id = mp6_qa_scene_face_storage_index(face, j);
            if (!mp6_qa_scene_valid_index(id.vertex, vertexCount))
                return "invalid_face_vertex_index";
        }
        *cornerCount += storageCount;
    }
    return NULL;
}

static void mp6_qa_scene_json_materials(FILE *file, HSF_DATA *hsf)
{
    int i;
    fputs("[", file);
    if (!hsf->material) {
        fputs("]", file);
        return;
    }
    for (i = 0; i < hsf->materialNum; ++i) {
        HSF_MATERIAL *material = &hsf->material[i];
        if (i) fputc(',', file);
        fputs("{\"index\":", file); fprintf(file, "%d", i);
        fputs(",\"name\":", file); mp6_qa_scene_json_string(file, material->name);
        fprintf(file, ",\"rgba\":[%u,%u,%u,255]", material->color[0],
                material->color[1], material->color[2]);
        fputs(",\"lit_rgb\":[", file);
        fprintf(file, "%u,%u,%u", material->litColor[0], material->litColor[1], material->litColor[2]);
        fputs("]", file);
        fputs(",\"inv_alpha\":", file); mp6_qa_scene_json_float(file, material->invAlpha);
        fputs(",\"ref_alpha\":", file); mp6_qa_scene_json_float(file, material->refAlpha);
        fprintf(file, ",\"pass\":%u,\"vertex_mode\":%u,\"flags\":%u,\"attributes\":[",
                material->pass, material->vtxMode, material->flags);
        int attributeCount = material->attrNum > 64 ? 64 : (int)material->attrNum;
        for (int j = 0; j < attributeCount; ++j) {
            if (j) fputc(',', file);
            fprintf(file, "%d", material->attr ? material->attr[j] : -1);
        }
        fputs("],\"attribute_records\":[", file);
        for (int j = 0; j < attributeCount; ++j) {
            int attrIndex = material->attr ? material->attr[j] : -1;
            HSF_ATTRIBUTE *attribute = hsf->attribute && attrIndex >= 0 &&
                                       attrIndex < hsf->attributeNum
                ? &hsf->attribute[attrIndex] : NULL;
            if (j) fputc(',', file);
            fputs("{\"index\":", file); fprintf(file, "%d", attrIndex);
            if (attribute) {
                fputs(",\"name\":", file); mp6_qa_scene_json_string(file, attribute->name);
                fprintf(file, ",\"wrap_s\":%u,\"wrap_t\":%u,\"bitmap\":",
                        attribute->wrapS, attribute->wrapT);
                if (attribute->bitmap) {
                    const char *waterDump = getenv("MP6_WS_QA_WATER_BITMAP");
                    HSF_BITMAP *bitmap = attribute->bitmap;
                    if (waterDump && bitmap->name && strcmp(bitmap->name, "hama_umi") == 0 &&
                        bitmap->dataFmt == 5 && bitmap->sizeX == 256 && bitmap->sizeY == 256) {
                        FILE *raw = fopen(waterDump, "wb");
                        if (raw) { fwrite(bitmap->data, 2, 256 * 256, raw); fclose(raw); }
                    }
                    fputs("{\"name\":", file);
                    mp6_qa_scene_json_string(file, attribute->bitmap->name);
                    fprintf(file, ",\"size\":[%d,%d],\"format\":%u,\"pixel_size\":%u}",
                            attribute->bitmap->sizeX, attribute->bitmap->sizeY,
                            attribute->bitmap->dataFmt, attribute->bitmap->pixSize);
                } else {
                    fputs("null", file);
                }
            }
            fputc('}', file);
        }
        fputs("]}", file);
    }
    fputc(']', file);
}

static void mp6_qa_scene_json_faces(MP6QASceneExportState *state, HSF_OBJECT *object)
{
    HSF_BUFFER *faceBuffer = object->mesh.face;
    HSF_BUFFER *vertexBuffer = object->mesh.vertex;
    HSF_BUFFER *stBuffer = object->mesh.st;
    HSF_BUFFER *normalBuffer = object->mesh.normal;
    HSF_BUFFER *colorBuffer = object->mesh.color;
    HSF_FACE *faces = (HSF_FACE *)faceBuffer->data;
    HuVecF *vertices = (HuVecF *)vertexBuffer->data;
    FILE *file = state->file;
    int i;

    fputs(",\"vertices\":[", file);
    for (i = 0; i < vertexBuffer->count; ++i) {
        if (i) fputc(',', file);
        mp6_qa_scene_json_vec3(file, &vertices[i]);
    }
    fputs("],\"faces\":[", file);
    for (i = 0; i < faceBuffer->count; ++i) {
        HSF_FACE *face = &faces[i];
        int storageCount = mp6_qa_scene_face_storage_count(face);
        int type = face->typeSrc & HSF_FACE_MASK;
        if (i) fputc(',', file);
        fputs("{\"index\":", file); fprintf(file, "%d", i);
        fputs(",\"primitive\":", file); mp6_qa_scene_json_string(file, mp6_qa_scene_face_type(face->typeSrc));
        fputs(",\"material\":", file); fprintf(file, "%d", face->mat & 0xFFF);
        fputs(",\"storage\":[", file);
        for (int j = 0; j < storageCount; ++j) {
            HSF_FACE_INDEX id = mp6_qa_scene_face_storage_index(face, j);
            if (j) fputc(',', file);
            fputs("{\"vertex\":", file); mp6_qa_scene_json_index(file, id.vertex);
            fputs(",\"normal\":", file); mp6_qa_scene_json_index(file, id.normal);
            fputs(",\"st\":", file); mp6_qa_scene_json_index(file, id.st);
            fputs(",\"color\":", file); mp6_qa_scene_json_index(file, id.color);
            fputs("}", file);
        }
        fputs("],\"emission\":[", file);
        for (int j = 0; j < storageCount; ++j) {
            if (j) fputc(',', file);
            if (type == HSF_FACE_QUAD) {
                static const int order[4] = { 0, 2, 3, 1 };
                fprintf(file, "%d", order[j]);
            } else if (type == HSF_FACE_TRI || type == HSF_FACE_TRISTRIP) {
                fprintf(file, "%d", j < 3 ? (j == 0 ? 0 : (j == 1 ? 2 : 1)) : j);
            }
        }
        fputs("],\"loops\":[", file);
        for (int j = 0; j < storageCount; ++j) {
            HSF_FACE_INDEX id = mp6_qa_scene_face_emission_index(face, j);
            if (j) fputc(',', file);
            fputs("{\"vertex\":", file); mp6_qa_scene_json_index(file, id.vertex);
            fputs(",\"normal\":", file); mp6_qa_scene_json_index(file, id.normal);
            fputs(",\"normal_value\":", file);
            mp6_qa_scene_json_normal(file, state->hsf, normalBuffer, id.normal);
            fputs(",\"uv\":", file); mp6_qa_scene_json_uv(file, stBuffer, id.st);
            fputs(",\"rgba\":", file); mp6_qa_scene_json_color(file, colorBuffer, id.color);
            fputs("}", file);
        }
        fputs("]}", file);
    }
    fputc(']', file);
}

static void mp6_qa_scene_object_matrix(Mtx out, Mtx parent, const HSF_TRANSFORM *transform)
{
    Mtx local;
    PSMTXScale(local, transform->scale.x, transform->scale.y, transform->scale.z);
    mtxRotCat(local, transform->rot.x, transform->rot.y, transform->rot.z);
    mtxTransCat(local, transform->pos.x, transform->pos.y, transform->pos.z);
    PSMTXConcat(parent, local, out);
}

/* objReplica uses rotation*scale (rather than the normal scale-then-rotate
 * object path) before concatenating its authored translation.  Keep this
 * separate so a replica's live transform remains identical to hsfdraw.c. */
static void mp6_qa_scene_replica_matrix(Mtx out, Mtx parent, const HSF_TRANSFORM *transform)
{
    Mtx local;
    Mtx rotation;
    mtxRot(rotation, transform->rot.x, transform->rot.y, transform->rot.z);
    PSMTXScale(local, transform->scale.x, transform->scale.y, transform->scale.z);
    PSMTXConcat(rotation, local, local);
    mtxTransCat(local, transform->pos.x, transform->pos.y, transform->pos.z);
    PSMTXConcat(parent, local, out);
}

static int mp6_qa_scene_object_index(HSF_DATA *hsf, HSF_OBJECT *object)
{
    ptrdiff_t index;
    if (!hsf || !hsf->object || !object) return -1;
    index = object - hsf->object;
    if (index < 0 || index >= hsf->objectNum) return -1;
    return (int)index;
}

/* HSF child/replica links are normally pointers into hsf->object.  A model
 * that has just been disposed can remain in Hu3DData with DISPOFF while its
 * stale root/child links contain the native -1 sentinel.  Validate the link
 * before dereferencing it; this keeps export observation from changing the
 * live renderer's teardown state. */
static int mp6_qa_scene_object_pointer_valid(MP6QASceneExportState *state,
                                             HSF_OBJECT *object)
{
    uintptr_t first;
    uintptr_t value;
    uintptr_t bytes;
    if (!state || !state->hsf || !state->hsf->object || !object ||
        state->hsf->objectNum <= 0) return 0;
    first = (uintptr_t)state->hsf->object;
    value = (uintptr_t)object;
    bytes = (uintptr_t)state->hsf->objectNum * sizeof(HSF_OBJECT);
    if (first == 0 || value < first || bytes > UINTPTR_MAX - first ||
        value - first >= bytes) return 0;
    return ((value - first) % sizeof(HSF_OBJECT)) == 0;
}

static int mp6_qa_scene_pointer_sane(const void *pointer)
{
    uintptr_t value = (uintptr_t)pointer;
    return value != 0 && value != UINTPTR_MAX && value >= 0x10000;
}

static int mp6_qa_scene_is_hidden(MP6QASceneExportState *state, HSF_OBJECT *object)
{
    HSF_CONSTDATA *constant = (HSF_CONSTDATA *)object->constData;
    return (state->model->attr & HU3D_ATTR_DISPOFF) != 0 ||
           (object->flags & HSF_MATERIAL_DISPOFF) != 0 ||
           (mp6_qa_scene_pointer_sane(constant) && (constant->attr & HU3D_CONST_DISPOFF) != 0);
}

static const char *mp6_qa_scene_unsupported(MP6QASceneExportState *state,
                                            HSF_OBJECT *object)
{
    HSF_CONSTDATA *constant = (HSF_CONSTDATA *)object->constData;
    if (constant && !mp6_qa_scene_pointer_sane(constant))
        return "stale_const_data";
    if (state->modelUnsupported)
        return "model_hook_transform";
    if (state->hsf->cenvNum != 0 || object->mesh.cenvNum != 0)
        return "skinned_cenv_mesh";
    if (object->mesh.clusterNum != 0)
        return "cluster_deformed_mesh";
    if (object->mesh.shapeNum != 0 && !getenv("MP6_WS_QA_EVALUATED_SHAPES"))
        return "shape_deformed_mesh";
    if (constant && constant->hook)
        return "object_hook_transform";
    return NULL;
}

static void mp6_qa_scene_write_object(MP6QASceneExportState *state,
                                      HSF_OBJECT *object, Mtx matrix, int instance,
                                      const char *unsupported)
{
    HSF_BUFFER *vertices = object->mesh.vertex;
    HSF_BUFFER *faces = object->mesh.face;
    FILE *file = state->file;
    int objectIndex = mp6_qa_scene_object_index(state->hsf, object);
    const char *reason = unsupported;
    const char *faceReason;
    int supported;
    int cornerCount = 0;
    int faceCount = faces && faces->data ? faces->count : 0;
    int vertexCount = vertices && vertices->data ? vertices->count : 0;

    if (!reason && (!vertices || !vertices->data || !faces || !faces->data ||
                    vertexCount <= 0 || faceCount < 0 ||
                    vertexCount > MP6_QA_SCENE_MAX_VERTICES ||
                    faceCount > MP6_QA_SCENE_MAX_FACES ||
                    state->vertexCount + vertexCount > MP6_QA_SCENE_MAX_VERTICES ||
                    state->faceCount + faceCount > MP6_QA_SCENE_MAX_FACES))
        reason = "scene_export_limit_or_missing_buffer";
    faceReason = !reason ? mp6_qa_scene_validate_faces(faces, vertexCount, &cornerCount) : NULL;
    if (!reason && faceReason) reason = faceReason;
    if (!reason && state->cornerCount > MP6_QA_SCENE_MAX_FACE_CORNERS - cornerCount)
        reason = "scene_export_face_corner_limit";
    supported = reason == NULL;

    if (!state->objectFirst) fputc(',', file);
    state->objectFirst = 0;
    fputs("{\"object_index\":", file); fprintf(file, "%d", objectIndex);
    fputs(",\"instance\":", file); fprintf(file, "%d", instance);
    fputs(",\"name\":", file); mp6_qa_scene_json_string(file, object->name);
    fputs(",\"type\":", file); mp6_qa_scene_json_string(file, mp6_qa_scene_object_type(object->type));
    fprintf(file, ",\"flags\":%u,\"hidden\":%s,\"transform\":",
            object->flags, mp6_qa_scene_is_hidden(state, object) ? "true" : "false");
    mp6_qa_scene_json_matrix(file, matrix);
    fputs(",\"supported\":", file); fputs(supported ? "true" : "false", file);
    if (supported && object->mesh.shapeNum != 0)
        fputs(",\"evaluated_shape_pose\":true", file);
    if (!supported) {
        fputs(",\"unsupported_reason\":", file);
        mp6_qa_scene_json_string(file, reason);
        if (reason && strcmp(reason, "scene_export_limit_or_missing_buffer") == 0 &&
            !state->warnedLimit) {
            printf("[WIDE-EXPORT] scene export limit reached at model=%d object=%d\n",
                   state->modelId, objectIndex);
            state->warnedLimit = 1;
        }
    }
    if (supported) {
        state->meshCount++;
        state->vertexCount += vertexCount;
        state->faceCount += faceCount;
        state->cornerCount += cornerCount;
        mp6_qa_scene_json_faces(state, object);
    }
    fputc('}', file);
}

static void mp6_qa_scene_walk(MP6QASceneExportState *state, HSF_OBJECT *object,
                              Mtx parent, int cancelTransform, int depth, int *instance)
{
    HSF_TRANSFORM *transform;
    Mtx matrix;
    int objectIndex;
    const char *unsupported;

    if (!object || depth > MP6_QA_SCENE_MAX_DEPTH ||
        !mp6_qa_scene_object_pointer_valid(state, object)) return;
    objectIndex = mp6_qa_scene_object_index(state->hsf, object);
    if (objectIndex >= 0) state->seen[objectIndex] = 1;
    transform = state->model->motId != HU3D_MOTIONID_NONE
        ? &object->mesh.curr : &object->mesh.base;
    if (object->type == HSF_OBJ_REPLICA) {
        mp6_qa_scene_replica_matrix(matrix, parent, transform);
        mp6_qa_scene_walk(state, object->mesh.replica, matrix, 1, depth + 1, instance);
        return;
    }
    if (!cancelTransform && object->type != HSF_OBJ_CAMERA && object->type != HSF_OBJ_LIGHT)
        mp6_qa_scene_object_matrix(matrix, parent, transform);
    else
        PSMTXCopy(parent, matrix);
    if (object->type == HSF_OBJ_MESH) {
        unsupported = mp6_qa_scene_unsupported(state, object);
        mp6_qa_scene_write_object(state, object, matrix, (*instance)++, unsupported);
    }
    /* Camera/light records use the same union as mesh children.  hsfdraw
     * never treats their payload as an HSF_OBJECT child array, so neither
     * should this exporter (otherwise a camera payload can become bogus
     * recursion when a scene stores cameras in the object table). */
    if (object->type != HSF_OBJ_CAMERA && object->type != HSF_OBJ_LIGHT) {
        if (object->mesh.childNum > MP6_QA_SCENE_MAX_OBJECTS ||
            object->mesh.childNum > (u32)state->hsf->objectNum ||
            (object->mesh.childNum != 0 &&
             !mp6_qa_scene_pointer_sane(object->mesh.child))) return;
        for (u32 i = 0; i < object->mesh.childNum; ++i)
            mp6_qa_scene_walk(state, object->mesh.child[i], matrix, 0, depth + 1, instance);
    }
}

static void mp6_qa_scene_write_hsf_cameras(MP6QASceneExportState *state)
{
    FILE *file = state->file;
    for (int i = 0; i < state->hsf->objectNum; ++i) {
        HSF_OBJECT *object = &state->hsf->object[i];
        if (object->type != HSF_OBJ_CAMERA) continue;
        if (!state->cameraFirst) fputc(',', file);
        state->cameraFirst = 0;
        fputs("{\"model_id\":", file); fprintf(file, "%d", state->modelId);
        fputs(",\"object_index\":", file); fprintf(file, "%d", i);
        fputs(",\"name\":", file); mp6_qa_scene_json_string(file, object->name);
        fputs(",\"position\":", file); mp6_qa_scene_json_vec3(file, &object->camera.pos);
        fputs(",\"target\":", file); mp6_qa_scene_json_vec3(file, &object->camera.target);
        fputs(",\"up_rot\":", file); mp6_qa_scene_json_float(file, object->camera.upRot);
        fputs(",\"fov\":", file); mp6_qa_scene_json_float(file, object->camera.fov);
        fputs(",\"near\":", file); mp6_qa_scene_json_float(file, object->camera.near);
        fputs(",\"far\":", file); mp6_qa_scene_json_float(file, object->camera.far);
        fputs("}", file);
    }
}

static void mp6_qa_scene_write_runtime_cameras(FILE *file, int *first)
{
    for (int i = 0; i < HU3D_CAM_MAX; ++i) {
        HU3D_CAMERA *camera = &Hu3DCamera[i];
        if (camera->fov == -1.0f) continue;
        if (!*first) fputc(',', file);
        *first = 0;
        fprintf(file, "{\"camera_id\":%d,\"fov\":", i);
        mp6_qa_scene_json_float(file, camera->fov);
        fputs(",\"near\":", file); mp6_qa_scene_json_float(file, camera->near);
        fputs(",\"far\":", file); mp6_qa_scene_json_float(file, camera->far);
        fputs(",\"aspect\":", file); mp6_qa_scene_json_float(file, camera->aspect);
        fputs(",\"position\":[", file);
        mp6_qa_scene_json_float(file, camera->pos.x); fputc(',', file);
        mp6_qa_scene_json_float(file, camera->pos.y); fputc(',', file);
        mp6_qa_scene_json_float(file, camera->pos.z); fputs("],\"target\":[", file);
        mp6_qa_scene_json_float(file, camera->target.x); fputc(',', file);
        mp6_qa_scene_json_float(file, camera->target.y); fputc(',', file);
        mp6_qa_scene_json_float(file, camera->target.z); fputs("],\"up\":[", file);
        mp6_qa_scene_json_float(file, camera->up.x); fputc(',', file);
        mp6_qa_scene_json_float(file, camera->up.y); fputc(',', file);
        mp6_qa_scene_json_float(file, camera->up.z); fputs("],\"viewport\":[", file);
        mp6_qa_scene_json_float(file, camera->viewportX); fputc(',', file);
        mp6_qa_scene_json_float(file, camera->viewportY); fputc(',', file);
        mp6_qa_scene_json_float(file, camera->viewportW); fputc(',', file);
        mp6_qa_scene_json_float(file, camera->viewportH); fputs("],\"scissor\":[", file);
        fprintf(file, "%d,%d,%d,%d]}", camera->scissorX, camera->scissorY,
                camera->scissorW, camera->scissorH);
    }
}

static void mp6_qa_scene_export_model(FILE *file, HU3D_MODELID modelId,
                                      int *modelFirst)
{
    MP6QASceneExportState state;
    HU3D_MODEL *model = &Hu3DData[modelId];
    HSF_DATA *hsf = model->hsf;
    Mtx modelMatrix;
    Mtx temp;
    int instance = 0;

    /* Export the currently drawable scene. Hook slots store a callback in the
     * hsf union, while hidden motion-source slots can retain unrelocated data.
     * MOTION_MODEL alone is not an exclusion: animated scenery uses it too. */
    if (model->attr & (HU3D_ATTR_DISPOFF | HU3D_ATTR_HOOKFUNC | HU3D_ATTR_CAMERA)) return;
    if (!hsf || !hsf->object || hsf->objectNum <= 0 || hsf->objectNum > MP6_QA_SCENE_MAX_OBJECTS)
        return;
    memset(&state, 0, sizeof(state));
    state.file = file;
    state.model = model;
    state.hsf = hsf;
    state.modelId = modelId;
    state.modelUnsupported = (model->attr & HU3D_ATTR_HOOK) != 0;
    for (int i = 0; i < hsf->objectNum; ++i) state.seen[i] = 0;
    mtxRot(temp, model->rot.x, model->rot.y, model->rot.z);
    mtxScaleCat(temp, model->scale.x, model->scale.y, model->scale.z);
    mtxTransCat(temp, model->pos.x, model->pos.y, model->pos.z);
    PSMTXConcat(temp, model->mtx, modelMatrix);
    PSMTXCopy(modelMatrix, state.modelMatrix);

    if (!*modelFirst) fputc(',', file);
    *modelFirst = 0;
    fprintf(file, "{\"model_id\":%d,\"visible\":%s,\"attrs\":%u,\"camera_bits\":%u,\"motion_id\":%d,\"cenv_count\":%d,\"model_matrix\":",
            modelId, (model->attr & HU3D_ATTR_DISPOFF) ? "false" : "true",
            model->attr, model->cameraBit, model->motId, hsf->cenvNum);
    mp6_qa_scene_json_matrix(file, modelMatrix);
    fputs(",\"supported\":", file);
    fputs(state.modelUnsupported ? "false" : "true", file);
    if (state.modelUnsupported)
        fputs(",\"unsupported_reason\":\"model_hook_transform\"", file);
    fputs(",\"materials\":", file); mp6_qa_scene_json_materials(file, hsf);
    fputs(",\"objects\":[", file);
    state.objectFirst = 1;
    if (hsf->root) mp6_qa_scene_walk(&state, hsf->root, modelMatrix, 0, 0, &instance);
    for (int i = 0; i < hsf->objectNum; ++i) {
        HSF_OBJECT *object = &hsf->object[i];
        if (!state.seen[i] && object->type == HSF_OBJ_MESH)
            mp6_qa_scene_walk(&state, object, modelMatrix, 0, 0, &instance);
    }
    fputs("],\"hsf_cameras\":[", file);
    state.cameraFirst = 1;
    mp6_qa_scene_write_hsf_cameras(&state);
    fputs("],\"mesh_count\":", file); fprintf(file, "%d", state.meshCount);
    fputs(",\"vertex_count\":", file); fprintf(file, "%d", state.vertexCount);
    fputs(",\"face_count\":", file); fprintf(file, "%d", state.faceCount);
    fputs(",\"corner_count\":", file); fprintf(file, "%d", state.cornerCount);
    fputs("}", file);
}

static void mp6_qa_scene_export_tick(void)
{
    const char *path = getenv("MP6_WS_QA_EXPORT_SCENE");
    const char *target = getenv("MP6_WS_QA_SCENE");
    unsigned long sequence;
    FILE *file;
    int modelFirst = 1;
    int cameraFirst = 1;
    if (mp6_qa_scene_export_done || !path || !*path || !Hu3DData) return;
    sequence = mp6_event_last_seq("ovl.start");
    if (target && sequence != 0 && sequence != mp6_qa_scene_export_seq &&
        mp6_event_matches("ovl.start", target)) {
        mp6_qa_scene_export_seq = sequence;
        mp6_qa_scene_export_start = mp6_tick_count;
    }
    if (mp6_qa_scene_export_start < 0 || mp6_tick_count - mp6_qa_scene_export_start < 500)
        return;
    file = fopen(path, "wb");
    if (!file) {
        printf("[WIDE-EXPORT] unable to open %s\n", path);
        mp6_qa_scene_export_done = 1;
        return;
    }
    fputs("{\"schema\":\"mp6-native-scene-v1\",\"coordinate_system\":\"native_hsf_y_up\",\"tick\":", file);
    fprintf(file, "%ld,\"overlay_id\":%d,\"overlay\":", mp6_tick_count, omcurovl);
    mp6_qa_scene_json_string(file, mp6_event_ovl_name(omcurovl));
    fputs(",\"limitations\":[\"textures are represented by native material/UV metadata; bitmap pixels are not copied\",\"animation is a live sample; no animation timeline is exported\",\"skinned, clustered, shape-deformed, or hook-transformed meshes are reported but omitted\"],\"limits\":{\"models\":", file);
    fprintf(file, "%d,\"objects\":%d,\"vertices\":%d,\"faces\":%d,\"face_corners\":%d},\"models\":[",
            MP6_QA_SCENE_MAX_MODELS, MP6_QA_SCENE_MAX_OBJECTS,
            MP6_QA_SCENE_MAX_VERTICES, MP6_QA_SCENE_MAX_FACES,
            MP6_QA_SCENE_MAX_FACE_CORNERS);
    for (int model = 0; model < HU3D_MODEL_MAX && model < MP6_QA_SCENE_MAX_MODELS; ++model) {
        if (!Hu3DData[model].hsf) continue;
        mp6_qa_scene_export_model(file, (HU3D_MODELID)model, &modelFirst);
    }
    fputs("],\"cameras\":[", file);
    mp6_qa_scene_write_runtime_cameras(file, &cameraFirst);
    fputs("]}\n", file);
    fclose(file);
    mp6_qa_scene_export_done = 1;
    printf("[WIDE-EXPORT] wrote %s tick=%ld\n", path, mp6_tick_count);
}

#endif /* MP6_QA_SCENE_EXPORT_H */
