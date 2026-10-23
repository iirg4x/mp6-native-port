/* QA only: compare actual loaded asset prefixes across margin registration. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "game/hu3d.h"
#include "mp6_widescreen.h"
static void qa_wide_terrain(s16 id, const char *name, float center, float reach, int mirror)
{
    HSF_DATA *hsf=Hu3DData[id].hsf;
    HSF_OBJECT *obj=NULL;
    for (int i=0;i<hsf->objectNum;i++)
        if (hsf->object[i].name && !strcmp(hsf->object[i].name,name)) obj=&hsf->object[i];
    if (!obj || !obj->mesh.vertex || !obj->mesh.st || !obj->mesh.face) {
        fprintf(stderr,"[BOARD-QA] FATAL missing terrain %s\n",name); exit(3);
    }
    size_t nv=obj->mesh.vertex->count*sizeof(HuVecF);
    size_t nt=obj->mesh.st->count*sizeof(HuVec2f);
    size_t nf=obj->mesh.face->count*sizeof(HSF_FACE);
    unsigned char *copy=malloc(nv+nt+nf);
    if (!copy) exit(3);
    memcpy(copy,obj->mesh.vertex->data,nv);
    memcpy(copy+nv,obj->mesh.st->data,nt);
    memcpy(copy+nv+nt,obj->mesh.face->data,nf);
    mp6_widescreen_extend_terrain_outer_sides(id,name,center,reach,mirror);
    if (memcmp(copy,obj->mesh.vertex->data,nv) || memcmp(copy+nv,obj->mesh.st->data,nt) ||
        memcmp(copy+nv+nt,obj->mesh.face->data,nf)) {
        fprintf(stderr,"[BOARD-QA] FATAL native terrain altered %s\n",name); exit(3);
    }
    printf("[WIDE-TERRAIN-QA] %s native-prefix exact vertices=%zu uv=%zu faces=%zu added-faces=%zu\n",
           name,nv/sizeof(HuVecF),nt/sizeof(HuVec2f),nf/sizeof(HSF_FACE),
           obj->mesh.face->count-nf/sizeof(HSF_FACE));
    free(copy);
}
