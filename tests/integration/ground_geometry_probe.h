/* QA only: capture the actual submitted, skinned world-space mesh geometry.
 * One or two requested ticks, camera zero, no shadow pass; never changes the
 * scene. The second tick is an opt-in frozen-fixture proof aid. */
#include "game/board/player.h"
#include "game/board/masu.h"
#include "game/board/object.h"
#include "mp6_grounding.h"
#include <math.h>

/* Capture the same live normal array and inverse-transpose used by ObjDraw.
 * Direct NBT display lists have a separate normal source: flag those records
 * rather than silently presenting their indexed normals as the final input. */
static void qa_ground_normals(FILE *out, HU3D_DRAW_OBJ *draw)
{
    HSF_OBJECT *obj = draw->object;
    HSF_DATA *hsf = draw->model->hsf;
    HSF_BUFFER *buffer = obj->mesh.normal;
    int available = buffer && buffer->data && buffer->count > 0;
    int packed = hsf->cenvNum == 0;
    Mtx matrix;
    int matrixValid = PSMTXInvXpose(draw->matrix, matrix) != 0;
    if (matrixValid) {
        for (int row=0; row<3; row++) for (int col=0; col<3; col++)
            if (!isfinite(matrix[row][col])) matrixValid=0;
    }
    fprintf(out,",\"normalFormat\":%s,\"normalsSource\":\"obj.mesh.normal->data:current-draw-array\","
        "\"normalMatrixSource\":\"PSMTXInvXpose(draw->matrix)\",\"normalMatrix3x3\":",
        available ? (packed ? "\"s8\"" : "\"f32\"") : "null");
    if (!matrixValid) fprintf(out,"null");
    else {
        fprintf(out,"[");
        for (int row=0; row<3; row++) for (int col=0; col<3; col++)
            fprintf(out,"%s%.9g",row||col?",":"",matrix[row][col]);
        fprintf(out,"]");
    }
    int final = available && matrixValid;
    fprintf(out,",\"viewNormals\":");
    if (!available || !matrixValid) fprintf(out,"null");
    else {
        fprintf(out,"[");
        for (int i=0; i<buffer->count; i++) {
            HuVecF local, view;
            if (packed) {
                const HSF_S8VEC *source=(const HSF_S8VEC *)buffer->data+i;
                local=(HuVecF){source->x,source->y,source->z};
            } else local=((const HuVecF *)buffer->data)[i];
            PSMTXMultVecSR(matrix,&local,&view);
            double length=sqrt((double)view.x*view.x+(double)view.y*view.y+(double)view.z*view.z);
            if (i) fprintf(out,",");
            if (!isfinite(length) || length<=1e-10) {
                fprintf(out,"null"); final=0;
            } else fprintf(out,"[%.9g,%.9g,%.9g]",view.x/length,view.y/length,view.z/length);
        }
        fprintf(out,"]");
    }
    fprintf(out,",\"faceNormalIndicesOrder\":\"source-corner-order\",\"faceNormalIndices\":[");
    HSF_FACE *faces=obj->mesh.face->data;
    for (int i=0; i<obj->mesh.face->count; i++) {
        HSF_FACE *face=faces+i;
        int type=face->typeSrc & HSF_FACE_MASK;
        fprintf(out,"%s[",i?",":"");
        for (int j=0; j<(type==HSF_FACE_QUAD?4:3); j++)
            fprintf(out,"%s%d",j?",":"",face->index[j].normal);
        if (type==HSF_FACE_TRISTRIP)
            for (unsigned j=0; j<face->strip.count; j++) fprintf(out,",%d",face->strip.data[j].normal);
        fprintf(out,"]");
        int material=face->mat & 0xFFF;
        if (!obj->mesh.material || material>=hsf->materialNum) { final=0; continue; }
        HSF_MATERIAL *m=obj->mesh.material+material;
        if (m->attrNum && (!m->attr || !obj->mesh.attribute)) { final=0; continue; }
        for (unsigned j=0; j<m->attrNum; j++) {
            int attribute=m->attr[j];
            if (attribute<0 || attribute>=hsf->attributeNum) { final=0; continue; }
            if (obj->mesh.attribute[attribute].nbtTpLvl!=0.0f) final=0;
        }
    }
    fprintf(out,"],\"indexedNormalsAreFinal\":%s",final?"true":"false");
}

static void qa_ground_geometry(HU3D_DRAW_OBJ *draw)
{
    const char *target = getenv("MP6_QA_GEOMETRY_AT");
    const char *second = getenv("MP6_QA_GEOMETRY_AT_SECOND");
    long targetTick = target ? atol(target) : -1;
    long secondTick = second ? atol(second) : -1;
    HSF_OBJECT *obj = draw->object;
    if (!target || (mp6_tick_count != targetTick &&
        (!second || mp6_tick_count != secondTick)) || Hu3DCameraNo != 0 ||
        shadowModelDrawF || !obj || !obj->mesh.vertex || !obj->mesh.face) return;
    static long spaceTick=-1;
    if (spaceTick!=mp6_tick_count) {
        spaceTick=mp6_tick_count;
        /* Record the installed, widescreen-adjusted camera alongside the
         * corrected draw vertices. Never infer it from the output aspect. */
        FILE *camera=fopen("geometry-camera.jsonl","a");
        if (camera) {
            const HU3D_CAMERA *c=Hu3DCamera+Hu3DCameraNo;
            fprintf(camera,"{\"tick\":%ld,\"camera\":%d,\"fov\":%.9g,\"aspect\":%.9g,"
                "\"viewport\":[%.9g,%.9g,%.9g,%.9g],\"viewMatrix\":[",
                mp6_tick_count,Hu3DCameraNo,c->fov,c->aspect,
                c->viewportX,c->viewportY,c->viewportW,c->viewportH);
            for (int row=0;row<3;row++) for (int col=0;col<4;col++)
                fprintf(camera,"%s%.9g",row||col?",":"",Hu3DCameraMtx[row][col]);
            fprintf(camera,"]}\n");
            fclose(camera);
        }
        FILE *spaces=fopen("spaces.jsonl","a");
        if (spaces) {
            for (int id=1;id<=mbMasuNumGet();id++) {
                MASU *s=mbMasuGet(id);
                Mtx world,rot,trans;
                mtxRot(rot,s->rot.x,s->rot.y,s->rot.z);
                PSMTXTrans(trans,s->pos.x,s->pos.y+3,s->pos.z);
                PSMTXConcat(trans,rot,world);
                if (s->useMtxF) PSMTXCopy(s->matrix,world);
                Mtx authored; PSMTXCopy(world,authored);
                fprintf(spaces,"{\"id\":%d,\"corrected\":%d,\"pos\":[%.9g,%.9g,%.9g],\"matrix\":[",
                    id,memcmp(authored,world,sizeof(Mtx))!=0,s->pos.x,s->pos.y,s->pos.z);
                for (int row=0;row<3;row++) for (int col=0;col<4;col++)
                    fprintf(spaces,"%s%.9g",row||col?",":"",world[row][col]);
                fprintf(spaces,"]}\n");
            }
            fclose(spaces);
        }
    }
    FILE *out = fopen("geometry.jsonl", "a");
    if (!out) return;
    Mtx inv, world;
    PSMTXInverse(Hu3DCameraMtx, inv);
    PSMTXConcat(inv, draw->matrix, world);
    int player = -1;
    for (int p = 0; p < 4; p++)
        if (mbObjModelIDGet(mbPlayerObjIDGet(p)) == draw->model - Hu3DData) player = p;
    fprintf(out, "{\"tick\":%ld,\"model\":%d,\"player\":%d,\"name\":\"%s\","
        "\"attr\":%u,\"flags\":%u,\"cenv\":%d,\"pos\":[%.8g,%.8g,%.8g],\"vertices\":[",
        mp6_tick_count, (int)(draw->model-Hu3DData), player, obj->name,
        (unsigned)((HSF_CONSTDATA *)obj->constData)->attr, (unsigned)obj->flags, draw->model->hsf->cenvNum,
        draw->model->pos.x,draw->model->pos.y,draw->model->pos.z);
    HuVecF *vertices = mp6_ground_vertices(draw->model,obj);
    for (int i=0; i<obj->mesh.vertex->count; i++) {
        HuVecF v;
        PSMTXMultVec(world, vertices+i, &v);
        fprintf(out,"%s[%.8g,%.8g,%.8g]",i?",":"",v.x,v.y,v.z);
    }
    fprintf(out,"],\"localVertices\":[");
    for (int i=0; i<obj->mesh.vertex->count; i++) {
        HuVecF *v=vertices+i;
        fprintf(out,"%s[%.8g,%.8g,%.8g]",i?",":"",v->x,v->y,v->z);
    }
    fprintf(out,"],\"faces\":[");
    HSF_FACE *faces = obj->mesh.face->data;
    for (int i=0; i<obj->mesh.face->count; i++) {
        HSF_FACE *f = faces+i;
        int type = f->typeSrc & HSF_FACE_MASK;
        fprintf(out,"%s[%d",i?",":"",type);
        for (int j=0; j<(type==HSF_FACE_QUAD?4:3); j++) fprintf(out,",%d",f->index[j].vertex);
        if (type == HSF_FACE_TRISTRIP)
            for (unsigned j=0; j<f->strip.count; j++) fprintf(out,",%d",f->strip.data[j].vertex);
        fprintf(out,"]");
    }
    fprintf(out,"]");
    qa_ground_normals(out,draw);
    fprintf(out,",\"parent\":\"%s\",\"children\":[",obj->mesh.parent?obj->mesh.parent->name:"");
    for (unsigned i=0;i<obj->mesh.childNum;i++) fprintf(out,"%s\"%s\"",i?",":"",obj->mesh.child[i]->name);
    fprintf(out,"],\"materials\":[");
    int maxMaterial=-1;
    for (int i=0;i<obj->mesh.face->count;i++)
        if ((faces[i].mat&0xFFF)>maxMaterial) maxMaterial=faces[i].mat&0xFFF;
    for (int i=0;i<=maxMaterial;i++) {
        HSF_MATERIAL *m=obj->mesh.material+i;
        fprintf(out,"%s{\"name\":\"%s\",\"flags\":%u,\"pass\":%u,\"invAlpha\":%.6g,\"textures\":[",
            i?",":"",m->name?m->name:"",(unsigned)m->flags,(unsigned)m->pass,m->invAlpha);
        for (unsigned j=0;j<m->attrNum;j++) {
            if (!obj->mesh.attribute || !m->attr || m->attr[j]<0 || m->attr[j]>=draw->model->hsf->attributeNum) {
                fprintf(out,"%snull",j?",":""); continue;
            }
            HSF_ATTRIBUTE *a=obj->mesh.attribute+m->attr[j];
            HSF_BITMAP *b=a->bitmap;
            if (!b) { fprintf(out,"%snull",j?",":""); continue; }
            if (b->name && (!strcmp(b->name,"s3b01t03") || !strcmp(b->name,"s3b01t10") ||
                !strcmp(b->name,"s3b01t11") || !strcmp(b->name,"s3b01t14")) &&
                b->dataFmt==HSF_BMPFMT_CMPR && b->sizeX>0 && b->sizeX<=128 &&
                b->sizeY>0 && b->sizeY<=128 && b->maxLod==0 && b->data) {
                char path[96];
                snprintf(path,sizeof(path),"fringe_%s_%dx%d_f14.bin",b->name,b->sizeX,b->sizeY);
                FILE *texture=fopen(path,"wb");
                if (texture) {
                    fwrite(b->data,1,((b->sizeX+7)/8)*((b->sizeY+7)/8)*32,texture);
                    fclose(texture);
                }
            }
            fprintf(out,"%s{\"name\":\"%s\",\"format\":%u,\"bits\":%u,\"width\":%d,\"height\":%d,"
                "\"maxLod\":%u,\"attributeFlags\":%u,\"animated\":%d,\"palette\":[",
                j?",":"",b->name?b->name:"",b->dataFmt,b->pixSize,b->sizeX,b->sizeY,
                b->maxLod,a->flag,a->animWorkP!=NULL);
            if (b->palData && b->palSize>0 && b->palSize<=256)
                for (int k=0;k<b->palSize;k++) {
                    const u8 *p=(const u8 *)b->palData+2*k;
                    fprintf(out,"%s%u",k?",":"",((unsigned)p[0]<<8)|p[1]);
                }
            fprintf(out,"]}");
        }
        fprintf(out,"]}");
    }
    fprintf(out,"],\"faceMaterials\":[");
    for (int i=0;i<obj->mesh.face->count;i++) fprintf(out,"%s%u",i?",":"",faces[i].mat&0xFFF);
    fprintf(out,"],\"objectIndex\":%d,\"motionTime\":%.8g,\"motionSpeed\":%.8g,\"shapeTime\":%.8g,\"poseIsBase\":%d,\"modelAttr\":%u,\"aoScenery\":%d,\"localTransform\":{\"pos\":[%.8g,%.8g,%.8g],\"rot\":[%.8g,%.8g,%.8g],\"scale\":[%.8g,%.8g,%.8g]}}\n",
        (int)(obj-draw->model->hsf->object), draw->model->motWork.time,
        draw->model->motWork.speed, draw->model->motShapeWork.time,
        !memcmp(&obj->mesh.curr,&obj->mesh.base,sizeof(HSF_TRANSFORM)),
        draw->model->attr,mp6_ground_is_scenery(draw->model),
        obj->mesh.curr.pos.x,obj->mesh.curr.pos.y,obj->mesh.curr.pos.z,
        obj->mesh.curr.rot.x,obj->mesh.curr.rot.y,obj->mesh.curr.rot.z,
        obj->mesh.curr.scale.x,obj->mesh.curr.scale.y,obj->mesh.curr.scale.z);
    fclose(out);
}
