#include <stdio.h>
#include <stdlib.h>
#include "../src/audio.h"
#include "../src/reader.h"
int main(int argc,char**argv){ int err,i,n=0; SWFMovie*m=swf_load_file(argv[1],&err); if(!m)return 1;
 for(i=0;i<=m->maxid;i++){ SWFChar*c=&m->dict[i]; if(c->type!=CHAR_SOUND)continue; SoundDef*s=sound_get(m,i); if(!s)continue;
   if(n<4){ printf("sound %d fmt %d rate %d st %d frames %d\n",i,s->format,s->rate,s->stereo,s->nframes);
     char path[64]; sprintf(path,"/home/claude/work/out/snd_%d.raw",i); FILE*f=fopen(path,"wb"); fwrite(s->pcm,2,(size_t)s->nframes*(s->stereo?2:1),f); fclose(f);
     if(s->format==2){ sprintf(path,"/home/claude/work/out/snd_%d.mp3",i); f=fopen(path,"wb"); Reader r; reader_init(&r,c->data,c->len); r_u16(&r); r_u8(&r); r_u32(&r); r_u16(&r); fwrite(c->data+r.pos,1,c->len-r.pos,f); fclose(f);} }
   n++; }
 printf("decoded %d sounds\n",n); return 0; }
