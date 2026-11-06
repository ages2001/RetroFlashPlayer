#include <stdio.h>
#include "../src/swf.h"
int main(int argc,char**argv){
    int err; SWFMovie*m=swf_load_file(argv[1],&err);
    if(!m){printf("load failed err=%d\n",err);return 1;}
    printf("version %d compressed %d\n",m->version,m->compressed);
    printf("stage twips %d..%d %d..%d => %gx%g px\n",
        m->stage.xmin,m->stage.xmax,m->stage.ymin,m->stage.ymax,
        (m->stage.xmax-m->stage.xmin)/20.0,(m->stage.ymax-m->stage.ymin)/20.0);
    printf("framerate %g framecount %d\n",m->framerate,m->framecount);
    printf("maxid %d  bg #%02x%02x%02x\n",m->maxid,m->bg.r,m->bg.g,m->bg.b);
    printf("root timeline: %d tags, %d frames\n",m->root.ntags,m->root.frame_count);
    /* count dict by type */
    int cnt[16]={0}; int i;
    for(i=0;i<=m->maxid;i++) cnt[m->dict[i].type]++;
    printf("dict: shapes=%d sprites=%d bitmaps=%d fonts=%d text=%d edittext=%d sound=%d button=%d morph=%d\n",
        cnt[1],cnt[2],cnt[3],cnt[4],cnt[5],cnt[6],cnt[7],cnt[8],cnt[9]);
    swf_free(m);
    return 0;
}
