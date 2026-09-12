/* QA only: capture the actual submitted, skinned world-space mesh geometry.
 * One frame, camera zero, no shadow pass; never changes the scene. */
#include "game/board/player.h"
#include "game/board/masu.h"
#include "game/board/object.h"
#include "mp6_grounding.h"
static void qa_ground_geometry(HU3D_DRAW_OBJ *draw)
{
    const char *target = getenv("MP6_QA_GEOMETRY_AT");
    HSF_OBJECT *obj = draw->object;
    if (!target || mp6_tick_count != atol(target) || Hu3DCameraNo != 0 ||
        shadowModelDrawF || !obj || !obj->mesh.vertex || !obj->mesh.face) return;
    static long spaceTick=-1;
    if (spaceTick!=mp6_tick_count) {
        spaceTick=mp6_tick_count;
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
    fprintf(out,"],\"parent\":\"%s\",\"children\":[",obj->mesh.parent?obj->mesh.parent->name:"");
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
    fprintf(out,"],\"objectIndex\":%d,\"motionTime\":%.8g,\"shapeTime\":%.8g,\"poseIsBase\":%d,\"modelAttr\":%u,\"aoScenery\":%d}\n",
        (int)(obj-draw->model->hsf->object), draw->model->motWork.time,
        draw->model->motShapeWork.time, !memcmp(&obj->mesh.curr,&obj->mesh.base,sizeof(HSF_TRANSFORM)),
        draw->model->attr,mp6_ground_is_scenery(draw->model));
    fclose(out);
}
