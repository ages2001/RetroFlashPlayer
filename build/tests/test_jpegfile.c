#include <stdio.h>
#include <stdlib.h>
#include "../../src/image.h"
Bitmap *jpeg_decode_file_for_test(const u8 *d, size_t n);
int main(int argc,char**argv){ FILE*f=fopen(argv[1],"rb"); fseek(f,0,SEEK_END); long n=ftell(f); fseek(f,0,SEEK_SET); u8*d=malloc(n); if(fread(d,1,n,f)!=(size_t)n)return 2; fclose(f);
 Bitmap*b=jpeg_decode_file_for_test(d,n); if(!b){printf("decode failed\n");return 1;}
 FILE*o=fopen(argv[2],"wb"); fprintf(o,"P6\n%d %d\n255\n",b->w,b->h); int i; for(i=0;i<b->w*b->h;i++){u32 p=b->px[i]; fputc((p>>16)&255,o);fputc((p>>8)&255,o);fputc(p&255,o);} fclose(o); printf("%dx%d\n",b->w,b->h); return 0; }
