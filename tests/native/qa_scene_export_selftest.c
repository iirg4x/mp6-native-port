/* Direct C-writer fixture for the QA scene export JSON fragment. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "qa_scene_export.h"

HU3D_MODEL *Hu3DData;

void PSMTXCopy(const Mtx source, Mtx destination)
{
    memcpy(destination, source, sizeof(Mtx));
}

void PSMTXConcat(const Mtx a, const Mtx b, Mtx ab)
{
    (void)b;
    PSMTXCopy(a, ab);
}

void PSMTXScale(Mtx matrix, f32 x, f32 y, f32 z)
{
    (void)x;
    (void)y;
    (void)z;
    memset(matrix, 0, sizeof(Mtx));
}

void mtxTransCat(Mtx matrix, float x, float y, float z)
{
    (void)matrix;
    (void)x;
    (void)y;
    (void)z;
}

void mtxScaleCat(Mtx matrix, float x, float y, float z)
{
    (void)matrix;
    (void)x;
    (void)y;
    (void)z;
}

void mtxRotCat(Mtx matrix, float x, float y, float z)
{
    (void)matrix;
    (void)x;
    (void)y;
    (void)z;
}

void mtxRot(Mtx matrix, float x, float y, float z)
{
    (void)x;
    (void)y;
    (void)z;
    memset(matrix, 0, sizeof(Mtx));
}

int main(void)
{
    HSF_FACE face = {0};
    HuVecF vertices[3] = {
        {0.0f, 0.0f, 0.0f},
        {1.0f, 0.0f, 0.0f},
        {0.0f, 0.0f, 1.0f},
    };
    HSF_BUFFER faceBuffer = {0};
    HSF_BUFFER vertexBuffer = {0};
    HSF_DATA hsf = {0};
    HSF_OBJECT object = {0};
    MP6QASceneExportState state = {0};
    HSF_OBJECT objects[2] = {0};
    HSF_DATA graph = {0};
    HU3D_MODEL model = {0};
    Mtx identity = {
        {1.0f, 0.0f, 0.0f, 0.0f},
        {0.0f, 1.0f, 0.0f, 0.0f},
        {0.0f, 0.0f, 1.0f, 0.0f},
    };
    int instance = 0;
    int modelFirst = 1;

    Hu3DData = &model;
    model.hsf = (HSF_DATA *)(uintptr_t)-1;
    state.file = tmpfile();
    model.attr = HU3D_ATTR_HOOKFUNC;
    mp6_qa_scene_export_model(state.file, 0, &modelFirst);
    model.attr = HU3D_ATTR_DISPOFF | HU3D_ATTR_MOTION_MODEL;
    mp6_qa_scene_export_model(state.file, 0, &modelFirst);
    model.attr = HU3D_ATTR_CAMERA;
    mp6_qa_scene_export_model(state.file, 0, &modelFirst);
    assert(modelFirst == 1 && ftell(state.file) == 0);
    fclose(state.file);
    memset(&model, 0, sizeof(model));

    face.typeSrc = HSF_FACE_TRI;
    face.mat = 0;
    face.index[0].vertex = 0;
    face.index[1].vertex = 1;
    face.index[2].vertex = 2;
    faceBuffer.count = 1;
    faceBuffer.data = &face;
    vertexBuffer.count = 3;
    vertexBuffer.data = vertices;
    hsf.cenvNum = 0;
    object.type = HSF_OBJ_MESH;
    object.mesh.face = &faceBuffer;
    object.mesh.vertex = &vertexBuffer;
    state.file = stdout;
    state.hsf = &hsf;

    graph.object = objects;
    graph.objectNum = 2;
    state.model = &model;
    state.hsf = &graph;
    objects[1].constData = (void *)(uintptr_t)-1;
    assert(!strcmp(mp6_qa_scene_unsupported(&state, &objects[1]), "stale_const_data"));
    assert(mp6_qa_scene_object_pointer_valid(&state, &objects[0]));
    assert(mp6_qa_scene_object_pointer_valid(&state, &objects[1]));
    assert(!mp6_qa_scene_object_pointer_valid(&state,
                                              (HSF_OBJECT *)(uintptr_t)-1));
    objects[0].type = HSF_OBJ_ROOT;
    objects[0].mesh.childNum = 1;
    objects[0].mesh.child = (HSF_OBJECT **)(uintptr_t)-1;
    state.file = tmpfile();
    mp6_qa_scene_walk(&state, &objects[0], identity, 1, 0, &instance);
    assert(instance == 0);
    fclose(state.file);

    state.file = stdout;
    state.hsf = &hsf;

    mp6_qa_scene_json_faces(&state, &object);
    fputc('\n', stdout);
    return ferror(stdout) ? 1 : 0;
}
