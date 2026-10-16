/* Isolated shadow POC: consumes a hardware-traced, fixed-light box mask.
 * Disabled unless RECOMP_SHADOW_BOX names a 256x256 uint32 mask. No guest writes.
 * This is a precomputed DXR mask, not a per-frame ray-tracing renderer. */
#include "nv2a_shadow_live.h"
typedef struct {float pos[4],uv[2],color[4];} ShadowBoxVertex;
static struct {
    int state,logged,mode,key_down;uint32_t frame;
    ID3D11VertexShader *vs;ID3D11PixelShader *ps;ID3D11InputLayout *layout;
    ID3D11Buffer *vb;ID3D11ShaderResourceView *mask;
    ID3D11Texture2D *texture;
    HANDLE mapping,mutex;ShadowLiveFrame *shared;
    unsigned int sequence,updates;unsigned long long tick,attempt;
    float light_x;double upload_ms,max_upload_ms;
    ID3D11BlendState *blend;ID3D11DepthStencilState *depth,*ground;
    ID3D11RasterizerState *raster;
} s_shadow_box;
static int gpu_shadow_box_init(void) {
    const char *path;FILE *f;uint32_t mask[256*256];ID3DBlob *vs=NULL,*ps=NULL,*error=NULL;
    ID3D11Texture2D *tex=NULL;D3D11_TEXTURE2D_DESC td={0};D3D11_SUBRESOURCE_DATA data={0};
    D3D11_BUFFER_DESC bd={0};D3D11_BLEND_DESC blend={0};D3D11_DEPTH_STENCIL_DESC depth={0};
    D3D11_RASTERIZER_DESC raster={0};HRESULT hr=E_FAIL;
    D3D11_INPUT_ELEMENT_DESC elements[]={
        {"POSITION",0,DXGI_FORMAT_R32G32B32A32_FLOAT,0,0,D3D11_INPUT_PER_VERTEX_DATA,0},
        {"TEXCOORD",0,DXGI_FORMAT_R32G32_FLOAT,0,16,D3D11_INPUT_PER_VERTEX_DATA,0},
        {"COLOR",0,DXGI_FORMAT_R32G32B32A32_FLOAT,0,24,D3D11_INPUT_PER_VERTEX_DATA,0}};
    const char *shader="struct V{float4 p:POSITION;float2 uv:TEXCOORD;float4 c:COLOR;};"
        "struct O{float4 p:SV_POSITION;float2 uv:TEXCOORD;float4 c:COLOR;};"
        "O vs_main(V v){O o;o.p=v.p;o.uv=v.uv;o.c=v.c;return o;}"
        "Texture2D<uint> mask:register(t0);"
        "float4 ps_main(O v):SV_TARGET{if(v.c.a<0){uint2 p=min(uint2(saturate(v.uv)*256),255);"
        "return float4(0,0,0,mask.Load(int3(p,0))*.60);}return v.c;}";
    if(s_shadow_box.state)return s_shadow_box.state==1;
    s_shadow_box.state=-1;path=env_cached("RECOMP_SHADOW_BOX");if(!path)return 0;
    f=fopen(path,"rb");if(!f){fprintf(stderr,"[SHADOW-BOX] Mask missing; disabled.\n");return 0;}
    if(fread(mask,sizeof(mask),1,f)!=1 || fgetc(f)!=EOF){fclose(f);return 0;}fclose(f);
    {unsigned i;for(i=0;i<256*256;i++)if(mask[i]>1)return 0;}
#define SHADOW_OK(call) do{hr=(call);if(FAILED(hr))goto done;}while(0)
    SHADOW_OK(D3DCompile(shader,strlen(shader),"shadow-box",NULL,NULL,"vs_main","vs_4_0",0,0,&vs,&error));
    if(error){ID3D10Blob_Release(error);error=NULL;}
    SHADOW_OK(D3DCompile(shader,strlen(shader),"shadow-box",NULL,NULL,"ps_main","ps_4_0",0,0,&ps,&error));
    SHADOW_OK(ID3D11Device_CreateVertexShader(s_gpu11.dev,ID3D10Blob_GetBufferPointer(vs),ID3D10Blob_GetBufferSize(vs),NULL,&s_shadow_box.vs));
    SHADOW_OK(ID3D11Device_CreatePixelShader(s_gpu11.dev,ID3D10Blob_GetBufferPointer(ps),ID3D10Blob_GetBufferSize(ps),NULL,&s_shadow_box.ps));
    SHADOW_OK(ID3D11Device_CreateInputLayout(s_gpu11.dev,elements,3,ID3D10Blob_GetBufferPointer(vs),ID3D10Blob_GetBufferSize(vs),&s_shadow_box.layout));
    td.Width=td.Height=256;td.MipLevels=td.ArraySize=1;td.Format=DXGI_FORMAT_R32_UINT;td.SampleDesc.Count=1;
    td.Usage=D3D11_USAGE_DEFAULT;td.BindFlags=D3D11_BIND_SHADER_RESOURCE;data.pSysMem=mask;data.SysMemPitch=256*4;
    SHADOW_OK(ID3D11Device_CreateTexture2D(s_gpu11.dev,&td,&data,&tex));
    SHADOW_OK(ID3D11Device_CreateShaderResourceView(s_gpu11.dev,(ID3D11Resource*)tex,NULL,&s_shadow_box.mask));
    bd.ByteWidth=42*sizeof(ShadowBoxVertex);bd.Usage=D3D11_USAGE_DYNAMIC;bd.BindFlags=D3D11_BIND_VERTEX_BUFFER;bd.CPUAccessFlags=D3D11_CPU_ACCESS_WRITE;
    SHADOW_OK(ID3D11Device_CreateBuffer(s_gpu11.dev,&bd,NULL,&s_shadow_box.vb));
    blend.RenderTarget[0].BlendEnable=TRUE;blend.RenderTarget[0].SrcBlend=D3D11_BLEND_SRC_ALPHA;blend.RenderTarget[0].DestBlend=D3D11_BLEND_INV_SRC_ALPHA;
    blend.RenderTarget[0].BlendOp=blend.RenderTarget[0].BlendOpAlpha=D3D11_BLEND_OP_ADD;
    blend.RenderTarget[0].SrcBlendAlpha=D3D11_BLEND_ONE;blend.RenderTarget[0].DestBlendAlpha=D3D11_BLEND_ZERO;blend.RenderTarget[0].RenderTargetWriteMask=15;
    SHADOW_OK(ID3D11Device_CreateBlendState(s_gpu11.dev,&blend,&s_shadow_box.blend));
    depth.DepthEnable=TRUE;depth.DepthWriteMask=D3D11_DEPTH_WRITE_MASK_ALL;depth.DepthFunc=D3D11_COMPARISON_LESS_EQUAL;
    SHADOW_OK(ID3D11Device_CreateDepthStencilState(s_gpu11.dev,&depth,&s_shadow_box.depth));
    depth.DepthWriteMask=D3D11_DEPTH_WRITE_MASK_ZERO;
    SHADOW_OK(ID3D11Device_CreateDepthStencilState(s_gpu11.dev,&depth,&s_shadow_box.ground));
    raster.FillMode=D3D11_FILL_SOLID;raster.CullMode=D3D11_CULL_NONE;raster.DepthClipEnable=TRUE;
    SHADOW_OK(ID3D11Device_CreateRasterizerState(s_gpu11.dev,&raster,&s_shadow_box.raster));
    s_shadow_box.texture=tex;tex=NULL;
    s_shadow_box.state=1;s_shadow_box.mode=1;
done:
    if(vs)ID3D10Blob_Release(vs);if(ps)ID3D10Blob_Release(ps);if(error)ID3D10Blob_Release(error);if(tex)ID3D11Texture2D_Release(tex);
    if(s_shadow_box.state!=1)fprintf(stderr,"[SHADOW-BOX] Initialization failed %08lX; disabled.\n",(unsigned long)hr);
#undef SHADOW_OK
    return s_shadow_box.state==1;
}
/* D3D12 fence waits stay in the worker process. A busy mutex or stale producer
 * never blocks the render thread; stale live shadows disappear after 2 seconds.
 * This intentionally measures a CPU readback/upload transport before attempting
 * D3D11/D3D12 shared GPU resources under Proton. */
static int gpu_shadow_box_live(void) {
    const char *name=env_cached("RECOMP_SHADOW_LIVE");unsigned long long now=GetTickCount64();
    static unsigned int copy[SHADOW_LIVE_PIXELS];
    if(!name)return 1;
    if(!s_shadow_box.shared && now-s_shadow_box.attempt>1000) {
        char mutex_name[160];s_shadow_box.attempt=now;
        if(strlen(name)>120)return 0;
        s_shadow_box.mapping=OpenFileMappingA(FILE_MAP_READ,FALSE,name);
        if(s_shadow_box.mapping){
            snprintf(mutex_name,sizeof(mutex_name),"%s-mutex",name);
            s_shadow_box.mutex=OpenMutexA(SYNCHRONIZE|MUTEX_MODIFY_STATE,FALSE,mutex_name);
            if(s_shadow_box.mutex)s_shadow_box.shared=(ShadowLiveFrame*)MapViewOfFile(s_shadow_box.mapping,FILE_MAP_READ,0,0,sizeof(ShadowLiveFrame));
            if(!s_shadow_box.shared){if(s_shadow_box.mutex)CloseHandle(s_shadow_box.mutex);CloseHandle(s_shadow_box.mapping);s_shadow_box.mutex=NULL;s_shadow_box.mapping=NULL;}
        }
    }
    if(s_shadow_box.shared && WaitForSingleObject(s_shadow_box.mutex,0)==WAIT_OBJECT_0) {
        ShadowLiveFrame *frame=s_shadow_box.shared;int changed=0;
        LARGE_INTEGER begin,end,frequency;
        QueryPerformanceCounter(&begin);
        if(frame->magic==SHADOW_LIVE_MAGIC && frame->sequence!=s_shadow_box.sequence && frame->tick<=now && now-frame->tick<2000) {
            memcpy(copy,frame->mask,sizeof(copy));s_shadow_box.sequence=frame->sequence;s_shadow_box.tick=frame->tick;s_shadow_box.light_x=frame->light[0];changed=1;
        }
        ReleaseMutex(s_shadow_box.mutex);
        if(changed) {
            ID3D11DeviceContext_UpdateSubresource(s_gpu11.ctx,(ID3D11Resource*)s_shadow_box.texture,0,NULL,copy,256*4,0);
            QueryPerformanceCounter(&end);QueryPerformanceFrequency(&frequency);
            {double ms=(double)(end.QuadPart-begin.QuadPart)*1000/(double)frequency.QuadPart;
                s_shadow_box.upload_ms+=ms;if(ms>s_shadow_box.max_upload_ms)s_shadow_box.max_upload_ms=ms;}
            ++s_shadow_box.updates;
            if(s_shadow_box.updates==1 || s_shadow_box.updates%30==0)
                fprintf(stderr,"[SHADOW-LIVE] sequence=%u light_x=%.3f updates=%u cpu_copy_upload_avg_ms=%.3f max_ms=%.3f\n",
                    s_shadow_box.sequence,(double)s_shadow_box.light_x,s_shadow_box.updates,s_shadow_box.upload_ms/s_shadow_box.updates,s_shadow_box.max_upload_ms);
        }
    }
    return s_shadow_box.tick && now>=s_shadow_box.tick && now-s_shadow_box.tick<2000;
}
static void gpu_shadow_box_vertex(ShadowBoxVertex *v,float x,float y,float z,float u,float t,int face,GpuSurface *surface) {
    float world[4]={x,y,z,1},q[4]={0};int i,k;
    for(i=0;i<4;i++)for(k=0;k<4;k++)q[i]+=s_vp.c[i][k]*world[k];
    v->pos[0]=(2*(q[0]+surface->ox*q[3])/(surface->w+2*surface->ox))-q[3];
    v->pos[1]=q[3]-2*q[1]/surface->h;v->pos[2]=q[2]/gpu_zmax();v->pos[3]=q[3];
    v->uv[0]=u;v->uv[1]=t;
    v->color[0]=.15f+(float)face*.035f;v->color[1]=.45f+(float)face*.04f;v->color[2]=.85f;v->color[3]=face<0?-1:1;
}
static void gpu_shadow_box_draw(int flat2d,GpuSurface *surface) {
    float lo[3]={FLT_MAX,FLT_MAX,FLT_MAX},hi[3]={-FLT_MAX,-FLT_MAX,-FLT_MAX};
    int live_ready;
    unsigned i,k,count=s_gpu.idx_count<12?s_gpu.idx_count:12;
    ShadowBoxVertex vertices[42];D3D11_MAPPED_SUBRESOURCE mapped;UINT stride=sizeof(ShadowBoxVertex),offset=0;
    D3D11_VIEWPORT viewport={0};
    const float corners[8][3]={{-73,7,-73},{73,7,-73},{73,147,-73},{-73,147,-73},{-73,7,73},{73,7,73},{73,147,73},{-73,147,73}};
    const unsigned box[]={0,1,2,0,2,3,4,6,5,4,7,6,0,4,5,0,5,1,3,2,6,3,6,7,0,3,7,0,7,4,1,5,6,1,6,2};
    const float ground[6][2]={{-400,-400},{400,-400},{400,400},{-400,-400},{400,400},{-400,400}};
    if(flat2d || !env_cached("RECOMP_SHADOW_BOX") || s_shadow_box.frame==s_gpu11.frame || count<6 || !s_vp.depth_test || s_vp.depth_mask)return;
    /* Captured full-field receiver: centimetre scale, Y=0, world-space input.
     * Fail closed on other geometry/cameras; never use a texture address. */
    for(i=0;i<count;i++){float v[4];fetch_attr(&s_gpu.attr[0],s_gpu.idx[i],v);if(!_finite(v[0]) || !_finite(v[1]) || !_finite(v[2]) || fabsf(v[3]-1)>1e-5f)return;
        for(k=0;k<3;k++){if(v[k]<lo[k])lo[k]=v[k];if(v[k]>hi[k])hi[k]=v[k];}}
    if(fabsf(lo[1])>.01f || fabsf(hi[1])>.01f || hi[0]-lo[0]<5000 || hi[2]-lo[2]<10000 ||
        fabsf(s_vp.c[3][0])+fabsf(s_vp.c[3][2])<.1f)return;
    if(!gpu_shadow_box_init())return;
    s_shadow_box.frame=s_gpu11.frame;
    {int down=(GetAsyncKeyState(VK_F8)&0x8000)!=0;
        if(down && !s_shadow_box.key_down){s_shadow_box.mode=(s_shadow_box.mode+1)%3;
            fprintf(stderr,"[SHADOW-BOX] F8 mode=%d (0 off, 1 box+shadow, 2 box only)\n",s_shadow_box.mode);}
        s_shadow_box.key_down=down;}
    if(!s_shadow_box.mode)return;
    live_ready=gpu_shadow_box_live();
    for(i=0;i<6;i++)gpu_shadow_box_vertex(&vertices[i],ground[i][0],.1f,2000+ground[i][1],(ground[i][0]+400)/800,(ground[i][1]+400)/800,-1,surface);
    for(i=0;i<36;i++){const float *v=corners[box[i]];gpu_shadow_box_vertex(&vertices[i+6],v[0],v[1],v[2]+2000,0,0,(int)(i/6),surface);}
    if(FAILED(ID3D11DeviceContext_Map(s_gpu11.ctx,(ID3D11Resource*)s_shadow_box.vb,0,D3D11_MAP_WRITE_DISCARD,0,&mapped)))return;
    memcpy(mapped.pData,vertices,sizeof(vertices));ID3D11DeviceContext_Unmap(s_gpu11.ctx,(ID3D11Resource*)s_shadow_box.vb,0);
    ID3D11DeviceContext_OMSetRenderTargets(s_gpu11.ctx,1,&surface->rtv,surface->dsv);
    ID3D11DeviceContext_IASetInputLayout(s_gpu11.ctx,s_shadow_box.layout);
    ID3D11DeviceContext_IASetVertexBuffers(s_gpu11.ctx,0,1,&s_shadow_box.vb,&stride,&offset);
    ID3D11DeviceContext_IASetPrimitiveTopology(s_gpu11.ctx,D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11DeviceContext_VSSetShader(s_gpu11.ctx,s_shadow_box.vs,NULL,0);ID3D11DeviceContext_GSSetShader(s_gpu11.ctx,NULL,NULL,0);
    ID3D11DeviceContext_PSSetShader(s_gpu11.ctx,s_shadow_box.ps,NULL,0);ID3D11DeviceContext_PSSetShaderResources(s_gpu11.ctx,0,1,&s_shadow_box.mask);
    ID3D11DeviceContext_RSSetState(s_gpu11.ctx,s_shadow_box.raster);
    viewport.Width=(float)((surface->w+2*surface->ox)*surface->scale);viewport.Height=(float)(surface->h*surface->scale);viewport.MaxDepth=1;
    ID3D11DeviceContext_RSSetViewports(s_gpu11.ctx,1,&viewport);ID3D11DeviceContext_OMSetBlendState(s_gpu11.ctx,s_shadow_box.blend,NULL,~0u);
    ID3D11DeviceContext_OMSetDepthStencilState(s_gpu11.ctx,s_shadow_box.ground,0);if(s_shadow_box.mode==1 && live_ready)ID3D11DeviceContext_Draw(s_gpu11.ctx,6,0);
    ID3D11DeviceContext_OMSetDepthStencilState(s_gpu11.ctx,s_shadow_box.depth,0);ID3D11DeviceContext_Draw(s_gpu11.ctx,36,6);
    s_bound.valid=0;s_shadow_box.frame=s_gpu11.frame;
    if(!s_shadow_box.logged){fprintf(stderr,"[SHADOW-BOX] Fixed DXR mask + box drawn on receiver; frame=%u target=%08X\n",s_gpu11.frame,surface->guest);s_shadow_box.logged=1;}
}
