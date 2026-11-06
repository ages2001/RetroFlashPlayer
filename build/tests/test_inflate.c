#include <stdio.h>
#include <stdlib.h>
#include "../src/inflate.h"
int main(int argc,char**argv){
    FILE*f=fopen(argv[1],"rb"); fseek(f,0,SEEK_END); long n=ftell(f); fseek(f,0,SEEK_SET);
    u8*comp=malloc(n); fread(comp,1,n,f); fclose(f);
    size_t cap=(size_t)atol(argv[3]);
    u8*out=malloc(cap);
    long r=swf_inflate_zlib(comp,n,out,cap);
    fprintf(stderr,"inflate returned %ld (cap %lu)\n",r,(unsigned long)cap);
    if(r<0){return 2;}
    FILE*g=fopen(argv[2],"wb"); fwrite(out,1,r,g); fclose(g);
    return 0;
}
