// Compare stock/unlocked GEMM using the same binary. Each run validates every output.
// Host elapsed time includes submission/synchronization; batch timings do not include copies.
#include <cuda_runtime.h>
#include <cublas_v2.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#define CUDA(x) do { cudaError_t e=(x); if(e!=cudaSuccess){fprintf(stderr,"%s: %s\n",#x,cudaGetErrorString(e)); return 1;} } while(0)
#define BLAS(x) do { cublasStatus_t e=(x); if(e!=CUBLAS_STATUS_SUCCESS){fprintf(stderr,"%s: %d\n",#x,(int)e); return 1;} } while(0)
static double now(void){struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t);return t.tv_sec+t.tv_nsec*1e-9;}
// All inputs are multiples of 1/8 and exactly representable in FP16 and TF32.
static float row(int i){return ((i%7)-3)*0.25f;}
static float col(int j){return ((j%7)-3)*0.25f;}
static float kval(int k){return ((k%5)-2)*0.125f;}
static uint16_t half_bits(float f){ // Inputs lie between -1 and 1 in increments of 1/8.
 int v=(int)lrintf(f*8); if(!v)return 0; unsigned sign=v<0?0x8000:0; if(v<0)v=-v; unsigned exp=0; while((1u<<(exp+1))<=(unsigned)v)exp++; return (uint16_t)(sign|((exp+12)<<10)|((v-(1<<exp))<<(10-exp))); }
int main(int argc,char**argv){
 if(argc!=3){fprintf(stderr,"usage: %s f16|tf32 N\n",argv[0]);return 2;} int f16=!strcmp(argv[1],"f16"), tf32=!strcmp(argv[1],"tf32"), n=atoi(argv[2]); if((!f16&&!tf32)||n<1||n>8192)return 2;
 const int samples=7,reps=5,warmup=3; size_t cnt=(size_t)n*n, bytes=cnt*(f16?2:4); void *ha=malloc(bytes),*hb=malloc(bytes); float *hc=malloc(cnt*4); if(!ha||!hb||!hc)return 1;
 for(int j=0;j<n;j++)for(int i=0;i<n;i++){size_t p=(size_t)j*n+i; float av=row(i)+kval(j), bv=kval(i)+col(j); if(f16){((uint16_t*)ha)[p]=half_bits(av);((uint16_t*)hb)[p]=half_bits(bv);}else{((float*)ha)[p]=av;((float*)hb)[p]=bv;}}
 double sumk=0,sumk2=0; for(int k=0;k<n;k++){double z=kval(k); sumk+=z; sumk2+=z*z;}
 void *a,*b,*c; CUDA(cudaMalloc(&a,bytes));CUDA(cudaMalloc(&b,bytes));CUDA(cudaMalloc(&c,cnt*4)); CUDA(cudaMemcpy(a,ha,bytes,cudaMemcpyHostToDevice)); CUDA(cudaMemcpy(b,hb,bytes,cudaMemcpyHostToDevice));free(ha);free(hb);
 cublasHandle_t h; BLAS(cublasCreate(&h)); float alpha=1,beta=0; cudaDataType_t dtype=f16?CUDA_R_16F:CUDA_R_32F; cublasComputeType_t comp=f16?CUBLAS_COMPUTE_32F:CUBLAS_COMPUTE_32F_FAST_TF32;
 #define GEMM() cublasGemmEx(h,CUBLAS_OP_N,CUBLAS_OP_N,n,n,n,&alpha,a,dtype,n,b,dtype,n,&beta,c,CUDA_R_32F,n,comp,CUBLAS_GEMM_DEFAULT)
 for(int w=0;w<warmup;w++){BLAS(GEMM());CUDA(cudaDeviceSynchronize());}
 for(int s=0;s<samples;s++){CUDA(cudaDeviceSynchronize());double t=now();for(int r=0;r<reps;r++) BLAS(GEMM());CUDA(cudaDeviceSynchronize());double elapsed=now()-t;printf("sample %d mode=%s n=%d elapsed_s=%.9f tflops=%.6f\n",s,argv[1],n,elapsed,2.0*n*n*n*reps/elapsed/1e12);fflush(stdout);}
 CUDA(cudaMemcpy(hc,c,cnt*4,cudaMemcpyDeviceToHost));size_t bad=0;double maxerr=0;for(int j=0;j<n;j++)for(int i=0;i<n;i++){double r=row(i),q=col(j),ref=(double)n*r*q+(r+q)*sumk+sumk2;float v=hc[(size_t)j*n+i];double e=fabs(v-ref);if(!isfinite(v)||e>0.05)bad++;if(e>maxerr)maxerr=e;} printf("CHECK mode=%s n=%d mismatches=%zu/%zu max_abs_err=%.9g tolerance=0.05\n",argv[1],n,bad,cnt,maxerr);free(hc);BLAS(cublasDestroy(h));CUDA(cudaFree(a));CUDA(cudaFree(b));CUDA(cudaFree(c));return bad?1:0;
}
