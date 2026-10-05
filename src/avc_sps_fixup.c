#include "avc_sps_fixup.h"
#include "avc_stream_format.h"
#include <string.h>

typedef struct { unsigned char bytes[1024]; size_t bits,pos; int bad; } SpsBits;

static unsigned int read_bits(SpsBits *b,unsigned int count) {
    unsigned int value=0,i;
    if(count>32 || b->pos>b->bits || count>b->bits-b->pos) {
        b->bad=1; return 0;
    }
    for(i=0;i<count;i++,b->pos++)
        value=(value<<1)|((b->bytes[b->pos/8]>>(7-b->pos%8))&1);
    return value;
}
static unsigned int read_ue(SpsBits *b) {
    unsigned int zeros=0;
    while(!b->bad && !read_bits(b,1)) {
        if(++zeros>30) { b->bad=1; return 0; }
    }
    if(b->bad) return 0;
    return ((1u<<zeros)-1)+read_bits(b,zeros);
}
static int put_bit(unsigned char *bytes,size_t capacity,size_t *pos,unsigned int bit) {
    if(*pos>=capacity*8) return -1;
    if(bit) bytes[*pos/8]|=(unsigned char)(0x80u>>(*pos%8));
    ++*pos; return 0;
}
static int copy_bits(unsigned char *dst,size_t capacity,size_t *out_pos,
                     const SpsBits *src,size_t first,size_t end) {
    size_t i;
    for(i=first;i<end;i++)
        if(put_bit(dst,capacity,out_pos,
                   (src->bytes[i/8]>>(7-i%8))&1)) return -1;
    return 0;
}

static int load_rbsp(const unsigned char *nal,size_t nal_size,
                     unsigned int nal_type,SpsBits *out) {
    size_t i,n=0;
    unsigned int zeros=0;
    if(!nal || !out || nal_size<2 || nal_size>1024 ||
       (nal[0]&0x9f)!=nal_type) return -1;
    memset(out,0,sizeof(*out));
    for(i=1;i<nal_size;i++) {
        unsigned int byte=nal[i];
        if(zeros==2 && byte==3) {
            if(i+1>=nal_size || nal[i+1]>3) return -1;
            zeros=0;
            continue;
        }
        if(zeros==2 && byte<3) return -1;
        if(n>=sizeof(out->bytes)) return -1;
        out->bytes[n++]=(unsigned char)byte;
        zeros=byte==0?zeros+1:0;
    }
    if(!n) return -1;
    out->bits=n*8;
    return 0;
}

static int rbsp_stop_bit(const SpsBits *b,size_t *stop) {
    size_t pos;
    if(!b || !stop) return -1;
    for(pos=b->bits;pos>0;pos--) {
        size_t bit=pos-1;
        if((b->bytes[bit/8]>>(7-bit%8))&1u) {
            *stop=bit;
            return 0;
        }
    }
    return -1;
}

static int read_se(SpsBits *b) {
    unsigned int code=read_ue(b);
    if(b->bad) return 0;
    return (code&1u)?(int)((code+1u)>>1):-(int)(code>>1);
}

static int put_bits(unsigned char *dst,size_t capacity,size_t *pos,
                    unsigned int value,unsigned int count) {
    unsigned int bit;
    if(count>32) return -1;
    for(bit=count;bit>0;bit--)
        if(put_bit(dst,capacity,pos,(value>>(bit-1))&1u)) return -1;
    return 0;
}

static int write_nal(unsigned char header,const unsigned char *rbsp,
                     size_t rbsp_size,unsigned char *dst,size_t dst_capacity,
                     size_t *dst_size) {
    size_t i,at=1;
    unsigned int zeros=0;
    if(!rbsp || !dst || !dst_size || !dst_capacity) return -1;
    dst[0]=header;
    for(i=0;i<rbsp_size;i++) {
        unsigned int byte=rbsp[i];
        if(zeros==2 && byte<=3) {
            if(at>=dst_capacity) return -1;
            dst[at++]=3;
            zeros=0;
        }
        if(at>=dst_capacity) return -1;
        dst[at++]=(unsigned char)byte;
        zeros=byte==0?zeros+1:0;
    }
    *dst_size=at;
    return 0;
}

int avc_sps_normalize_main(const unsigned char *src,size_t src_size,
                           unsigned char *dst,size_t dst_capacity,
                           size_t *dst_size,int *converted) {
    SpsBits in;
    unsigned char rewritten[1024];
    AvcStreamFormat format;
    size_t stop,id_end,extension_end,out_bits=0;
    unsigned int profile,constraints,level,sps_id,chroma,depth_luma,depth_chroma;
    unsigned int transform_bypass,scaling_matrix;
    if(dst_size) *dst_size=0;
    if(converted) *converted=0;
    if(!src || !dst || !dst_size || !converted || dst_capacity<5 ||
       load_rbsp(src,src_size,7,&in) || rbsp_stop_bit(&in,&stop)) return -1;
    in.bits=stop;
    profile=read_bits(&in,8);
    constraints=read_bits(&in,8);
    level=read_bits(&in,8);
    sps_id=read_ue(&in);
    id_end=in.pos;
    if(in.bad || sps_id>31) return -1;
    if(profile==66 || profile==77) {
        if(src_size>dst_capacity) return -1;
        if(dst!=src) memcpy(dst,src,src_size);
        *dst_size=src_size;
        return 0;
    }
    if(profile!=100) return -2;

    /* Main supports CABAC but not the High-profile SPS/PPS tools below. Only
     * normalize High streams whose optional syntax proves those tools unused. */
    chroma=read_ue(&in);
    if(chroma!=1) return -3; /* only 8-bit 4:2:0 is Main-compatible */
    depth_luma=read_ue(&in);
    depth_chroma=read_ue(&in);
    transform_bypass=read_bits(&in,1);
    scaling_matrix=read_bits(&in,1);
    extension_end=in.pos;
    if(in.bad || (constraints&3u) || depth_luma || depth_chroma ||
       transform_bypass || scaling_matrix ||
       extension_end>=stop) return -3;

    memset(rewritten,0,sizeof(rewritten));
    /* The current High SPS uses constraint_set5 to promise no B slices. Drop
     * profile-specific constraint flags when spelling the same tools as Main. */
    if(put_bits(rewritten,sizeof(rewritten),&out_bits,77,8) ||
       put_bits(rewritten,sizeof(rewritten),&out_bits,0,8) ||
       put_bits(rewritten,sizeof(rewritten),&out_bits,level,8) ||
       copy_bits(rewritten,sizeof(rewritten),&out_bits,&in,24,id_end) ||
       copy_bits(rewritten,sizeof(rewritten),&out_bits,&in,extension_end,stop) ||
       put_bit(rewritten,sizeof(rewritten),&out_bits,1)) return -1;
    while(out_bits%8)
        if(put_bit(rewritten,sizeof(rewritten),&out_bits,0)) return -1;
    if(write_nal(src[0],rewritten,out_bits/8,dst,dst_capacity,dst_size)) return -1;
    if(avc_stream_format(dst,*dst_size,&format) || format.profile!=77 ||
       format.sps_id!=sps_id || format.level!=level) {
        *dst_size=0;
        return -1;
    }
    *converted=1;
    return 0;
}

int avc_pps_validate_main(const unsigned char *nal,size_t size,
                          unsigned int expected_sps_id,
                          int *entropy_coding_mode) {
    return avc_pps_validate_main_ex(nal,size,expected_sps_id,
                                    entropy_coding_mode,NULL);
}

int avc_pps_validate_main_ex(const unsigned char *nal,size_t size,
                             unsigned int expected_sps_id,
                             int *entropy_coding_mode,
                             unsigned int *parsed_pps_id) {
    SpsBits in;
    size_t stop;
    unsigned int pps_id,sps_id,entropy,slice_groups;
    if(entropy_coding_mode) *entropy_coding_mode=0;
    if(parsed_pps_id) *parsed_pps_id=0;
    if(!entropy_coding_mode || load_rbsp(nal,size,8,&in) ||
       rbsp_stop_bit(&in,&stop)) return -1;
    in.bits=stop;
    pps_id=read_ue(&in);
    sps_id=read_ue(&in);
    entropy=read_bits(&in,1);
    read_bits(&in,1); /* bottom_field_pic_order_in_frame_present_flag */
    slice_groups=read_ue(&in);
    read_ue(&in); /* num_ref_idx_l0_default_active_minus1 */
    read_ue(&in); /* num_ref_idx_l1_default_active_minus1 */
    read_bits(&in,1); /* weighted_pred_flag */
    read_bits(&in,2); /* weighted_bipred_idc */
    read_se(&in); /* pic_init_qp_minus26 */
    read_se(&in); /* pic_init_qs_minus26 */
    read_se(&in); /* chroma_qp_index_offset */
    read_bits(&in,1); /* deblocking_filter_control_present_flag */
    read_bits(&in,1); /* constrained_intra_pred_flag */
    read_bits(&in,1); /* redundant_pic_cnt_present_flag */
    if(in.bad || pps_id>255 || sps_id!=expected_sps_id || slice_groups!=0 ||
       in.pos!=stop) return -2; /* no FMO and no High-profile PPS extension */
    *entropy_coding_mode=(int)entropy;
    if(parsed_pps_id) *parsed_pps_id=pps_id;
    return 0;
}

int avc_sps_limit_refs(const unsigned char *src,size_t src_size,
                       unsigned char *dst,size_t dst_capacity,
                       size_t *dst_size,unsigned int *original_refs) {
    SpsBits in; unsigned char rewritten[1024];
    AvcStreamFormat before,after;
    size_t i,n=0,start,end,payload_bits,out_bits=0,at=1;
    unsigned int zeros=0,profile,poc,cycle,refs,j;
    if(dst_size) *dst_size=0;
    if(original_refs) *original_refs=0;
    if(!src || !dst || !dst_size || !original_refs ||
       src_size<5 || src_size>1024 || dst_capacity<5 ||
       (src[0]&0x9f)!=7 || avc_stream_format(src,src_size,&before)) return -1;
    memset(&in,0,sizeof(in)); memset(rewritten,0,sizeof(rewritten));
    for(i=1;i<src_size;i++) {
        unsigned int byte=src[i];
        if(zeros==2 && byte==3) {
            if(i+1>=src_size || src[i+1]>3) return -1;
            zeros=0; continue;
        }
        if(zeros==2 && byte<3) return -1;
        in.bytes[n++]=(unsigned char)byte;
        zeros=byte==0?zeros+1:0;
    }
    in.bits=n*8;
    profile=read_bits(&in,8); read_bits(&in,8); read_bits(&in,8);
    read_ue(&in); /* seq_parameter_set_id */
    if(profile!=66 && profile!=77) return -1;
    read_ue(&in); /* log2_max_frame_num_minus4 */
    poc=read_ue(&in);
    if(poc==0) read_ue(&in);
    else if(poc==1) {
        read_bits(&in,1); read_ue(&in); read_ue(&in);
        cycle=read_ue(&in);
        if(cycle>255) return -1;
        for(j=0;j<cycle;j++) read_ue(&in);
    } else if(poc!=2) return -1;
    if(in.bad) return -1;
    start=in.pos;
    refs=read_ue(&in);
    end=in.pos;
    if(in.bad || refs>4) return -1;
    *original_refs=refs;
    if(refs<=1) {
        if(src_size>dst_capacity) return -1;
        if(dst!=src) memcpy(dst,src,src_size);
        *dst_size=src_size; return 0;
    }
    /* Find rbsp_stop_one_bit; old byte-alignment zeroes must not be copied
     * after changing the Exp-Golomb field length. */
    payload_bits=in.bits;
    while(payload_bits && !(in.bytes[(payload_bits-1)/8] &
                            (0x80u>>((payload_bits-1)%8)))) --payload_bits;
    if(payload_bits<=end) return -1;
    --payload_bits;
    if(copy_bits(rewritten,sizeof(rewritten),&out_bits,&in,0,start) ||
       put_bit(rewritten,sizeof(rewritten),&out_bits,0) ||
       put_bit(rewritten,sizeof(rewritten),&out_bits,1) ||
       put_bit(rewritten,sizeof(rewritten),&out_bits,0) ||
       copy_bits(rewritten,sizeof(rewritten),&out_bits,&in,end,payload_bits) ||
       put_bit(rewritten,sizeof(rewritten),&out_bits,1)) return -1;
    while(out_bits%8) if(put_bit(rewritten,sizeof(rewritten),&out_bits,0)) return -1;
    dst[0]=src[0]; zeros=0;
    for(i=0;i<out_bits/8;i++) {
        unsigned int byte=rewritten[i];
        if(zeros==2 && byte<=3) {
            if(at>=dst_capacity) return -1;
            dst[at++]=3; zeros=0;
        }
        if(at>=dst_capacity) return -1;
        dst[at++]=(unsigned char)byte;
        zeros=byte==0?zeros+1:0;
    }
    if(avc_stream_format(dst,at,&after) || after.refs!=1 ||
       before.profile!=after.profile || before.level!=after.level ||
       before.sps_id!=after.sps_id || before.width!=after.width ||
       before.height!=after.height ||
       before.coded_width!=after.coded_width ||
       before.coded_height!=after.coded_height ||
       before.crop_left!=after.crop_left || before.crop_top!=after.crop_top)
        return -1;
    *dst_size=at;
    return 0;
}
