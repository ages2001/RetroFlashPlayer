#include <stdio.h>
#include <stdlib.h>
#include "../src/image.h"
int write_png(const char *path, const Surface *s);
int main(int argc,char**argv){ int err,i; SWFMovie*m=swf_load_file(argv[1],&err); Surface*s=surf_create(64*8,64); SWFColor bg={255,0,255,255}; surf_clear(s,bg);
 int ids[8]={926,2163,2238,2246,3033,0,0,0}; int n=5, k=0;
 for(i=0;i<=m->maxid && k<8;i++){ SWFChar*c=&m->dict[i]; if(c->type==CHAR_BITMAP && c->tagcode==TAG_DefineBits){ Bitmap*b=image_get(m,i); if(!b)continue; int x,y; for(y=0;y<b->h&&y<64;y++)for(x=0;x<b->w&&x<64;x++){u32 p=b->px[y*b->w+x]; s->px[y*s->w+k*64+x]=p&0xffffff;} k++; } }
 write_png(argv[2],s); return 0; }
