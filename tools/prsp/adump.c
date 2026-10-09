/* Test-only audio plugin: appends every AI DMA buffer (raw big-endian N64 samples) to $ADUMP_FILE. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "m64p_types.h"
#include "m64p_plugin.h"
static AUDIO_INFO ai; static FILE *out; static unsigned bufs;
EXPORT m64p_error CALL PluginStartup(m64p_dynlib_handle h, void *c, void (*cb)(void*,int,const char*)){return M64ERR_SUCCESS;}
EXPORT m64p_error CALL PluginShutdown(void){ if(out){fclose(out);out=NULL;} return M64ERR_SUCCESS;}
EXPORT m64p_error CALL PluginGetVersion(m64p_plugin_type *t,int *v,int *a,const char **n,int *c){
  if(t)*t=M64PLUGIN_AUDIO; if(v)*v=0x010000; if(a)*a=0x020000; if(n)*n="audio dump (test)"; if(c)*c=0; return M64ERR_SUCCESS;}
EXPORT void CALL AiDacrateChanged(int s){}
EXPORT void CALL AiLenChanged(void){
  if(!out||!ai.RDRAM) return;
  unsigned addr=*ai.AI_DRAM_ADDR_REG & 0xFFFFF8, len=*ai.AI_LEN_REG & 0x3FFF8;
  /* RDRAM is stored host-endian per 32-bit word in the core; write words as big-endian */
  for(unsigned i=0;i<len;i+=4){unsigned w=*(unsigned*)(ai.RDRAM+addr+i); fputc(w>>24,out);fputc(w>>16,out);fputc(w>>8,out);fputc(w,out);} 
  bufs++;
}
EXPORT int CALL InitiateAudio(AUDIO_INFO i){ai=i; const char*f=getenv("ADUMP_FILE"); if(f) out=fopen(f,"wb"); return 1;}
EXPORT void CALL ProcessAList(void){}
EXPORT int CALL RomOpen(void){return 1;}
EXPORT void CALL RomClosed(void){ if(out){fflush(out);} }
EXPORT void CALL SetSpeedFactor(int p){}
EXPORT void CALL VolumeUp(void){}
EXPORT void CALL VolumeDown(void){}
EXPORT int CALL VolumeGetLevel(void){return 100;}
EXPORT void CALL VolumeSetLevel(int l){}
EXPORT void CALL VolumeMute(void){}
EXPORT const char * CALL VolumeGetString(void){return "100%";}
