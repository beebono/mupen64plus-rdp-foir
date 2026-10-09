/* Test-only timing shim: loads the RSP plugin named by $RSPT_TARGET, forwards the plugin API to it and
 * accumulates the wall time spent in DoRspCycles (CLOCK_MONOTONIC). Writes "<CLOCK_BOOTTIME s> <calls> <cumulative RSP ms>"
 * about once a second to $RSPT_LOG. */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <dlfcn.h>
#include "m64p_types.h"
#include "m64p_plugin.h"
static void *h; static FILE *lg; static double cum_ms; static unsigned long calls; static double last_log;
static m64p_error (*p_start)(m64p_dynlib_handle, void *, void (*)(void *, int, const char *));
static m64p_error (*p_stop)(void);
static m64p_error (*p_ver)(m64p_plugin_type *, int *, int *, const char **, int *);
static unsigned int (*p_do)(unsigned int);
static void (*p_init)(RSP_INFO, unsigned int *);
static void (*p_rc)(void);
static RSP_INFO rinfo; static FILE *hl; static unsigned long long hacc=0xcbf29ce484222325ull;
static unsigned long long fnv(const unsigned char*d,unsigned n,unsigned long long h){for(unsigned i=0;i<n;i++)h=(h^d[i])*0x100000001b3ull;return h;}
static double now(clockid_t c){struct timespec t; clock_gettime(c,&t); return t.tv_sec+t.tv_nsec*1e-9;}
EXPORT m64p_error CALL PluginStartup(m64p_dynlib_handle ch, void *ctx, void (*cb)(void *, int, const char *)){
  const char *t=getenv("RSPT_TARGET"); const char *l=getenv("RSPT_LOG");
  h=dlopen(t,RTLD_NOW|RTLD_LOCAL); if(!h) return M64ERR_INPUT_NOT_FOUND;
  p_start=dlsym(h,"PluginStartup"); p_stop=dlsym(h,"PluginShutdown"); p_ver=dlsym(h,"PluginGetVersion");
  p_do=dlsym(h,"DoRspCycles"); p_init=dlsym(h,"InitiateRSP"); p_rc=dlsym(h,"RomClosed");
  if(l) lg=fopen(l,"w");
  { const char *hf=getenv("RSPT_HASH"); if(hf) hl=fopen(hf,"w"); }
  return p_start(ch,ctx,cb);
}
EXPORT m64p_error CALL PluginShutdown(void){ m64p_error e=p_stop(); if(lg){fclose(lg);lg=NULL;} if(hl){fclose(hl);hl=NULL;} return e; }
EXPORT m64p_error CALL PluginGetVersion(m64p_plugin_type *t,int *v,int *a,const char **n,int *c){ if(!p_ver){ if(t)*t=M64PLUGIN_RSP; if(a)*a=0x020000; return M64ERR_SUCCESS;} return p_ver(t,v,a,n,c);}
EXPORT unsigned int CALL DoRspCycles(unsigned int c){
  double a=now(CLOCK_MONOTONIC); unsigned r=p_do(c); double b=now(CLOCK_MONOTONIC);
  cum_ms+=(b-a)*1e3; calls++;
  /* RSPT_HASH=<file>: one line per DoRspCycles call: FNV-1 of DMEM+IMEM and the return value (determinism / bit-exactness check) */
  if(hl && rinfo.DMEM){ unsigned long long h=fnv(rinfo.DMEM,4096,0xcbf29ce484222325ull); h=fnv(rinfo.IMEM,4096,h); h=(h^r)*0x100000001b3ull; fprintf(hl,"%llx\n",h); }
  if(lg && b-last_log>=1.0){ last_log=b; fprintf(lg,"%.3f %lu %.3f\n",now(CLOCK_BOOTTIME),calls,cum_ms); fflush(lg);} 
  return r; }
EXPORT void CALL InitiateRSP(RSP_INFO i, unsigned int *cc){ rinfo=i; p_init(i,cc); }
EXPORT void CALL RomClosed(void){ p_rc(); }
