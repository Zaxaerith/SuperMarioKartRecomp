/* Single-threaded diagnostic host. No replacement CPU/device implementations. */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include "game_rtl.h"
#include "common_rtl.h"
#include "cpu_state.h"
#include "snes/snes.h"
#include "snes/ppu.h"
#include "snes/dsp1.h"
#include "snes/interp_bridge.h"
#include "config.h"

struct SpcPlayer *g_spc_player;
Config g_config;
int g_ws_extra;
extern int g_watchdog_tripped;
extern uint64_t smk_nmi_count, smk_irq_count, smk_max_late;
static struct {int begin,end; unsigned mask;} inputs[64];
static int input_count;
static void parse_inputs(void) {
  const char *s=getenv("SMK_SCRIPT"); if(!s) return;
  char buf[1024]; if(strlen(s)>=sizeof buf) Die("input script too long");
  strcpy(buf,s);
  const char *names[]={"B","Y","SELECT","START","UP","DOWN","LEFT","RIGHT","A","X","L","R"};
  for(char *t=strtok(buf,","); t; t=strtok(NULL,",")) {
    int a,b; char name[32];
    if(sscanf(t,"%d-%d:%31s",&a,&b,name)!=3) {
      if(sscanf(t,"%d:%31s",&a,name)!=2) Die("invalid input token");
      b=a+3;
    }
    /* RtlRunFrame packs two 12-bit pads. Unprefixed names remain P1. */
    unsigned shift=0; const char *button=name;
    if(!strncmp(name,"P1_",3)) button=name+3;
    else if(!strncmp(name,"P2_",3)) {button=name+3;shift=12;}
    unsigned bit=0; for(int j=0;j<12;j++) if(!strcmp(button,names[j])) bit=1u<<(j+shift);
    if(!bit || a<0 || b<a || input_count==64) Die("invalid input interval");
    inputs[input_count].begin=a;inputs[input_count].end=b;inputs[input_count++].mask=bit;
  }
}
static unsigned input_at(int frame) {
  unsigned v=0;for(int j=0;j<input_count;j++) if(frame>=inputs[j].begin && frame<=inputs[j].end) v|=inputs[j].mask;
  return v;
}
static void snapshot(int frame) {
  const char *prefix=getenv("SMK_SNAPSHOT_PREFIX"); if(!prefix) return;
  char path[1024];snprintf(path,sizeof path,"%s_f%06d.bin",prefix,frame);
  FILE *f=fopen(path,"wb"); if(!f) Die("snapshot open failed");
  uint32_t sizes[]={sizeof g_ram,sizeof g_ppu->vram,sizeof g_ppu->cgram};
  int ok=fwrite("SMKSNAP2",1,8,f)==8 && fwrite(sizes,1,sizeof sizes,f)==sizeof sizes &&
    fwrite(g_ram,1,sizes[0],f)==sizes[0] && fwrite(g_ppu->vram,1,sizes[1],f)==sizes[1] && fwrite(g_ppu->cgram,1,sizes[2],f)==sizes[2];
  if(fclose(f)!=0 || !ok) Die("snapshot write failed");
}
void RtlApuLock(void) {}
void RtlApuUnlock(void) {}
void debug_on_block_enter(uint32_t pc, uint32_t a, uint32_t x, uint32_t y) {(void)pc;(void)a;(void)x;(void)y;}
void debug_on_wram_write_byte(uint32_t a, uint8_t b, uint8_t c) {(void)a;(void)b;(void)c;}
void debug_on_wram_write_word(uint32_t a, uint16_t b, uint16_t c) {(void)a;(void)b;(void)c;}

void NORETURN Die(const char *s) {fprintf(stderr,"FATAL: %s\n",s);exit(1);}
static uint32_t hash(const void *p, size_t n) {
  const uint8_t *b=p; uint32_t h=2166136261u;
  while(n--) h=(h^*b++)*16777619u;
  return h;
}
int main(int argc, char **argv) {
  if(argc!=3) {fprintf(stderr,"usage: smk_headless ROM FRAMES\n");return 2;}
  int frames=atoi(argv[2]); if(frames<1 || frames>36000) return 2;
  FILE *f=fopen(argv[1],"rb"); if(!f) return 3;
  fseek(f,0,SEEK_END); long size=ftell(f); rewind(f);
  if(size!=524288) {fclose(f);return 3;}
  uint8_t *rom=malloc(size);
  if(!rom || fread(rom,1,size,f)!=(size_t)size) return 3;
  fclose(f);
  RtlRegisterGame(&kGameInfo);
  if(!SnesInit(rom,(int)size)) return 4;
  free(rom);
  if(!cart_has_dsp1(g_snes->cart)) return 4;
  parse_inputs();
  static uint8_t pixels[256*4*256];
  PpuBeginDrawing(g_ppu,pixels,256*4,0);
  for(int i=0;i<frames;i++) {
    RtlRunFrame(input_at(i));
    GameDrawPpuFrame();
    if(g_fail || g_watchdog_tripped || dsp1_hle_failed(g_snes->cart->dsp1)) return 5;
    if((i+1)%100==0 || i+1==10 || i+1==frames) snapshot(i+1);
    if(i<3 || (i+1)%10==0 || i+1==frames) {
      int sites;unsigned long long clean,bail;interp_tier2_stats(&sites,&clean,&bail);
      printf("{\"frame\":%d,\"master\":%llu,\"pc\":%u,\"wram\":\"%08x\",\"pixels\":\"%08x\",\"mode\":%u,\"pending\":%u,\"state\":%u,\"nmi_enabled\":%d,\"nmi\":%llu,\"irq\":%llu,\"max_deadline_late\":%llu,\"dsp_reads\":%llu,\"dsp_writes\":%llu,\"tier_hits\":%ld,\"tier_sites\":%d,\"tier_clean\":%llu,\"tier_bail\":%llu}\n",
        i+1,(unsigned long long)g_cpu.master_cycles,interp_bridge_lle_resume_pc(),hash(g_ram,0x20000),hash(pixels,sizeof pixels),g_ram[0x2e],g_ram[0x32],g_ram[0x36],g_snes->nmiEnabled,(unsigned long long)smk_nmi_count,(unsigned long long)smk_irq_count,(unsigned long long)smk_max_late,(unsigned long long)dsp1_host_reads(g_snes->cart->dsp1),(unsigned long long)dsp1_host_writes(g_snes->cart->dsp1),interp_tier_hit_count(),sites,clean,bail);
      fflush(stdout);
    }
  }
  const char *image=getenv("SMK_FRAME_PPM");
  if(image) {
    FILE *out=fopen(image,"wb");if(!out) Die("frame image open failed");
    fprintf(out,"P6\n256 224\n255\n");
    for(int p=0;p<256*224;p++) {unsigned char rgb[]={pixels[p*4+2],pixels[p*4+1],pixels[p*4]};fwrite(rgb,1,3,out);}
    if(fclose(out)) Die("frame image write failed");
  }
  return 0;
}
