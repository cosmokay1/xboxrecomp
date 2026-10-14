#define _GNU_SOURCE
#include <assert.h>
#include <sys/mman.h>
#include <unistd.h>
#include "../../src/kernel/nv2a_pb_exec.c"

/* Synthetic guest mapping only; these never replace production access guards. */
static unsigned char *guest;
uint32_t g_xbox_image_lo, g_xbox_image_hi;
uint32_t g_xbox_contig_size;
ptrdiff_t xbox_GetMemoryOffset(void) { return (ptrdiff_t)guest; }
uint32_t xbox_ContiguousAllocatedBytes(void) { return 0; }

int main(void)
{
    const size_t page=(size_t)sysconf(_SC_PAGESIZE);
    uint32_t *fb,*tex,pixel;
    float out[4];
    const float a[2]={0,0},b[2]={4,0},c[2]={0,4};
    const float uv[3][2]={{0,0},{0,0},{0,0}};
    guest=mmap(NULL,page*6,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    assert(guest!=MAP_FAILED);
    fb=(uint32_t *)(guest+page); tex=(uint32_t *)(guest+page*2);
    for (int i=0;i<16;i++) { fb[i]=0xFF0000FFu;tex[i]=0x80FF0000u; }
    memset(&s_gpu,0,sizeof s_gpu);
    s_gpu.color_offset=(uint32_t)page;s_gpu.pitch=16;s_gpu.clip_w=s_gpu.clip_h=4;
    s_gpu.blend=1;s_gpu.blend_sf=0x0302;s_gpu.blend_df=0x0303;
    s_gpu.tex.offset=(uint32_t)(page*2);s_gpu.tex.width=s_gpu.tex.height=4;
    s_gpu.tex.pitch=16;s_gpu.tex.color=0x12;s_gpu.tex.valid=s_gpu.tex.enabled=1;
    s_stage[1]=s_gpu.tex;s_stage[1].offset+=(uint32_t)64;
    for (int i=0;i<16;i++) tex[16+i]=0xFF00FF00u;
    assert(snapshot_textures());
    assert(sample_texture(0,0,&pixel) && pixel==0x80FF0000u);
    assert(sample_stage(1,0,0,&pixel) && pixel==0xFF00FF00u);
    assert(sample_texture(0,0,&pixel) && pixel==0x80FF0000u);
    raster_triangle(a,b,c,0xFFFFFFFFu,uv);
    assert(fb[0]==0xFF0000FFu); /* Writes remain in host staging before flush. */
    assert(s_surface_first[0]==0 && s_surface_end[0]>0);
    assert(surface_stage_flush(guest));surface_stage_reset();
    assert((fb[0]&0x00FFFFFFu)==0x0080007Fu);
    assert(fb[15]==0xFF0000FFu); /* Outside triangle remains unchanged. */
    /* DXT1 red block and indexed Morton layout use staged host data too. */
    s_gpu.tex.color=0x0C;
    memset(tex,0,8);((uint16_t *)tex)[0]=0xF800;
    assert(snapshot_textures());
    assert(sample_texture(0,0,&pixel) && pixel==0xFFFF0000u);
    s_gpu.tex.color=0x0B;s_gpu.tex.width=s_gpu.tex.height=2;
    s_gpu.tex.palette=(uint32_t)(page*4);
    unsigned char *indices=(unsigned char *)tex;
    for(int i=0;i<4;i++)indices[i]=(unsigned char)i;
    uint32_t *palette=(uint32_t *)(guest+page*4);
    palette[0]=0xFF000000u;palette[1]=0xFF00FF00u;
    palette[2]=0xFF0000FFu;palette[3]=0xFFFFFFFFu;
    assert(snapshot_textures());
    assert(sample_texture(1,0,&pixel) && pixel==palette[1]);
    assert(mprotect(guest+page*4,page,PROT_NONE)==0);
    assert(!snapshot_textures());
    assert(mprotect(guest+page*4,page,PROT_READ|PROT_WRITE)==0);
    s_gpu.tex.color=0x12;s_gpu.tex.width=s_gpu.tex.height=4;
    s_gpu.tex.palette=0;
    assert(mprotect(guest+page*3,page,PROT_NONE)==0);
    s_gpu.tex.offset=(uint32_t)(page*3);assert(!snapshot_textures());
    assert(!sample_texture(0,0,&pixel));
    s_gpu.tex.offset=(uint32_t)(page*3-8);assert(!snapshot_textures());
    s_gpu.color_offset=(uint32_t)(page*3);assert(!surface_stage_begin(guest));
    s_gpu.color_offset=(uint32_t)(page*3-32);assert(!surface_stage_begin(guest));
    s_gpu.color_offset=(uint32_t)page;
    assert(mprotect(guest+page,page,PROT_READ)==0);
    assert(surface_stage_begin(guest));put_pixel(guest,4,0,0,0xFFFFFFFFu);
    assert(!surface_stage_flush(guest));surface_stage_reset();
    assert(mprotect(guest+page,page,PROT_READ|PROT_WRITE)==0);
    VertexAttr attr={0};attr.type=2;attr.size=4;attr.stride=16;
    s_gpu.inline_active=1;s_gpu.inline_count=1;
    assert(!fetch_attr(&attr,0,out));
    s_gpu.inline_count=4;
    float values[4]={1,2,3,4};memcpy(s_gpu.inline_buf,values,sizeof values);
    assert(fetch_attr(&attr,0,out) && out[3]==4);
    s_gpu.inline_active=0;attr.offset=UINT32_MAX-7;
    assert(!fetch_attr(&attr,1,out));
    attr.offset=(uint32_t)(page*3);assert(!fetch_attr(&attr,0,out));
    attr.offset=(uint32_t)(page*2);memcpy(tex,values,sizeof values);
    assert(fetch_attr(&attr,0,out) && out[2]==3);
    for (int i=0;i<4;i++) free(s_texture_snapshot[i].pixels);
    assert(munmap(guest,page*6)==0);
    return 0;
}
