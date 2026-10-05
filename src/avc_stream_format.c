#include "avc_stream_format.h"
#include <string.h>
typedef struct { unsigned char data[1024]; size_t bits,pos; int bad; } Bits;
static unsigned int read_bits(Bits *b,unsigned int n) {
    unsigned int value=0,i;
    if(n>32 || b->pos>b->bits || n>b->bits-b->pos) { b->bad=1; return 0; }
    for(i=0;i<n;i++,b->pos++) value=(value<<1)|((b->data[b->pos/8]>>(7-b->pos%8))&1);
    return value;
}
static unsigned int ue(Bits *b) {
    unsigned int zeros=0;
    while(!b->bad && !read_bits(b,1)) {
        if(++zeros>30) { b->bad=1; return 0; }
    }
    if(b->bad) return 0;
    return ((1u<<zeros)-1)+read_bits(b,zeros);
}
int avc_stream_format(const unsigned char *nal,size_t size,AvcStreamFormat *out) {
    Bits b; AvcStreamFormat f; size_t i,n=0; unsigned int zeros=0,poc,w,h;
    unsigned int crop[4]={0,0,0,0},j,cycle;
    if(!out) return -1;
    memset(out,0,sizeof(*out));
    if(!nal || size<5 || size>1024 || (nal[0]&0x9f)!=7) return -1;
    memset(&b,0,sizeof(b)); memset(&f,0,sizeof(f));
    for(i=1;i<size;i++) {
        unsigned int byte=nal[i];
        if(zeros==2 && byte==3) {
            if(i+1>=size || nal[i+1]>3) return -1;
            zeros=0; continue;
        }
        if(zeros==2 && byte<3) return -1;
        b.data[n++]=(unsigned char)byte;
        zeros=byte==0?zeros+1:0;
    }
    b.bits=n*8;
    f.profile=read_bits(&b,8);
    if(read_bits(&b,8)&3) return -1; /* reserved_zero_2bits */
    f.level=read_bits(&b,8); f.sps_id=ue(&b);
    if(f.profile!=66 && f.profile!=77) return -2;
    if(f.sps_id>31 || ue(&b)>12) return -1;
    poc=ue(&b);
    if(poc==0) { if(ue(&b)>12) return -1; }
    else if(poc==1) {
        read_bits(&b,1); ue(&b); ue(&b); /* signed values only skipped */
        cycle=ue(&b); if(cycle>255) return -1;
        for(j=0;j<cycle;j++) ue(&b);
    } else if(poc!=2) return -1;
    f.refs=ue(&b); read_bits(&b,1);
    w=ue(&b); h=ue(&b);
    if(b.bad) return -1;
    if(w>=30 || h>=17 || f.refs>4) return -2;
    if(!read_bits(&b,1)) return b.bad?-1:-2; /* progressive only */
    read_bits(&b,1); /* direct_8x8_inference_flag */
    if(read_bits(&b,1)) for(j=0;j<4;j++) crop[j]=ue(&b);
    read_bits(&b,1); /* VUI presence, contents not interpreted here */
    if(b.bad) return -1;
    f.coded_width=(w+1)*16; f.coded_height=(h+1)*16;
    for(j=0;j<4;j++) if(crop[j]>240) return -1;
    if(2*(crop[0]+crop[1])>=f.coded_width ||
       2*(crop[2]+crop[3])>=f.coded_height) return -1;
    f.crop_left=2*crop[0]; f.crop_top=2*crop[2];
    f.width=f.coded_width-2*(crop[0]+crop[1]);
    f.height=f.coded_height-2*(crop[2]+crop[3]);
    *out=f; return 0;
}
