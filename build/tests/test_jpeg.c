#include <stdio.h>
#include "../src/image.h"
int main(int argc,char**argv){ int err,i,ok=0,fail=0; SWFMovie*m=swf_load_file(argv[1],&err); if(!m)return 1;
 for(i=0;i<=m->maxid;i++){ SWFChar*c=&m->dict[i]; if(c->type!=CHAR_BITMAP||c->tagcode==TAG_DefineBitsLossless||c->tagcode==TAG_DefineBitsLossless2)continue;
   Bitmap*b=image_get(m,i); if(b){ok++; if(ok<=5)printf("id %d tag %d -> %dx%d px0=%08x\n",i,c->tagcode,b->w,b->h,b->px[b->w*b->h/2]);} else {fail++; printf("FAIL id %d tag %d len %lu\n",i,c->tagcode,(unsigned long)c->len);} }
 printf("jpeg ok=%d fail=%d\n",ok,fail); return 0; }
