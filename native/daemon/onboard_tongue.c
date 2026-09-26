#define _POSIX_C_SOURCE 200809L
#include "onboard_tongue.h"

#include <dlfcn.h>
#include <errno.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>

typedef int (*hta_init_fn)(uint32_t *);
typedef int (*hta_prepare_fn)(uint32_t);
typedef int (*hta_teardown_fn)(uint32_t);
typedef int (*hta_append_const_fn)(uint32_t, uint32_t, uint32_t, uint32_t,
                                   uint32_t, uint32_t, const void *, uint32_t);
typedef struct { uint32_t src_id, output_idx; } hta_input;
typedef struct {
  uint32_t rank, max_sizes[8], elem_size;
  int32_t zero_offset;
  float stepsize;
} hta_output;
typedef struct {
  uint32_t batches, height, width, depth;
  void *data;
  uint32_t data_len, data_valid_len, unused;
} hta_tensordef;
typedef int (*hta_append_node_fn)(uint32_t, uint32_t, uint32_t, uint32_t,
                                  const hta_input *, uint32_t,
                                  const hta_output *, uint32_t);
typedef int (*hta_execute_fn)(uint32_t, const hta_tensordef *, uint32_t,
                              hta_tensordef *, uint32_t);
_Static_assert(sizeof(hta_input) == 8, "hta_input size");
_Static_assert(sizeof(hta_output) == 48, "hta_output size");
_Static_assert(sizeof(hta_tensordef) == 40, "hta_tensordef size");

typedef struct { uint32_t id, b, h, w, d, off, len; } CRec;
typedef struct {
  uint32_t id, op, pad, ni, no;
  hta_input *in;
  hta_output *out;
} NRec;
typedef struct { uint32_t b, h, w, d, e; } Tensor;
typedef struct {
  uint32_t nc, nn, ni, no, bo, bs;
  CRec *c;
  NRec *n;
  Tensor *in, *out;
} Graph;
typedef struct {
  uint8_t *file;
  size_t file_size;
  Graph graph;
  uint32_t id;
  int live;
  hta_tensordef input, output;
  uint8_t *input_data, *output_data;
  uint32_t input_size, output_size;
} TongueGraph;
typedef struct {
  float encoder_min, encoder_max;
  float fusion_in_min, fusion_in_max;
  float fusion_out_min, fusion_out_max;
  char names[TONGUE_MAX_TARGETS][128];
  uint32_t targets;
  uint32_t image_size;
} ModelParams;
typedef struct {
  float *w0, *b0, *w1, *b1, *w2, *b2;
  uint8_t signed_mask[TONGUE_MAX_TARGETS];
  uint32_t inputs, hidden0, hidden1, outputs;
} Head;
typedef struct {
  uint32_t h, w, c, h2, w2, f;
} ModelDims;

struct OnboardTongue {
  void *library;
  hta_init_fn init;
  hta_prepare_fn prepare;
  hta_teardown_fn teardown;
  hta_append_const_fn append_const;
  hta_append_node_fn append_node;
  hta_execute_fn execute;
  TongueGraph graphs[4];
  ModelParams params[2];
  Head heads[2];
  ModelDims dims[2];
  float *head_storage[2];
  uint8_t *bridge;
  float *features, *h0, *h1;
  char names[TONGUE_MAX_TARGETS][128];
  uint32_t targets;
  uint32_t image_size;
  int single;
};

static double now_ms(void) {
  struct timespec t;
  if (clock_gettime(CLOCK_MONOTONIC, &t) != 0) return 0.0;
  return (double)t.tv_sec * 1000.0 + (double)t.tv_nsec / 1000000.0;
}
static uint32_t u32(const uint8_t *p) {
  return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 |
         (uint32_t)p[3] << 24;
}
static int fits(size_t o, size_t n, size_t z) { return o <= z && n <= z - o; }
static int tensor_bytes(const Tensor *t, uint32_t *bytes) {
  uint32_t dims[5] = {t->b, t->h, t->w, t->d, t->e};
  uint32_t n = 1;
  for (unsigned i = 0; i < 5; i++) {
    if (!dims[i] || n > UINT32_MAX / dims[i]) return 0;
    n *= dims[i];
  }
  *bytes = n;
  return 1;
}
static void graph_free(Graph *g) {
  uint32_t i;
  if (g->n) for (i = 0; i < g->nn; i++) { free(g->n[i].in); free(g->n[i].out); }
  free(g->c); free(g->n); free(g->in); free(g->out);
  memset(g, 0, sizeof(*g));
}
static int graph_validate(Graph *g, size_t size, char *err, size_t cap,
                          const char *path) {
  uint32_t i, j;
  if (g->bo % 16 || !fits(g->bo, g->bs, size)) {
    snprintf(err, cap, "%s: invalid blob offset, size, or alignment", path); return 0;
  }
  for (i = 0; i < g->nc; i++) {
    CRec *c = &g->c[i];
    if (!c->id || c->id >= 0x10000000 || !fits(c->off, c->len, g->bs)) {
      snprintf(err, cap, "%s: const record %u has invalid id/range", path, i); return 0;
    }
    for (j = 0; j < i; j++) if (g->c[j].id == c->id) {
      snprintf(err, cap, "%s: duplicate const id in record %u", path, i); return 0;
    }
    for (j = 0; j < g->nn; j++) if (g->n[j].id == c->id) {
      snprintf(err, cap, "%s: duplicate id in const record %u", path, i); return 0;
    }
  }
  for (i = 0; i < g->nn; i++) {
    NRec *n = &g->n[i];
    if (!n->id || n->id >= 0x10000000) {
      snprintf(err, cap, "%s: invalid node id in record %u", path, i); return 0;
    }
    for (j = 0; j < i; j++) if (g->n[j].id == n->id) {
      snprintf(err, cap, "%s: duplicate node id in record %u", path, i); return 0;
    }
    for (j = 0; j < n->ni; j++) {
      uint32_t k; int found = 0;
      for (k = 0; k < g->nc; k++) found |= g->c[k].id == n->in[j].src_id;
      for (k = 0; k < g->nn; k++) found |= g->n[k].id == n->in[j].src_id;
      if (!found) { snprintf(err, cap, "%s: missing node source id", path); return 0; }
    }
  }
  for (i = 0; i < g->ni; i++) {
    uint32_t b;
    if (g->in[i].e != 1 || !tensor_bytes(&g->in[i], &b)) {
      snprintf(err, cap, "%s: invalid input tensor %u", path, i); return 0;
    }
  }
  for (i = 0; i < g->no; i++) {
    uint32_t b;
    if (g->out[i].e != 1 || !tensor_bytes(&g->out[i], &b)) {
      snprintf(err, cap, "%s: invalid output tensor %u", path, i); return 0;
    }
  }
  return 1;
}
static int graph_parse(const uint8_t *b, size_t z, Graph *g, char *err,
                       size_t cap, const char *path) {
  size_t p = 32; uint32_t i;
  memset(g, 0, sizeof(*g));
  if (z < 32 || memcmp(b, "HTAG", 4)) { snprintf(err, cap, "%s: invalid/truncated HTAG header or magic", path); return 0; }
  if (u32(b + 4) != 1) { snprintf(err, cap, "%s: unsupported HTAG version", path); return 0; }
  g->nc=u32(b+8); g->nn=u32(b+12); g->ni=u32(b+16); g->no=u32(b+20); g->bo=u32(b+24); g->bs=u32(b+28);
  if ((uint64_t)p + (uint64_t)g->nc * 32 > z) { snprintf(err,cap,"%s: const table exceeds file",path); return 0; }
  g->c=calloc(g->nc?g->nc:1,sizeof(*g->c)); g->n=calloc(g->nn?g->nn:1,sizeof(*g->n));
  g->in=calloc(g->ni?g->ni:1,sizeof(*g->in)); g->out=calloc(g->no?g->no:1,sizeof(*g->out));
  if (!g->c || !g->n || !g->in || !g->out) { snprintf(err,cap,"%s: out of memory parsing graph",path); graph_free(g); return 0; }
  for (i=0;i<g->nc;i++,p+=32) {
    const uint8_t *q=b+p;
    g->c[i]=(CRec){u32(q),u32(q+4),u32(q+8),u32(q+12),u32(q+16),u32(q+20),u32(q+24)};
    if(u32(q+28)){snprintf(err,cap,"%s: const reserved field nonzero",path);graph_free(g);return 0;}
  }
  for(i=0;i<g->nn;i++) {
    NRec *n=&g->n[i]; uint32_t j;
    if(!fits(p,20,z)){snprintf(err,cap,"%s: truncated node header",path);graph_free(g);return 0;}
    n->id=u32(b+p);n->op=u32(b+p+4);n->pad=u32(b+p+8);n->ni=u32(b+p+12);n->no=u32(b+p+16);p+=20;
    if(n->ni>64||n->no>64||!fits(p,(size_t)n->ni*8+(size_t)n->no*48,z)){snprintf(err,cap,"%s: invalid node record size/count",path);graph_free(g);return 0;}
    n->in=calloc(n->ni?n->ni:1,sizeof(*n->in));n->out=calloc(n->no?n->no:1,sizeof(*n->out));
    if(!n->in||!n->out){snprintf(err,cap,"%s: out of memory parsing node",path);graph_free(g);return 0;}
    for(j=0;j<n->ni;j++,p+=8)n->in[j]=(hta_input){u32(b+p),u32(b+p+4)};
    for(j=0;j<n->no;j++,p+=48){unsigned k;const uint8_t*q=b+p;n->out[j].rank=u32(q);for(k=0;k<8;k++)n->out[j].max_sizes[k]=u32(q+4+4*k);n->out[j].elem_size=u32(q+36);n->out[j].zero_offset=(int32_t)u32(q+40);memcpy(&n->out[j].stepsize,q+44,4);}
  }
  for(uint64_t ti=0;ti<(uint64_t)g->ni+g->no;ti++) {
    Tensor*t=ti<g->ni?&g->in[ti]:&g->out[ti-g->ni];
    if(!fits(p,20,z)){snprintf(err,cap,"%s: truncated tensor table",path);graph_free(g);return 0;}
    *t=(Tensor){u32(b+p),u32(b+p+4),u32(b+p+8),u32(b+p+12),u32(b+p+16)};p+=20;
  }
  if(g->bo<p){snprintf(err,cap,"%s: blob overlaps graph records",path);graph_free(g);return 0;}
  return graph_validate(g,z,err,cap,path);
}
static int shape_is(const Tensor *t,uint32_t h,uint32_t w,uint32_t d) {
  return t->b==1&&t->h==h&&t->w==w&&t->d==d&&t->e==1;
}
static int read_file(const char *path,uint8_t **data,size_t *size,char *err,size_t cap) {
  FILE*f=fopen(path,"rb");long n;
  if(!f){snprintf(err,cap,"%s: open failed: %s",path,strerror(errno));return 0;}
  if(fseek(f,0,SEEK_END)||(n=ftell(f))<0||fseek(f,0,SEEK_SET)){snprintf(err,cap,"%s: cannot determine file size",path);fclose(f);return 0;}
  *data=malloc(n?(size_t)n:1);if(!*data){snprintf(err,cap,"%s: out of memory",path);fclose(f);return 0;}
  *size=(size_t)n;if(fread(*data,1,*size,f)!=*size){snprintf(err,cap,"%s: read failed",path);free(*data);*data=NULL;fclose(f);return 0;}
  fclose(f);return 1;
}
static int graph_build(OnboardTongue*t,TongueGraph*tg,const char*path,
                       uint32_t ih,uint32_t iw,uint32_t id,
                       char*err,size_t cap) {
  uint32_t i;int rc;const Tensor *out;
  if(!read_file(path,&tg->file,&tg->file_size,err,cap))return 0;
  if(!graph_parse(tg->file,tg->file_size,&tg->graph,err,cap,path))return 0;
  if(tg->graph.ni!=1||tg->graph.no!=1||!shape_is(&tg->graph.in[0],ih,iw,id)){
    snprintf(err,cap,"%s: expected one input 1x%ux%ux%u and one output, elem_size=1",path,ih,iw,id);return 0;
  }
  out=&tg->graph.out[0];
  if(out->b!=1||out->e!=1||!out->h||!out->w||!out->d){snprintf(err,cap,"%s: expected NHWC output with batch 1 and elem_size=1",path);return 0;}
  if(!tensor_bytes(&tg->graph.in[0],&tg->input_size)||!tensor_bytes(&tg->graph.out[0],&tg->output_size)){snprintf(err,cap,"%s: invalid graph tensor size",path);return 0;}
  tg->input_data=malloc(tg->input_size);tg->output_data=malloc(tg->output_size);
  if(!tg->input_data||!tg->output_data){snprintf(err,cap,"%s: out of memory allocating graph tensors",path);return 0;}
  tg->input=(hta_tensordef){1,ih,iw,id,tg->input_data,tg->input_size,tg->input_size,0};
  tg->output=(hta_tensordef){1,out->h,out->w,out->d,tg->output_data,tg->output_size,tg->output_size,0};
  rc=t->init(&tg->id);if(rc){snprintf(err,cap,"%s: hexagon_hta_nn_init failed (%d)",path,rc);return 0;}tg->live=1;
  for(i=0;i<tg->graph.nc;i++){CRec*c=&tg->graph.c[i];rc=t->append_const(tg->id,c->id,c->b,c->h,c->w,c->d,tg->file+tg->graph.bo+c->off,c->len);if(rc){snprintf(err,cap,"%s: append const record %u failed (%d)",path,i,rc);return 0;}}
  for(i=0;i<tg->graph.nn;i++){NRec*n=&tg->graph.n[i];rc=t->append_node(tg->id,n->id,n->op,n->pad,n->in,n->ni,n->out,n->no);if(rc){snprintf(err,cap,"%s: append node record %u failed (%d)",path,i,rc);return 0;}}
  rc=t->prepare(tg->id);if(rc){snprintf(err,cap,"%s: hexagon_hta_nn_prepare failed (%d)",path,rc);return 0;}return 1;
}
static int parse_float_value(const char*s,float*out) {
  char*end;float v;errno=0;v=strtof(s,&end);if(errno||end==s||*end||!isfinite(v))return 0;*out=v;return 1;
}
static int params_load(const char*path,ModelParams*p,char*err,size_t cap) {
  FILE*f=fopen(path,"r");char line[2048];unsigned seen=0;int targets=0;
  memset(p,0,sizeof(*p));if(!f){snprintf(err,cap,"%s: open failed: %s",path,strerror(errno));return 0;}
  while(fgets(line,sizeof(line),f)) {
    char*eq=strchr(line,'=');char*nl=strchr(line,'\n');if(nl)*nl=0;if((nl=strchr(line,'\r')))*nl=0;
    if(!eq){if(line[0]){snprintf(err,cap,"%s: malformed line",path);fclose(f);return 0;}continue;}*eq++=0;
    if(!strcmp(line,"image_size")){char*end;unsigned long v=strtoul(eq,&end,10);if(end==eq||*end||v==0||v>TONGUE_MAX_IMAGE)goto bad;p->image_size=(uint32_t)v;}
    else if(!strcmp(line,"encoder_out_min")){if(!parse_float_value(eq,&p->encoder_min))goto bad;seen|=1;}
    else if(!strcmp(line,"encoder_out_max")){if(!parse_float_value(eq,&p->encoder_max))goto bad;seen|=2;}
    else if(!strcmp(line,"fusion_in_min")){if(!parse_float_value(eq,&p->fusion_in_min))goto bad;seen|=4;}
    else if(!strcmp(line,"fusion_in_max")){if(!parse_float_value(eq,&p->fusion_in_max))goto bad;seen|=8;}
    else if(!strcmp(line,"fusion_out_min")){if(!parse_float_value(eq,&p->fusion_out_min))goto bad;seen|=16;}
    else if(!strcmp(line,"fusion_out_max")){if(!parse_float_value(eq,&p->fusion_out_max))goto bad;seen|=32;}
    else if(!strcmp(line,"targets")){char*save=NULL,*tok;unsigned i=0;if(targets)goto bad;targets=1;for(tok=strtok_r(eq,",",&save);tok;tok=strtok_r(NULL,",",&save)){size_t n=strlen(tok);if(i>=TONGUE_MAX_TARGETS||n==0||n>=sizeof(p->names[0]))goto bad;memcpy(p->names[i++],tok,n+1);}if(i==0)goto bad;p->targets=i;}
    else {snprintf(err,cap,"%s: unknown key",path);fclose(f);return 0;}
  }
  if(ferror(f)){snprintf(err,cap,"%s: read error",path);fclose(f);return 0;}fclose(f);
  if(seen!=63||!targets){snprintf(err,cap,"%s: requires all six ranges and a target list",path);return 0;}if(!p->image_size)p->image_size=224;
  if(!(p->encoder_min<p->encoder_max)||!(p->fusion_in_min<p->fusion_in_max)||!(p->fusion_out_min<p->fusion_out_max)){snprintf(err,cap,"%s: each range min must be less than max",path);return 0;}return 1;
bad: snprintf(err,cap,"%s: invalid value or targets",path);fclose(f);return 0;
}
static int head_load(const char*path,Head*h,float**storage,uint32_t expected_inputs,uint32_t outputs,char*err,size_t cap) {
  uint8_t*b=NULL;size_t z=0,remaining;float*mem;uint32_t ni,n0,n1;size_t floats,p=20;
  if(!read_file(path,&b,&z,err,cap))return 0;
  if(z<20||memcmp(b,"THD1",4)||u32(b+4)!=outputs){snprintf(err,cap,"%s: invalid THD1 header",path);free(b);return 0;}ni=u32(b+8);n0=u32(b+12);n1=u32(b+16);if(ni!=expected_inputs||!n0||!n1){snprintf(err,cap,"%s: head input/hidden dimensions mismatch",path);free(b);return 0;}
  if(z<20+outputs||(z-20-outputs)%sizeof(float)){snprintf(err,cap,"%s: head file size does not match header",path);free(b);return 0;}
  floats=(z-20-outputs)/sizeof(float);remaining=floats;
  {
    uint64_t terms[6]={(uint64_t)n0*ni,n0,(uint64_t)n1*n0,n1,(uint64_t)outputs*n1,outputs};
    for(unsigned i=0;i<6;i++){
      if(terms[i]>remaining){snprintf(err,cap,"%s: head file size does not match header",path);free(b);return 0;}
      remaining-=(size_t)terms[i];
    }
  }
  if(remaining){snprintf(err,cap,"%s: head file size does not match header",path);free(b);return 0;}
  mem=malloc(floats*sizeof(float));if(!mem){snprintf(err,cap,"%s: out of memory",path);free(b);return 0;}
  memcpy(mem,b+p,floats*sizeof(float));p+=floats*sizeof(float);memcpy(h->signed_mask,b+p,outputs);h->outputs=outputs;free(b);
  {size_t q=0;h->inputs=ni;h->hidden0=n0;h->hidden1=n1;h->w0=mem+q;q+=(size_t)n0*ni;h->b0=mem+q;q+=n0;h->w1=mem+q;q+=(size_t)n1*n0;h->b1=mem+q;q+=n1;h->w2=mem+q;q+=(size_t)outputs*n1;h->b2=mem+q;}
  *storage=mem;return 1;
}
static int load_symbols(OnboardTongue*t,char*err,size_t cap) {
  const char*names[]={"hexagon_hta_nn_init","hexagon_hta_nn_prepare","hexagon_hta_nn_teardown","hexagon_hta_nn_append_const_node","hexagon_hta_nn_append_node","hexagon_hta_nn_execute_new"};
  void**slots[]={ (void**)&t->init,(void**)&t->prepare,(void**)&t->teardown,(void**)&t->append_const,(void**)&t->append_node,(void**)&t->execute};unsigned i;
  t->library=dlopen("/vendor/lib64/libhta_hexagon_runtime.so",RTLD_NOW|RTLD_LOCAL);if(!t->library){snprintf(err,cap,"dlopen /vendor/lib64/libhta_hexagon_runtime.so: %s",dlerror());return 0;}
  for(i=0;i<sizeof(names)/sizeof(names[0]);i++){*slots[i]=dlsym(t->library,names[i]);if(!*slots[i]){snprintf(err,cap,"missing runtime symbol %s",names[i]);return 0;}}return 1;
}
OnboardTongue*tongue_open(const char*bundle_dir,char*error,size_t error_len) {
  OnboardTongue*t=calloc(1,sizeof(*t));char path[1024];unsigned m,models;int ok=0;struct stat st;
  size_t bridge_size=0,feature_count=0,h0_count=0,h1_count=0;
  if(error&&error_len)error[0]=0;if(!t){if(error&&error_len)snprintf(error,error_len,"out of memory");return NULL;}
  if(!bundle_dir||snprintf(path,sizeof(path),"%s/model",bundle_dir)>=(int)sizeof(path)){if(error&&error_len)snprintf(error,error_len,"bundle path is null or too long");goto done;}
  if(stat(path,&st)==0){if(!S_ISDIR(st.st_mode)){snprintf(error,error_len,"%s: expected model directory",path);goto done;}t->single=1;}
  else if(errno!=ENOENT){snprintf(error,error_len,"%s: stat failed: %s",path,strerror(errno));goto done;}
  models=t->single?1u:2u;
  for(m=0;m<models;m++){
    const char*model=t->single?"model":(m?"direction":"gate");
    if(snprintf(path,sizeof(path),"%s/%s/params.txt",bundle_dir,model)>=(int)sizeof(path)){snprintf(error,error_len,"bundle path too long for %s/params.txt",model);goto done;}
    if(!params_load(path,&t->params[m],error,error_len))goto done;
    if(t->params[m].image_size%16){snprintf(error,error_len,"%s: image_size must be a multiple of 16",path);goto done;}
  }
  if(models==2){
    if(t->params[0].image_size!=t->params[1].image_size){snprintf(error,error_len,"direction/params.txt: image_size differs from gate/params.txt");goto done;}
    if(t->params[0].targets!=t->params[1].targets){snprintf(error,error_len,"direction/params.txt: target count differs from gate/params.txt");goto done;}
    for(m=0;m<t->params[0].targets;m++)if(strcmp(t->params[0].names[m],t->params[1].names[m])){snprintf(error,error_len,"direction/params.txt: target names differ from gate/params.txt at index %u",m);goto done;}
  }
  t->image_size=t->params[0].image_size;
  memcpy(t->names,t->params[0].names,sizeof(t->names));t->targets=t->params[0].targets;
  if(!load_symbols(t,error,error_len))goto done;
  for(m=0;m<models;m++){
    const char*model=t->single?"model":(m?"direction":"gate");
    TongueGraph*enc=&t->graphs[m*2],*fus=&t->graphs[m*2+1];ModelDims*d=&t->dims[m];
    if(snprintf(path,sizeof(path),"%s/%s/encoder.htag",bundle_dir,model)>=(int)sizeof(path)){snprintf(error,error_len,"bundle path too long for %s/encoder.htag",model);goto done;}
    if(!graph_build(t,enc,path,t->image_size,t->image_size,1,error,error_len))goto done;
    d->h=enc->graph.out[0].h;d->w=enc->graph.out[0].w;d->c=enc->graph.out[0].d;
    if(d->c>UINT32_MAX/4){snprintf(error,error_len,"%s: encoder channels overflow fusion input depth",path);goto done;}
    if(snprintf(path,sizeof(path),"%s/%s/fusion.htag",bundle_dir,model)>=(int)sizeof(path)){snprintf(error,error_len,"bundle path too long for %s/fusion.htag",model);goto done;}
    if(!graph_build(t,fus,path,d->h,d->w,d->c*4,error,error_len))goto done;
    d->h2=fus->graph.out[0].h;d->w2=fus->graph.out[0].w;d->f=fus->graph.out[0].d;
    if(d->f>UINT32_MAX/2){snprintf(error,error_len,"%s: fusion channels overflow head input count",path);goto done;}
    if(snprintf(path,sizeof(path),"%s/%s/head.bin",bundle_dir,model)>=(int)sizeof(path)){snprintf(error,error_len,"bundle path too long for %s/head.bin",model);goto done;}
    if(!head_load(path,&t->heads[m],&t->head_storage[m],d->f*2,t->targets,error,error_len))goto done;
    if(bridge_size<fus->input_size)bridge_size=fus->input_size;
    if(feature_count<t->heads[m].inputs)feature_count=t->heads[m].inputs;
    if(h0_count<t->heads[m].hidden0)h0_count=t->heads[m].hidden0;
    if(h1_count<t->heads[m].hidden1)h1_count=t->heads[m].hidden1;
  }
  if(feature_count>SIZE_MAX/sizeof(float)||h0_count>SIZE_MAX/sizeof(float)||h1_count>SIZE_MAX/sizeof(float)){snprintf(error,error_len,"head scratch buffer size overflow");goto done;}
  t->bridge=malloc(bridge_size);t->features=malloc(feature_count*sizeof(float));t->h0=malloc(h0_count*sizeof(float));t->h1=malloc(h1_count*sizeof(float));
  if(!t->bridge||!t->features||!t->h0||!t->h1){snprintf(error,error_len,"out of memory allocating tongue scratch buffers");goto done;}
  ok=1;
done: if(!ok){tongue_close(t);return NULL;}return t;
}
static float dequant(uint8_t code,float lo,float hi) { return (float)code*((hi-lo)/255.0f)+lo; }
static uint8_t quantize(float real,float lo,float hi) {
  float q=nearbyintf((real-lo)/((hi-lo)/255.0f));if(q<0.0f)q=0.0f;if(q>255.0f)q=255.0f;return (uint8_t)q;
}
static int run_graph(OnboardTongue*t,TongueGraph*g,double*elapsed,char*err,size_t cap) {
  int rc;double start;g->input.data_valid_len=g->input.data_len;g->output.data_valid_len=g->output.data_len;start=now_ms();rc=t->execute(g->id,&g->input,1,&g->output,1);*elapsed+=now_ms()-start;
  if(rc){snprintf(err,cap,"HTA execute failed for graph 0x%x (%d)",g->id,rc);return 0;}return 1;
}
static void cpu_head(const Head*h,const float*x,float*h0,float*h1,float*y) {
  uint32_t i,j;for(i=0;i<h->hidden0;i++){float s=h->b0[i];for(j=0;j<h->inputs;j++)s+=h->w0[(size_t)i*h->inputs+j]*x[j];h0[i]=s/(1.0f+expf(-s));}
  for(i=0;i<h->hidden1;i++){float s=h->b1[i];for(j=0;j<h->hidden0;j++)s+=h->w1[(size_t)i*h->hidden0+j]*h0[j];h1[i]=s/(1.0f+expf(-s));}
  for(i=0;i<h->outputs;i++){float s=h->b2[i];for(j=0;j<h->hidden1;j++)s+=h->w2[(size_t)i*h->hidden1+j]*h1[j];y[i]=h->signed_mask[i]?tanhf(s):1.0f/(1.0f+expf(-s));}
}
/* One caller thread owns this object; tongue_run and tongue_close are not thread-safe. */
int tongue_run(OnboardTongue*t,const uint8_t*left,const uint8_t*right,TongueResult*r,char*error,size_t error_len) {
  unsigned m,p,c;double start=now_ms(),htams=0.0;float y[TONGUE_MAX_TARGETS]={0};
  if(!t||!left||!right||!r){if(error&&error_len)snprintf(error,error_len,"tongue_run: null argument");return -1;}
  if(error&&error_len)error[0]=0;
  for(m=0;m<(t->single?1u:2u);m++){
    TongueGraph*enc=&t->graphs[m*2],*fus=&t->graphs[m*2+1];ModelParams*q=&t->params[m];ModelDims*d=&t->dims[m];
    uint32_t enc_pixels=d->h*d->w, fus_pixels=d->h2*d->w2, channels4=d->c*4;
    memcpy(enc->input_data,left,(size_t)t->image_size*t->image_size);if(!run_graph(t,enc,&htams,error,error_len))return -1;
    /* Save left encoder output in the first C channels while right runs. */
    for(p=0;p<enc_pixels;p++)memcpy(t->bridge+(size_t)p*channels4,enc->output_data+(size_t)p*d->c,d->c);
    memcpy(enc->input_data,right,(size_t)t->image_size*t->image_size);if(!run_graph(t,enc,&htams,error,error_len))return -1;
    for(p=0;p<enc_pixels;p++)for(c=0;c<d->c;c++){
      float l=dequant(t->bridge[(size_t)p*channels4+c],q->encoder_min,q->encoder_max);
      float rr=dequant(enc->output_data[(size_t)p*d->c+c],q->encoder_min,q->encoder_max);
      size_t base=(size_t)p*channels4;float vals[4]={l,rr,fabsf(l-rr),l*rr};unsigned k;
      for(k=0;k<4;k++)t->bridge[base+(size_t)k*d->c+c]=quantize(vals[k],q->fusion_in_min,q->fusion_in_max);
    }
    memcpy(fus->input_data,t->bridge,fus->input_size);if(!run_graph(t,fus,&htams,error,error_len))return -1;
    for(c=0;c<d->f;c++){
      float sum=0.0f,mx=-INFINITY;for(p=0;p<fus_pixels;p++){float v=dequant(fus->output_data[(size_t)p*d->f+c],q->fusion_out_min,q->fusion_out_max);sum+=v;if(v>mx)mx=v;}
      t->features[c]=sum/(float)fus_pixels;t->features[d->f+c]=mx;
    }
    cpu_head(&t->heads[m],t->features,t->h0,t->h1,y);
    if(m==0)memcpy(r->gate,y,sizeof(y));if(m==1||t->single)memcpy(r->direction,y,sizeof(y));
  }
  r->hta_ms=htams;r->total_ms=now_ms()-start;return 0;
}
int tongue_image_size(const OnboardTongue*t) { return t?(int)t->image_size:0; }
int tongue_is_single_model(const OnboardTongue*t) { return t?t->single:0; }
const char*tongue_target_name(const OnboardTongue*t,int index) {
  if(!t||index<0||(uint32_t)index>=t->targets)return NULL;return t->names[index];
}
int tongue_target_count(const OnboardTongue*t) { return t?(int)t->targets:0; }
void tongue_close(OnboardTongue*t) {
  unsigned i;if(!t)return;
  for(i=0;i<4;i++){TongueGraph*g=&t->graphs[i];if(g->live&&t->teardown)t->teardown(g->id);free(g->input_data);free(g->output_data);free(g->file);graph_free(&g->graph);}
  for(i=0;i<2;i++)free(t->head_storage[i]);free(t->bridge);free(t->features);free(t->h0);free(t->h1);if(t->library)dlclose(t->library);free(t);
}
