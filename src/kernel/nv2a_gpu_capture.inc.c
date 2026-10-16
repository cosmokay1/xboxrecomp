/* Opt-in, one-frame renderer inspection for the shadow experiment. No guest
 * memory writes or application callbacks. Included after the vertex helpers. */
#include <float.h>
static struct {
    int mode; /* 0 unparsed, 1 disabled, 2 waiting, 3 recording, 4 finished */
    char prefix[768];
    FILE *file;
    uint32_t frame, checked_frame, draw, draws, triangles, samples;
    int truncated, batch3d;
    GpuSurface *surface;
} s_capture;
static void gpu_capture_float(float value) {
    if (_finite(value)) fprintf(s_capture.file,"%.9g",(double)value);
    else fputs("null",s_capture.file);
}
static void gpu_capture_vector(const float *value,int count) {
    int k;fputc('[',s_capture.file);
    for(k=0;k<count;k++){if(k)fputc(',',s_capture.file);gpu_capture_float(value[k]);}
    fputc(']',s_capture.file);
}
static void gpu_capture_finish(void) {
    char path[800];FILE *image;int index;
    GpuSurface *surface=NULL;
    if(s_capture.mode!=3)return;
    for(index=0;index<GPU_MAX_SURFACES;index++)
        if(s_gpu11.surf[index].tex && s_gpu11.surf[index].guest==dma_resolve(s_gpu.color_offset)) {
            surface=&s_gpu11.surf[index];break;
        }
    /* Copy only the selected render target, not guest memory. This readback is
     * deliberately diagnostic: one frame, bounded dimensions, no recurring cost. */
    if(surface && surface->tex) {
        D3D11_TEXTURE2D_DESC desc;
        ID3D11Texture2D_GetDesc(surface->tex,&desc);
        if(desc.Width<=4096 && desc.Height<=4096 && desc.Format==DXGI_FORMAT_B8G8R8A8_UNORM && desc.SampleDesc.Count==1) {
            ID3D11Texture2D *staging=NULL;
            desc.Usage=D3D11_USAGE_STAGING;desc.BindFlags=0;desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;desc.MiscFlags=0;
            if(SUCCEEDED(ID3D11Device_CreateTexture2D(s_gpu11.dev,&desc,NULL,&staging))) {
                D3D11_MAPPED_SUBRESOURCE mapped;
                ID3D11DeviceContext_CopyResource(s_gpu11.ctx,(ID3D11Resource*)staging,(ID3D11Resource*)surface->tex);
                if(SUCCEEDED(ID3D11DeviceContext_Map(s_gpu11.ctx,(ID3D11Resource*)staging,0,D3D11_MAP_READ,0,&mapped))) {
                    snprintf(path,sizeof(path),"%s.ppm",s_capture.prefix);image=fopen(path,"wbx");
                    if(image) {
                        UINT x,y;fprintf(image,"P6\n%u %u\n255\n",desc.Width,desc.Height);
                        for(y=0;y<desc.Height;y++)for(x=0;x<desc.Width;x++) {
                            const unsigned char *p=(const unsigned char*)mapped.pData+y*mapped.RowPitch+x*4;
                            const unsigned char rgb[3]={p[2],p[1],p[0]};fwrite(rgb,1,3,image);
                        }
                        if(fclose(image))s_capture.truncated=1;
                    } else s_capture.truncated=1;
                    ID3D11DeviceContext_Unmap(s_gpu11.ctx,(ID3D11Resource*)staging,0);
                } else s_capture.truncated=1;
                ID3D11Texture2D_Release(staging);
            } else s_capture.truncated=1;
        } else s_capture.truncated=1;
    } else s_capture.truncated=1;
    fprintf(s_capture.file,"{\"type\":\"end\",\"draws\":%u,\"triangles\":%u,\"truncated\":%s,\"present_target\":%u}\n",
        s_capture.draws,s_capture.triangles,s_capture.truncated?"true":"false",surface?surface->guest:0);
    if(fclose(s_capture.file))s_capture.truncated=1;
    s_capture.file=NULL;s_capture.mode=4;
    fprintf(stderr,"[SHADOW-CAPTURE] frame=%u draws=%u triangles=%u complete=%d\n",
        s_capture.frame,s_capture.draws,s_capture.triangles,!s_capture.truncated);
}
static int gpu_capture_begin(int flat2d,GpuSurface *surface) {
    char path[800];
    s_capture.batch3d=!flat2d;
    if(!s_capture.mode) {
        const char *prefix=env_cached("RECOMP_SHADOW_CAPTURE");
        s_capture.mode=1;
        if(prefix && strlen(prefix)>3 && strlen(prefix)<sizeof(s_capture.prefix) &&
           ((prefix[1]==':' && (prefix[2]=='\\' || prefix[2]=='/')) || prefix[0]=='/')) {
            strcpy(s_capture.prefix,prefix);s_capture.mode=2;s_capture.checked_frame=0xFFFFFFFFu;
        }
    }
    if(flat2d || s_capture.mode==1 || s_capture.mode==4)return 0;
    if(s_capture.mode==2) {
        if(s_capture.checked_frame==s_gpu11.frame)return 0;
        s_capture.checked_frame=s_gpu11.frame;
        snprintf(path,sizeof(path),"%s.request",s_capture.prefix);
        if(GetFileAttributesA(path)==INVALID_FILE_ATTRIBUTES)return 0;
        snprintf(path,sizeof(path),"%s.jsonl",s_capture.prefix);
        s_capture.file=fopen(path,"wx");
        if(!s_capture.file){s_capture.mode=4;fprintf(stderr,"[SHADOW-CAPTURE] Refusing existing/unwritable capture.\n");return 0;}
        snprintf(path,sizeof(path),"%s.request",s_capture.prefix);DeleteFileA(path);
        s_capture.frame=s_gpu11.frame;s_capture.mode=3;s_capture.draw=0xFFFFFFFFu;s_capture.surface=surface;
        fprintf(s_capture.file,"{\"type\":\"header\",\"schema\":1,\"frame\":%u,\"width\":%u,\"height\":%u,\"scale\":%u,\"extra_x\":%u}\n",
            s_capture.frame,surface->w,surface->h,surface->scale,surface->ox);
    }
    s_capture.surface=surface;
    return s_capture.mode==3;
}
static void gpu_capture_triangle(const XfVert *a,const XfVert *b,const XfVert *c,uint32_t ia,uint32_t ib,uint32_t ic) {
    const XfVert *vertices[3]={a,b,c};uint32_t indices[3]={ia,ib,ic};int v,k;
    if(s_capture.mode!=3 || !s_capture.batch3d || !a->ok || !b->ok || !c->ok)return;
    if(ftell(s_capture.file)>16*1024*1024L || s_capture.draws>=2048){s_capture.truncated=1;return;}
    if(s_capture.draw!=s_gpu.draws) {
        s_capture.draw=s_gpu.draws;s_capture.samples=0;s_capture.draws++;
        fprintf(s_capture.file,"{\"type\":\"draw\",\"draw\":%u,\"texture\":%u,\"program\":%u,\"depth_test\":%u,\"depth_write\":%u,\"target\":%u,\"width\":%u,\"height\":%u,\"scale\":%u,\"extra_x\":%u,\"constants\":[",
            s_gpu.draws,s_gpu.tex.valid?s_gpu.tex.offset:0,s_vp.start,s_vp.depth_test,s_vp.depth_mask,
            s_capture.surface->guest,s_capture.surface->w,s_capture.surface->h,s_capture.surface->scale,s_capture.surface->ox);
        for(k=0;k<192;k++){if(k)fputc(',',s_capture.file);gpu_capture_vector(s_vp.c[k],4);}
        fputs("]}\n",s_capture.file);
    }
    if(s_capture.samples>=4)return;
    fprintf(s_capture.file,"{\"type\":\"triangle\",\"draw\":%u,\"vertices\":[",s_gpu.draws);
    for(v=0;v<3;v++) {
        float raw[4],screen[4];const XfVert *x=vertices[v];
        fetch_attr(&s_gpu.attr[0],indices[v],raw);
        screen[0]=x->x;screen[1]=x->y;screen[2]=x->z;screen[3]=x->w;
        if(v)fputc(',',s_capture.file);
        fputs("{\"input\":",s_capture.file);gpu_capture_vector(raw,4);
        fputs(",\"screen\":",s_capture.file);gpu_capture_vector(screen,4);
        fputs(",\"uv\":",s_capture.file);gpu_capture_vector(x->tc[0],4);fputc('}',s_capture.file);
    }
    fputs("]}\n",s_capture.file);s_capture.samples++;s_capture.triangles++;
}
