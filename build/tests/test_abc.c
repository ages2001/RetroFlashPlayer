#include <stdio.h>
#include <stdlib.h>
#include "../../src/swf.h"
#include "../../src/abc.h"
int main(int argc,char**argv){ int err; SWFMovie*m=swf_load_file(argv[1],&err); if(!m){printf("load fail\n");return 1;}
 int i, n=0; for(i=0;i<m->root.ntags;i++){} 
 /* scan raw body for DoABC tags */
 const u8*b=m->body; size_t pos; { unsigned nb=b[0]>>3; pos=(5+4*nb+7)/8+4; }
 while(pos<m->bodylen){ unsigned th=b[pos]|(b[pos+1]<<8); pos+=2; int code=th>>6; size_t ln=th&0x3f; if(ln==0x3f){ln=b[pos]|(b[pos+1]<<8)|(b[pos+2]<<16)|((size_t)b[pos+3]<<24);pos+=4;}
   if(code==82){ const u8*d=b+pos+4; while(*d)d++; d++; size_t alen=ln-(size_t)(d-(b+pos)); AbcFile*a=abc_parse(d,alen);
     if(!a){printf("ABC parse FAILED\n");return 2;}
     printf("ABC v%d.%d: ints=%u strings=%u ns=%u mn=%u methods=%u classes=%u scripts=%u bodies=%u\n",a->major,a->minor,a->int_count,a->string_count,a->ns_count,a->mn_count,a->method_count,a->class_count,a->script_count,a->body_count);
     unsigned k; for(k=0;k<a->class_count && k<5;k++) printf("  class %s extends %s (iinit %u, %u itraits, %u ctraits)\n",abc_mn_name(a,a->instances[k].name),abc_mn_name(a,a->instances[k].super_name),a->instances[k].iinit,a->instances[k].trait_count,a->classes[k].trait_count);
     abc_free(a); n++; }
   if(code==0)break; pos+=ln; }
 printf("%d DoABC tags\n",n); return 0; }
