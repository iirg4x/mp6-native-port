#include <stdio.h>
#include <string.h>

#define DICE_MAX 5
#define MB_MODEL_NONE (-1)
#define FALSE 0
#define TRUE 1
typedef int BOOL;
typedef struct { int updateF; } DICE_NUM_WORK;
typedef struct { int mdlId[2]; DICE_NUM_WORK work; } OMOBJ;
#define omObjGetWork(obj, type) (&(obj)->work)
static OMOBJ *diceNumOMObj[DICE_MAX][3];
static int stale_on_delete, deleted, models_killed;
static void *HuPrcCurrentGet(void) { return NULL; }
static void mbObjKill(int model) { (void)model; models_killed++; }
static void omDelObj(void *process, OMOBJ *obj)
{
    (void)process;
    for (int p=0;p<DICE_MAX;p++) for (int i=0;i<3;i++)
        if (diceNumOMObj[p][i]==obj) stale_on_delete++;
    obj->work.updateF=TRUE; /* Pool slot reused by an unrelated live object. */
    deleted++;
}

#include "board_subject.inc"

#define CHECK(x) do { if (!(x)) { puts("FAIL: " #x); return 1; } } while (0)
int main(void)
{
    for (int player=0;player<DICE_MAX;player++) for (int slot=0;slot<3;slot++) {
        OMOBJ old={{7,MB_MODEL_NONE},{FALSE}};
        OMOBJ other={{9,10},{TRUE}};
        int neighbor=(player+1)%DICE_MAX;
        memset(diceNumOMObj,0,sizeof(diceNumOMObj));
        diceNumOMObj[player][slot]=&old;
        diceNumOMObj[neighbor][0]=&other;
        stale_on_delete=deleted=models_killed=0;
        mbDiceNumObjKill(&old);
        CHECK(stale_on_delete==0 && deleted==1 && models_killed==1);
        CHECK(diceNumOMObj[player][slot]==NULL);
        CHECK(diceNumOMObj[neighbor][0]==&other);
        CHECK(mbDiceNumStopCheck(player));
        CHECK(!mbDiceNumStopCheck(neighbor));
        other.work.updateF=FALSE;
        CHECK(mbDiceNumStopCheck(neighbor));
    }
    puts("PASS: dice number destruction detaches every stale owner before slot reuse");
    return 0;
}
