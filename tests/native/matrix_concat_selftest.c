#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
typedef float Mtx[3][4];
typedef float (*MtxPtr)[4];
static void C_MTXCopy(const Mtx a, Mtx b) {
  if (a!=b) for(unsigned i=0;i<3;i++) for(unsigned j=0;j<4;j++) b[i][j]=a[i][j];
}
#include "matrix_subject.inc"
static uint32_t seed=0x4b78319u;
static uint32_t next(void) { seed=seed*1664525u+1013904223u; return seed; }
static unsigned long comparisons;
static void compare(const float* a,const float* b,unsigned n) {
  for(unsigned i=0;i<n;i++) {
    uint32_t x,y; memcpy(&x,a+i,4); memcpy(&y,b+i,4);
    if(x!=y && !(isnan(a[i])&&isnan(b[i]))) {
      fprintf(stderr,"mismatch %lu element %u: %08x != %08x\n",comparisons,i,x,y); exit(1);
    }
  }
  comparisons++;
}
typedef void (*Concat)(const Mtx,const Mtx,Mtx);
static double measure(Concat volatile fn) {
  Mtx a[256],b[256],out;
  for(unsigned i=0;i<256;i++) for(unsigned r=0;r<3;r++) for(unsigned c=0;c<4;c++) {
    a[i][r][c]=(int)(next()%20000)/100.f-100.f;
    b[i][r][c]=(int)(next()%20000)/100.f-100.f;
  }
  clock_t start=clock();
  for(unsigned i=0;i<3000000;i++) fn(a[i&255],b[i&255],out);
  static volatile float sink; sink=out[0][0];
  return 1e9*(double)(clock()-start)/CLOCKS_PER_SEC/3000000.;
}
int main(void) {
  static const unsigned offsets[][3]={{0,16,32},{0,16,0},{0,16,16},{0,0,0},{0,0,32},
    {0,16,1},{0,16,17},{16,0,3},{4,20,0},{8,20,12},{0,1,32},{1,2,0}};
  for(unsigned trial=0;trial<100000;trial++) {
    float initial[64],expected[64],actual[64];
    for(unsigned i=0;i<64;i++) {
      uint32_t bits=next();
      if(trial%3==0) bits&=0x80000000u; // signed zeros
      else if(trial%3==1) bits=(bits&0x807fffffu)|((next()%254u)<<23); // finite extremes
      else { initial[i]=((int)(bits%200000)-100000)/32.f; continue; }
      memcpy(initial+i,&bits,4);
    }
    for(unsigned layout=0;layout<sizeof(offsets)/sizeof(offsets[0]);layout++) {
      memcpy(expected,initial,sizeof initial); memcpy(actual,initial,sizeof initial);
      unsigned a=offsets[layout][0],b=offsets[layout][1],d=offsets[layout][2];
      reference_concat((MtxPtr)(expected+a),(MtxPtr)(expected+b),(MtxPtr)(expected+d));
      C_MTXConcat((MtxPtr)(actual+a),(MtxPtr)(actual+b),(MtxPtr)(actual+d));
      compare(expected,actual,64);
    }
  }
  printf("PASS %lu matrix layouts (finite/infinite/signed-zero bits; NaN classification)\n",comparisons);
  for(unsigned repeat=0;repeat<4;repeat++) {
    double before,after;
    if(repeat&1) { after=measure(C_MTXConcat); before=measure(reference_concat); }
    else { before=measure(reference_concat); after=measure(C_MTXConcat); }
    printf("reference_ns=%.3f candidate_ns=%.3f\n",before,after);
  }
}
