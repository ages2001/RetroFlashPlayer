#include <stdio.h>
#include <stdlib.h>
#include "../src/swf.h"
#include "../src/shape.h"
#include "../src/render.h"
static void write_ppm(const char*path,Surface*s){
    FILE*f=fopen(path,"wb"); int x,y; fprintf(f,"P6\n%d %d\n255\n",s->w,s->h);
    for(y=0;y<s->h;y++)for(x=0;x<s->w;x++){u32 p=s->px[y*s->w+x];fputc((p>>16)&255,f);fputc((p>>8)&255,f);fputc(p&255,f);}
    fclose(f);
}
int main(int argc,char**argv){
    int err; SWFMovie*m=swf_load_file(argv[1],&err); if(!m)return 1;
    int W=256,H=256; int k;
    for(k=2;k<argc;k++){
        int id=atoi(argv[k]); SWFChar*ch=swf_get_char(m,id);
        if(!ch||ch->type!=CHAR_SHAPE){printf("id %d not a shape (type %d)\n",id,ch?ch->type:-1);continue;}
        ShapeDef*sh=shape_get(m,ch);
        printf("shape %d v%d: fills=%d lines=%d edges=%d bounds=(%d,%d)-(%d,%d)\n",id,sh->version,sh->nfills,sh->nlines,sh->nedges,
            sh->bounds.xmin,sh->bounds.ymin,sh->bounds.xmax,sh->bounds.ymax);
        int i; for(i=0;i<sh->nfills&&i<4;i++)printf("   fill%d type=0x%02x rgba=%d,%d,%d,%d\n",i+1,sh->fills[i].type,sh->fills[i].color.r,sh->fills[i].color.g,sh->fills[i].color.b,sh->fills[i].color.a);
        Surface*s=surf_create(W,H); SWFColor bg={40,40,60,255}; surf_clear(s,bg);
        double bw=sh->bounds.xmax-sh->bounds.xmin, bh=sh->bounds.ymax-sh->bounds.ymin;
        if(bw<1)bw=1;
        if(bh<1)bh=1;
        double sc=(W-16)/bw; if((H-16)/bh<sc)sc=(H-16)/bh;
        SWFMatrix mt; swf_matrix_identity(&mt); mt.sx=mt.sy=sc;
        mt.tx=-sh->bounds.xmin*sc+8+((W-16)-bw*sc)/2; mt.ty=-sh->bounds.ymin*sc+8+((H-16)-bh*sc)/2;
        SWFCxform cx; swf_cxform_identity(&cx);
        render_shape(s,m,sh,&mt,&cx,NULL);
        char path[64]; sprintf(path,"../out/shape_%d.ppm",id); write_ppm(path,s); surf_free(s);
    }
    swf_free(m); return 0;
}
