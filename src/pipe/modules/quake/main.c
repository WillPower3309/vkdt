#include "modules/api.h"
#include "modules/localsize.h"
#include "core/half.h"
#include "../rt/quat.h"
#include "../i-raw/mat3.h"
#undef CLAMP // ouch. careful! quake redefines this but with (m, x, M) param order!

#include "config.h"
#include "quakedef.h"
#include "bgmusic.h"

#define MAX_GLTEXTURES 4096 // happens to be MAX_GLTEXTURES in quakespasm, a #define in gl_texmgr.c
#define MAX_IDX_CNT 2000000 // global bounds for dynamic geometry, because lazy programmer
#define MAX_VTX_CNT 2000000

// TODO factor out into some geo.h
typedef struct rtgeo_vtx_t
{
  uint16_t x, y, z;  // half float cartesian vertex coordinate
  uint16_t tex;      // texture id
  int16_t  n0, n1;   // encoded normal
  uint16_t s, t;     // half float texture st coordinates
}
rtgeo_vtx_t;
typedef struct rtgeo_tri_t
{ // plain flat triangle. each vertex stores a different texture for the triangle: albedo, normals, extra.
  rtgeo_vtx_t v0, v1, v2;
}
rtgeo_tri_t;

static inline double
xrand(uint32_t reset)
{ // Algorithm "xor" from p. 4 of Marsaglia, "Xorshift RNGs"
  static uint32_t seed = 1337;
  if(reset) seed = 1337;
  seed ^= seed << 13;
  seed ^= seed >> 17;
  seed ^= seed << 5;
  return seed / 4294967296.0;
}

typedef struct qs_data_t
{
  double       oldtime;
  quakeparms_t parms;

  uint32_t    *tex[MAX_GLTEXTURES];       // cpu side cache of quake textures
  uint32_t     tex_dim[2*MAX_GLTEXTURES]; // resolutions of textures for modify_roi_out
  uint8_t      tex_req[MAX_GLTEXTURES];   // dynamic texture request flag: replace this slot by new upload
  int32_t      tex_cnt;                   // current number of textures
  uint32_t     tex_maxw, tex_maxh;        // maximum texture size, to allocate staging upload buffer
  uint32_t     skybox[6];                 // 6 cubemap skybox texture ids
  uint32_t     tex_explosion;             // something emissive for particles
  uint32_t     tex_blood;                 // for blood particles

  uint32_t     move;                      // for our own debug camera
  double       mx, my;                    // mouse coordinates

  uint32_t     initing;                   // for synchronous initing
  uint32_t     worldspawn;                // need to re-create everything?
}
qs_data_t;

// put quakeparams and ref to this module in static global memory :(
// this is because quake does it too and thus we can't have two instances
// also the texture manager will call into us
static qs_data_t qs_data = {0};
extern float r_avertexnormals[162][3]; // from r_alias.c
extern particle_t *active_particles;

int init(dt_module_t *mod)
{
  mod->data = &qs_data;
  mod->flags = s_module_request_read_source;

  uint32_t initing = 0;
  do
  {
    initing = __sync_val_compare_and_swap(&qs_data.initing, 0, 1);
    if(initing == 2) // someone else is already done initing
      return 0;
    sched_yield(); // let the other thread continue with this
  } while(initing == 1); // someone else is already initing this


  // quake has already been globally inited. it's so full of static global stuff,
  // we can't init it for thumbnails again. give up:
  if(host_initialized) return 0;

  qs_data_t *d = mod->data;
  d->tex_maxw = 2048; // as it turns out, we sometimes adaptively load more textures and thus the max detection isn't robust.
  d->tex_maxh = 2048; // we would need to re-create nodes every time a new max is detected in QS_load_texture.

  // these host parameters are just a pointer with defined linkage in host.c.
  // we'll need to point it to an actual struct (that belongs to us):
  host_parms = &d->parms;
  d->parms.basedir = "/usr/share/games/quake"; // does not work

  char *argv[] = {"quakespasm",
    "-basedir", "/usr/share/games/quake",
    "+skill", "2",
  };
  int argc = 5;

  d->worldspawn = 0;
  d->parms.argc = argc;
  d->parms.argv = argv;
  d->parms.errstate = 0;

  srand(1337); // quake uses this
  COM_InitArgv(d->parms.argc, d->parms.argv);

  Sys_Init();

  d->parms.memsize = 256 * 1024 * 1024; // qs default in 0.94.3
  d->parms.membase = malloc (d->parms.memsize);

  if (!d->parms.membase)
    Sys_Error ("Not enough memory free; check disk space\n");

  Sys_Printf("Quake %1.2f (c) id Software\n", VERSION);
  Sys_Printf("GLQuake %1.2f (c) id Software\n", GLQUAKE_VERSION);
  Sys_Printf("FitzQuake %1.2f (c) John Fitzgibbons\n", FITZQUAKE_VERSION);
  Sys_Printf("FitzQuake SDL port (c) SleepwalkR, Baker\n");
  Sys_Printf("QuakeSpasm " QUAKESPASM_VER_STRING " (c) Ozkan Sezer, Eric Wasylishen & others\n");

  Sys_Printf("Host_Init\n");
  Host_Init(); // this one has a lot of meat! we'll need to cut it short i suppose

  // S_BlockSound(); // only start when grabbing
  // close menu because we don't have an esc key:
  key_dest = key_game;
  m_state = m_none;
  IN_Activate();

  initing = __sync_val_compare_and_swap(&qs_data.initing, 1, 2);
  return 0;
}

void
input(
    dt_module_t             *mod,
    dt_module_input_event_t *p)
{
  qs_data_t *dat = mod->data;

  if(p->type == 0)
  { // activate event: store mouse zero and clear all movement flags we might still have
    dat->move = 0;
    dat->mx = dat->my = -666.0;
    S_UnblockSound();
    return;
  }

  if(p->type == -1)
  { // deactivate event
    dat->mx = dat->my = -666.0;
    S_BlockSound();
  }

#if 0 // custom camera:
  float *p_cam = (float *)dt_module_param_float(mod, dt_module_get_param(mod->so, dt_token("cam")));
  if(p->type == 1)
  { // mouse button
  }

  if(p->type == 2)
  { // mouse position: rotate camera based on mouse coordinate
    if(dat->mx == -666.0 && dat->my == -666.0) { dat->mx = p->x; dat->my = p->y; }
    const float avel = 0.001f; // angular velocity
    quat_t rotx, roty, tmp;
    float rgt[3], top[] = {0,0,1};
    cross(top, p_cam+4, rgt);
    quat_init_angle(&rotx, (p->x-dat->mx)*avel, 0, 0, -1);
    quat_init_angle(&roty, (p->y-dat->my)*avel, rgt[0], rgt[1], rgt[2]);
    quat_mul(&rotx, &roty, &tmp);
    quat_transform(&tmp, p_cam+4);
    dat->mx = p->x;
    dat->my = p->y;
  }

  if(p->type == 3)
  { // mouse scrolled, .dx .dy
  }

  if(p->type == 4)
  { // keyboard (key, scancode)
    if(p->action <= 1) // ignore key repeat
    switch(p->key)
    {
      case 'E': dat->move = (dat->move & ~(1<<0)) | (p->action<<0); break;
      case 'D': dat->move = (dat->move & ~(1<<1)) | (p->action<<1); break;
      case 'S': dat->move = (dat->move & ~(1<<2)) | (p->action<<2); break;
      case 'F': dat->move = (dat->move & ~(1<<3)) | (p->action<<3); break;
      case ' ': dat->move = (dat->move & ~(1<<4)) | (p->action<<4); break;
      case 'V': dat->move = (dat->move & ~(1<<5)) | (p->action<<5); break;
      case 'R': // reset camera
                dat->move = 0;
                memset(p_cam, 0, sizeof(float)*8);
                p_cam[4] = 1;
                dt_module_input_event_t p = { 0 };
                mod->so->input(mod, &p);
                return;
    }
  }
#else
  if(p->type == 1)
  { // mouse button
    const int remap[] = {K_MOUSE1, K_MOUSE2, K_MOUSE3, K_MOUSE4, K_MOUSE5};
    if(p->mbutton < 0 || p->mbutton > 4) return;
    Key_Event(remap[p->mbutton], p->action == 1);
  }
  else if(p->type == 2)
  { // mouse position
    dat->mx = p->x; dat->my = p->y;
  }
  else if(p->type == 3)
  { // mouse wheel
    if (p->dy > 0)
    {
      Key_Event(K_MWHEELUP, true);
      Key_Event(K_MWHEELUP, false);
    }
    else if (p->dy < 0)
    {
      Key_Event(K_MWHEELDOWN, true);
      Key_Event(K_MWHEELDOWN, false);
    }
  }
  else if(p->type == 4)
  { // key
    int down = p->action == 1; // GLFW_PRESS
    // we interpret the keyboard as the US layout, so keybindings
    // are based on key position, not the label on the key cap.
    // case SDL_SCANCODE_TAB: return K_TAB;
    // case SDL_SCANCODE_RETURN: return K_ENTER;
    // case SDL_SCANCODE_RETURN2: return K_ENTER;
    // case SDL_SCANCODE_ESCAPE: return K_ESCAPE;
    // case SDL_SCANCODE_SPACE: return K_SPACE;
    // the rest seems sane
    int key = p->key;//p->scancode;
    if(key >= 65 && key <= 90) key += 32; // convert lower to upper case
    if(p->action <= 1) // ignore key repeat
      Key_Event (key, down);
  }
#endif
}

void cleanup(dt_module_t *mod)
{
  // Host_Shutdown(); // doesn't work
  // XXX also can't clean up the params on there! another module/pipeline might need it!
  // qs_data_t *d = mod->data;
  // free(d);
  // i don't think quake does any cleanup
  qs_data.worldspawn = 1; // re-init next time
  mod->data = 0;
}

// called from within qs, pretty much a copy from in_sdl.c:
extern cvar_t cl_maxpitch; //johnfitz -- variable pitch clamping
extern cvar_t cl_minpitch; //johnfitz -- variable pitch clamping
void IN_Move(usercmd_t *cmd)
{
  static double oldx, oldy;
  if(qs_data.mx == -666.0 && qs_data.my == -666.0)
  {
    oldx = qs_data.mx;
    oldy = qs_data.my;
  }
  if(oldx == -666.0 && oldy == -666.0)
  {
    oldx = qs_data.mx;
    oldy = qs_data.my;
  }
  int dmx = (qs_data.mx - oldx) * sensitivity.value;
  int dmy = (qs_data.my - oldy) * sensitivity.value;
  oldx = qs_data.mx;
  oldy = qs_data.my;

  if ( (in_strafe.state & 1) || (lookstrafe.value && (in_mlook.state & 1) ))
    cmd->sidemove += m_side.value * dmx;
  else
    cl.viewangles[YAW] -= m_yaw.value * dmx;

  if (in_mlook.state & 1)
  {
    if (dmx || dmy)
      V_StopPitchDrift ();
  }

  if ( (in_mlook.state & 1) && !(in_strafe.state & 1))
  {
    cl.viewangles[PITCH] += m_pitch.value * dmy;
    /* johnfitz -- variable pitch clamping */
    if (cl.viewangles[PITCH] > cl_maxpitch.value)
      cl.viewangles[PITCH] = cl_maxpitch.value;
    if (cl.viewangles[PITCH] < cl_minpitch.value)
      cl.viewangles[PITCH] = cl_minpitch.value;
  }
  else
  {
    if ((in_strafe.state & 1) && noclip_anglehack)
      cmd->upmove -= m_forward.value * dmy;
    else
      cmd->forwardmove -= m_forward.value * dmy;
  }
}

// called from within quakespasm each time a new map is (re)loaded
void QS_worldspawn()
{
  qs_data.worldspawn = 1;
}

// called from within quakespasm, we provide the function:
void QS_texture_load(gltexture_t *glt, uint32_t *data)
{
  // TODO: maybe use our string pool to do name->id mapping? also it seems quake has a crc
  if(qs_data.initing < 1) return;
  // mirror the data locally. the copy sucks, but whatever.
  // fprintf(stderr, "[load tex] %u %ux%u %s\n", glt->texnum, glt->width, glt->height, glt->name);
  qs_data.tex_cnt = MIN(MAX_GLTEXTURES, MAX(qs_data.tex_cnt, glt->texnum+1));
  if(glt->texnum >= qs_data.tex_cnt)
  {
    fprintf(stderr, "[load tex] no more free slots for %s!\n", glt->name);
    return;
  }
  if(!strcmp(glt->name, "progs/gib_1.mdl:frame0")) // makes okay blood (non-emissive)
    qs_data.tex_blood = glt->texnum;
  if(!strcmp(glt->name, "progs/s_exp_big.spr:frame10")) // makes good sparks
    qs_data.tex_explosion = glt->texnum; // HACK: store for emissive rocket particle trails
  int wd = qs_data.tex_dim[2*glt->texnum+0] = MAX(2, glt->width);
  int ht = qs_data.tex_dim[2*glt->texnum+1] = MAX(2, glt->height);
  if(qs_data.tex[glt->texnum]) free(qs_data.tex[glt->texnum]);
  qs_data.tex[glt->texnum] = malloc(sizeof(uint32_t)*wd*ht);
  if(glt->width > 0 && glt->height > 0) memcpy(qs_data.tex[glt->texnum], data, sizeof(uint32_t)*glt->width*glt->height);
  qs_data.tex_maxw = MAX(qs_data.tex_maxw, glt->width);
  qs_data.tex_maxh = MAX(qs_data.tex_maxh, glt->height);
  qs_data.tex_req[glt->texnum] = 1; // replace this allocation and upload new texture

  if(!strncmp(glt->name+strlen(glt->name)-6, "_front", 6) ||
     !strncmp(glt->name+strlen(glt->name)-5, "_back", 5))
  { // classic quake sky
    if(!strncmp(glt->name+strlen(glt->name)-6, "_front", 6))
      qs_data.skybox[1] = glt->texnum;
    if(!strncmp(glt->name+strlen(glt->name)-5, "_back", 5))
      qs_data.skybox[0] = glt->texnum;
    qs_data.skybox[2] = -1u;
  }
  if(!strncmp(glt->name, "gfx/env/", 8))
  { // full featured cube map/arcane dimensions
    if(!strncmp(glt->name+strlen(glt->name)-3, "_rt", 3)) qs_data.skybox[0] = glt->texnum;
    if(!strncmp(glt->name+strlen(glt->name)-3, "_bk", 3)) qs_data.skybox[1] = glt->texnum;
    if(!strncmp(glt->name+strlen(glt->name)-3, "_lf", 3)) qs_data.skybox[2] = glt->texnum;
    if(!strncmp(glt->name+strlen(glt->name)-3, "_ft", 3)) qs_data.skybox[3] = glt->texnum;
    if(!strncmp(glt->name+strlen(glt->name)-3, "_up", 3)) qs_data.skybox[4] = glt->texnum;
    if(!strncmp(glt->name+strlen(glt->name)-3, "_dn", 3)) qs_data.skybox[5] = glt->texnum;
  }
  // these should emit in ad_tears. we hack it later when uploading the texture:
  // if(strstr(glt->name, "wfall")) fprintf(stderr, "XXX %s\n", glt->name);

  // TODO: think about cleanup later, maybe
}

#if 0
// could wire this, but we'll notice when quake overwrites the textures anyways
void QS_texture_free(gltexture_t *texture)
{
  if(qs_data.initing < 1) return;
  qs_data.tex_req[glt->texnum] = ??;
  free(qs_data.tex[glt->texnum]);
  qs_data.tex[glt->texnum] = 0;
}
#endif

static inline void
encode_normal(
    int16_t *enc,
    float   *vec)
{
  const float invL1Norm = 1.0f / (fabsf(vec[0]) + fabsf(vec[1]) + fabsf(vec[2]));
  // first find floating point values of octahedral map in [-1,1]:
  float enc0, enc1;
  if (vec[2] < 0.0f)
  {
    enc0 = (1.0f - fabsf(vec[1] * invL1Norm)) * ((vec[0] < 0.0f) ? -1.0f : 1.0f);
    enc1 = (1.0f - fabsf(vec[0] * invL1Norm)) * ((vec[1] < 0.0f) ? -1.0f : 1.0f);
  }
  else
  {
    enc0 = vec[0] * invL1Norm;
    enc1 = vec[1] * invL1Norm;
  }
  enc[0] = roundf(CLAMP(-32768.0f, enc0 * 32768.0f, 32767.0f));
  enc[1] = roundf(CLAMP(-32768.0f, enc1 * 32768.0f, 32767.0f));
}

static void
add_particles(
    uint16_t *vtx,   // vertex data + extra
    uint32_t *vtx_cnt)
{
  uint32_t nvtx = 0;
  xrand(1); // reset so we can do reference renders
  for(particle_t *p=active_particles;p;p=p->next)
  {
    uint8_t *c = (uint8_t *) &d_8to24table[(int)p->color]; // that would be the colour. pick texture based on this:
    // smoke is r=g=b
    // blood is g=b=0
    // rocket trails are r=2*g=2*b a bit randomised
    uint32_t tex_col = c[1] == 0 && c[2] == 0 ? qs_data.tex_blood : qs_data.tex_explosion;
    uint32_t tex_lum = c[1] == 0 && c[2] == 0 ? 0 : qs_data.tex_explosion;
    int numv = 3; // add single random triangle
    if(*vtx_cnt + nvtx + numv >= MAX_VTX_CNT) return;
    const float voff[4][3] = {
      { 0.0,  1.0,  0.0},
      {-0.5, -0.5, -0.87},
      {-0.5, -0.5,  0.87},
      { 1.0, -0.5,  0.0}};
    float vert[4][3];
    if(vtx)
    {
      for(int l=0;l<3;l++)
      {
        float off = 2*(xrand(0)-0.5) + 2*(xrand(0)-0.5);
        for(int k=0;k<4;k++)
          vert[k][l] = p->org[l] + off + 2*voff[k][l] + (xrand(0)-0.5) + (xrand(0)-0.5);
      }
      // vertex stride: 3v 2n 2uv 1texid = 8
      for(int l=0;l<3;l++) vtx[8*(nvtx+0)+l] = float_to_half(vert[0][l]);
      for(int l=0;l<3;l++) vtx[8*(nvtx+1)+l] = float_to_half(vert[1][l]);
      for(int l=0;l<3;l++) vtx[8*(nvtx+2)+l] = float_to_half(vert[2][l]);
      vtx[8*(nvtx+0)+3] = 0; // encoded normal
      vtx[8*(nvtx+0)+4] = 0; // encoded normal
      vtx[8*(nvtx+0)+5] = float_to_half(0);
      vtx[8*(nvtx+0)+6] = float_to_half(1);
      vtx[8*(nvtx+1)+5] = float_to_half(0);
      vtx[8*(nvtx+1)+6] = float_to_half(0);
      vtx[8*(nvtx+2)+5] = float_to_half(1);
      vtx[8*(nvtx+2)+6] = float_to_half(0);
      vtx[8*(nvtx+0)+7] = tex_col;
      vtx[8*(nvtx+1)+7] = tex_lum;
      vtx[8*(nvtx+2)+7] = 0; // extra
      nvtx += 3;
    }
  } // end for all particles
  *vtx_cnt += nvtx;
}

static void
add_geo(
    entity_t *ent,
    uint16_t *vtx,   // triangle data: 3v f16 + all the extra data, vertex stride is 8 uint16_t
    uint32_t *vtx_cnt)
{
  if(!ent) return;
  // count all verts in all models
  uint32_t nvtx = 0;
  qmodel_t *m = ent->model;
  // fprintf(stderr, "[add_geo] %s\n", m->name);
  // if (!m || m->name[0] == '*') return; // '*' is the moving brush models such as doors
  if (!m) return;
  if(qs_data.worldspawn) return;

  if(m->type == mod_alias)
  { // alias model:
    // TODO: e->model->flags & MF_HOLEY <= enable alpha test
    // fprintf(stderr, "alias origin and angles %g %g %g -- %g %g %g\n",
    //     ent->origin[0], ent->origin[1], ent->origin[2],
    //     ent->angles[0], ent->angles[1], ent->angles[2]);
    aliashdr_t *hdr = (aliashdr_t *)Mod_Extradata(ent->model);
    aliasmesh_t *desc = (aliasmesh_t *) ((uint8_t *)hdr + hdr->meshdesc);
    // the plural here really hurts but it's from quakespasm code:
    int16_t *indexes = (int16_t *) ((uint8_t *) hdr + hdr->indexes);
    trivertx_t *trivertexes = (trivertx_t *) ((uint8_t *)hdr + hdr->vertexes);

    lerpdata_t  lerpdata;
    R_SetupAliasFrame(ent, hdr, ent->frame, &lerpdata);
    R_SetupEntityTransform(ent, &lerpdata);
    // angles: pitch yaw roll. axes: right fwd up
    float angles[3] = {-lerpdata.angles[0], lerpdata.angles[1], lerpdata.angles[2]};
    float fwd[3], rgt[3], top[3], pos_t0[3], pos_t1[3], pos[3];
    float origin[3] = { lerpdata.origin[0], lerpdata.origin[1], lerpdata.origin[2]};
    AngleVectors (angles, fwd, rgt, top);
    for(int k=0;k<3;k++) rgt[k] = -rgt[k]; // seems these come in flipped

    float M[9] = {
      fwd[0], rgt[0], top[0],
      fwd[1], rgt[1], top[1],
      fwd[2], rgt[2], top[2]};
    float Mi[9] = {0};
    mat3inv(Mi, M);

    // for(int f = 0; f < hdr->numposes; f++)
    // TODO: upload all vertices so we can just alter the indices on gpu
    int f = ent->frame;
    if(f < 0 || f >= hdr->numposes) return;
#if 1 // both options fail to extract correct creases/vertex normals for health/shells
    // in fact, the shambler has crazy artifacts all over. maybe this is all wrong and
    // just by chance happened to produce something similar enough sometimes?
    // TODO: fuck this vbo bs and get the mdl itself
    int16_t *tmpn = alloca(2*sizeof(int16_t)*hdr->numverts_vbo);
    if(vtx) for(int v = 0; v < hdr->numverts_vbo; v++)
    {
      int i0 = hdr->numverts * lerpdata.pose1 + desc[v].vertindex;
      int i1 = hdr->numverts * lerpdata.pose2 + desc[v].vertindex;
      float nm0[3], nm1[3], nm[3], nw[3];
      memcpy(nm0, r_avertexnormals[trivertexes[i0].lightnormalindex], sizeof(float)*3);
      memcpy(nm1, r_avertexnormals[trivertexes[i1].lightnormalindex], sizeof(float)*3);
      for(int k=0;k<3;k++) nm[k] = (1.0-lerpdata.blend) * nm0[k] + lerpdata.blend * nm1[k];
      for(int k=0;k<3;k++) nw[k] = nm[0] * Mi[3*0+k] + nm[1] * Mi[3*1+k] + nm[2] * Mi[3*2+k];
      encode_normal(tmpn+2*v, nw);
    }
#endif
    if(*vtx_cnt + nvtx + 3*hdr->numindexes >= MAX_VTX_CNT) return;
    if(vtx) for(int i = 0; i < hdr->numindexes/3; i++)
    {
      rtgeo_tri_t *tri = ((rtgeo_tri_t *)vtx) + i;
      rtgeo_vtx_t *trivtx = &tri->v0;

      for(int tv=0;tv<3;tv++)
      {
        int v = indexes[3*i+tv];
        int i0 = hdr->numverts * lerpdata.pose1 + desc[v].vertindex;
        int i1 = hdr->numverts * lerpdata.pose2 + desc[v].vertindex;

        for(int k=0;k<3;k++) pos_t0[k] = trivertexes[i0].v[k] * hdr->scale[k] + hdr->scale_origin[k];
        for(int k=0;k<3;k++) pos_t1[k] = trivertexes[i1].v[k] * hdr->scale[k] + hdr->scale_origin[k];

        for(int k=0;k<3;k++) pos[k] = (1.0-lerpdata.blend) * pos_t0[k] + lerpdata.blend * pos_t1[k];
        trivtx[tv].x = float_to_half(origin[0] + rgt[0] * pos[1] + top[0] * pos[2] + fwd[0] * pos[0]);
        trivtx[tv].y = float_to_half(origin[1] + rgt[1] * pos[1] + top[1] * pos[2] + fwd[1] * pos[0]);
        trivtx[tv].z = float_to_half(origin[2] + rgt[2] * pos[1] + top[2] * pos[2] + fwd[2] * pos[0]);
        memcpy(&trivtx[tv].n0, tmpn+2*v, sizeof(int16_t)*2);
        trivtx[tv].s = float_to_half((desc[v].st[0]+0.5)/(float)hdr->skinwidth);
        trivtx[tv].t = float_to_half((desc[v].st[1]+0.5)/(float)hdr->skinheight);
      }
      const int sk = CLAMP(0, ent->skinnum, hdr->numskins-1), fm = ((int)(cl.time*10))&3;
      tri->v0.tex = MIN(qs_data.tex_cnt-1, hdr->gltextures[sk][fm]->texnum);
      tri->v1.tex = MIN(qs_data.tex_cnt-1, hdr->fbtextures[sk][fm] ? hdr->fbtextures[sk][fm]->texnum : 0);
      tri->v2.tex = 0;
#if 1 // XXX use normal map if we have it. FIXME this discards the vertex normals
      if(hdr->nmtextures[sk][fm])
      {
        tri->v0.n0 = 0;
        tri->v0.n1 = MIN(qs_data.tex_cnt-1, hdr->nmtextures[sk][fm]->texnum);
        tri->v1.n0 = 0xffff; // mark as brush model // XXX ??? why aren't we using v2.tex?
        tri->v1.n1 = 0xffff;
      }
#endif
#if 0
      float nm[3], nw[3];
      int off = hdr->numverts * f;
  fprintf(stderr, "[add_geo] %s normal index %d numposes %d/%d\n", m->name, trivertexes[off+desc[indexes[3*i+0]].vertindex].lightnormalindex, f, hdr->numposes);
      memcpy(nm, r_avertexnormals[trivertexes[off+desc[indexes[3*i+0]].vertindex].lightnormalindex], sizeof(float)*3);
      for(int k=0;k<3;k++) nw[k] = nm[0] * fwd[k] + nm[1] * rgt[k] + nm[2] * top[k];
      encode_normal(ext+14*i+0, nw);
      memcpy(nm, r_avertexnormals[trivertexes[off+desc[indexes[3*i+1]].vertindex].lightnormalindex], sizeof(float)*3);
      for(int k=0;k<3;k++) nw[k] = nm[0] * fwd[k] + nm[1] * rgt[k] + nm[2] * top[k];
      encode_normal(ext+14*i+2, nw);
      memcpy(nm, r_avertexnormals[trivertexes[off+desc[indexes[3*i+2]].vertindex].lightnormalindex], sizeof(float)*3);
      for(int k=0;k<3;k++) nw[k] = nm[0] * fwd[k] + nm[1] * rgt[k] + nm[2] * top[k];
      encode_normal(ext+14*i+4, nw);
#endif
    }
  }
  else if(m->type == mod_brush)
  { // brush model:
    // fprintf(stderr, "brush origin and angles %g %g %g -- %g %g %g with %d surfs\n",
    //     ent->origin[0], ent->origin[1], ent->origin[2],
    //     ent->angles[0], ent->angles[1], ent->angles[2],
    //     m->nummodelsurfaces);
    float angles[3] = {-ent->angles[0], ent->angles[1], ent->angles[2]};
    float fwd[3], rgt[3], top[3];
    AngleVectors (angles, fwd, rgt, top);
    for (int i=0; i<m->nummodelsurfaces; i++)
    {
#if WATER_MODE==WATER_MODE_FULL
      int wateroffset = 0;
again:;
#endif
      msurface_t *surf = &m->surfaces[m->firstmodelsurface + i];
      if(!strcmp(surf->texinfo->texture->name, "skip")) continue;
      glpoly_t *p = surf->polys;
      while(p)
      {
        if(*vtx_cnt + nvtx + (p->numverts-2) >= MAX_VTX_CNT) break;
#if WATER_MODE==WATER_MODE_FULL
        if(vtx && wateroffset) for(int k=2;k<p->numverts;k++)
        {
          rtgeo_tri_t *tri = ((rtgeo_tri_t*)vtx) + k-2;
          rtgeo_vtx_t *vert = &tri->v0;
          for(int tv=0;tv<3;tv++)
          { // dunno what's wrong with quake's coordinate systems and the bounds, but this works:
            const int i0 = *vtx_cnt + nvtx + (tv == 0 ? 0 : tv == 1 ? k-1 : k);
            vert[tv].x += fwd[0] * (p->verts[i0][0] > (surf->mins[0] + surf->maxs[0])/2.0 ? WATER_DEPTH : - WATER_DEPTH);
            vert[tv].x -= rgt[0] * (p->verts[i0][1] > (surf->mins[1] + surf->maxs[1])/2.0 ? WATER_DEPTH : - WATER_DEPTH);
            vert[tv].y += fwd[1] * (p->verts[i0][0] > (surf->mins[0] + surf->maxs[0])/2.0 ? WATER_DEPTH : - WATER_DEPTH);
            vert[tv].y -= rgt[1] * (p->verts[i0][1] > (surf->mins[1] + surf->maxs[1])/2.0 ? WATER_DEPTH : - WATER_DEPTH);
            vert[tv].z += fwd[2] * (p->verts[i0][0] > (surf->mins[0] + surf->maxs[0])/2.0 ? WATER_DEPTH : - WATER_DEPTH);
            vert[tv].z -= rgt[2] * (p->verts[i0][1] > (surf->mins[1] + surf->maxs[1])/2.0 ? WATER_DEPTH : - WATER_DEPTH);
            vert[tv].z -= WATER_DEPTH;
          }
        }
#endif
        // TODO: make somehow dynamic. don't want to re-upload the whole model just because the texture animates.
        // for now that means static brush models will not actually animate their textures
        texture_t *t = R_TextureAnimation(surf->texinfo->texture, ent->frame);
        if(vtx) for(int k=2;k<p->numverts;k++)
        {
          const int i0 = 0;
          const int i1 = k-1;
          const int i2 = k;
          rtgeo_tri_t *tri = ((rtgeo_tri_t*)vtx) + nvtx/3 + k-2;
          *tri = (rtgeo_tri_t){0};
          tri->v0.x = float_to_half(p->verts[i0][0] * fwd[0] - p->verts[i0][1] * rgt[0] + p->verts[i0][2] * top[0] + ent->origin[0]);
          tri->v0.y = float_to_half(p->verts[i0][0] * fwd[1] - p->verts[i0][1] * rgt[1] + p->verts[i0][2] * top[1] + ent->origin[1]);
          tri->v0.z = float_to_half(p->verts[i0][0] * fwd[2] - p->verts[i0][1] * rgt[2] + p->verts[i0][2] * top[2] + ent->origin[2]);
          tri->v1.x = float_to_half(p->verts[i1][0] * fwd[0] - p->verts[i1][1] * rgt[0] + p->verts[i1][2] * top[0] + ent->origin[0]);
          tri->v1.y = float_to_half(p->verts[i1][0] * fwd[1] - p->verts[i1][1] * rgt[1] + p->verts[i1][2] * top[1] + ent->origin[1]);
          tri->v1.z = float_to_half(p->verts[i1][0] * fwd[2] - p->verts[i1][1] * rgt[2] + p->verts[i1][2] * top[2] + ent->origin[2]);
          tri->v2.x = float_to_half(p->verts[i2][0] * fwd[0] - p->verts[i2][1] * rgt[0] + p->verts[i2][2] * top[0] + ent->origin[0]);
          tri->v2.y = float_to_half(p->verts[i2][0] * fwd[1] - p->verts[i2][1] * rgt[1] + p->verts[i2][2] * top[1] + ent->origin[1]);
          tri->v2.z = float_to_half(p->verts[i2][0] * fwd[2] - p->verts[i2][1] * rgt[2] + p->verts[i2][2] * top[2] + ent->origin[2]);
          // normals, brush models don't have normals
          tri->v0.n0 = t->gloss ? MIN(qs_data.tex_cnt-1, t->gloss->texnum) : 0;
          tri->v0.n1 = t->norm  ? MIN(qs_data.tex_cnt-1, t->norm->texnum ) : 0;
          tri->v1.n0 = 0xffff; // mark as brush model
          tri->v1.n1 = 0xffff;
          tri->v2.n0 = 0xffff;
          tri->v2.n1 = 0xffff;
          if(surf->texinfo->texture->gltexture)
          { // texture st coordinates
            tri->v0.s = float_to_half(p->verts[i0][3]);
            tri->v0.t = float_to_half(p->verts[i0][4]);
            tri->v1.s = float_to_half(p->verts[i1][3]);
            tri->v1.t = float_to_half(p->verts[i1][4]);
            tri->v2.s = float_to_half(p->verts[i2][3]);
            tri->v2.t = float_to_half(p->verts[i2][4]);
            // texture ids for albedo + fullbright
            tri->v0.tex = MIN(qs_data.tex_cnt-1, t->gltexture->texnum);
            tri->v1.tex = t->fullbright ? MIN(qs_data.tex_cnt-1, t->fullbright->texnum) : 0;
            tri->v2.tex = 0;
            // max textures is 4096 (12 bit) and we have 16. so we can put 4 bits worth of flags here:
            uint32_t flags = 0;
            if(surf->flags & SURF_DRAWLAVA)  flags = 1;
            if(surf->flags & SURF_DRAWSLIME) flags = 2;
            if(surf->flags & SURF_DRAWTELE)  flags = 3;
            if(surf->flags & SURF_DRAWWATER) flags = 4;
            // hack for ad_tears and emissive waterfalls
            if(strstr(t->gltexture->name, "wfall")) flags = 7;// ext[14*pi+13] = t->gltexture->texnum;
#if WATER_MODE==WATER_MODE_FULL
            if(wateroffset)                  flags = 5; // this is our procedural water lower mark
#endif
            // if(surf->flags & SURF_DRAWSKY)   flags = 6; // could do this too
            tri->v1.tex |= flags << 12;
            uint32_t ai = CLAMP(0, (ent->alpha - 1.0)/254.0 * 15, 15); // alpha in 4 bits
            if(!ent->alpha) ai = 15;
            // TODO: 0 means default, 1 means invisible, 255 is opaque, 2--254 is really applicable
            // TODO: default means  map_lavaalpha > 0 ? map_lavaalpha : map_wateralpha
            // TODO: or "slime" or "tele" instead of "lava"
            tri->v0.tex |= ai << 12;
          }
          if(surf->flags & SURF_DRAWSKY) tri->v0.tex = 0xfff;
        }
        nvtx += 3*(p->numverts-2);
#if WATER_MODE==WATER_MODE_FULL
        if(!wateroffset && (surf->flags & SURF_DRAWWATER))
        { // TODO: and normal points the right way?
          wateroffset = 1;
          goto again;
        }
#endif
        // p = p->next;
        p = 0; // XXX
      }
    }
  }
  else if(m->type == mod_sprite)
  { // explosions, decals, etc, this is R_DrawSpriteModel
    vec3_t      point, v_forward, v_right, v_up;
    msprite_t   *psprite;
    mspriteframe_t  *frame;
    float     *s_up, *s_right;
    float     angle, sr, cr;
    float     scale = 1.0f;// XXX newer quakespasm has this: ENTSCALE_DECODE(ent->scale);

    vec3_t vpn, vright, vup, r_origin;
    VectorCopy (r_refdef.vieworg, r_origin);
    AngleVectors (r_refdef.viewangles, vpn, vright, vup);

    frame = R_GetSpriteFrame(ent);
    psprite = (msprite_t *) ent->model->cache.data;

    switch(psprite->type)
    {
      case SPR_VP_PARALLEL_UPRIGHT: //faces view plane, up is towards the heavens
        v_up[0] = 0;
        v_up[1] = 0;
        v_up[2] = 1;
        s_up = v_up;
        s_right = vright;
        break;
      case SPR_FACING_UPRIGHT: //faces camera origin, up is towards the heavens
        VectorSubtract(ent->origin, r_origin, v_forward);
        v_forward[2] = 0;
        VectorNormalizeFast(v_forward);
        v_right[0] = v_forward[1];
        v_right[1] = -v_forward[0];
        v_right[2] = 0;
        v_up[0] = 0;
        v_up[1] = 0;
        v_up[2] = 1;
        s_up = v_up;
        s_right = v_right;
        break;
      case SPR_VP_PARALLEL: //faces view plane, up is towards the top of the screen
        s_up = vup;
        s_right = vright;
        break;
      case SPR_ORIENTED: //pitch yaw roll are independent of camera
        AngleVectors (ent->angles, v_forward, v_right, v_up);
        s_up = v_up;
        s_right = v_right;
        break;
      case SPR_VP_PARALLEL_ORIENTED: //faces view plane, but obeys roll value
        angle = ent->angles[ROLL] * M_PI_DIV_180;
        sr = sin(angle);
        cr = cos(angle);
        v_right[0] = vright[0] * cr + vup[0] * sr;
        v_right[1] = vright[1] * cr + vup[1] * sr;
        v_right[2] = vright[2] * cr + vup[2] * sr;
        v_up[0] = vright[0] * -sr + vup[0] * cr;
        v_up[1] = vright[1] * -sr + vup[1] * cr;
        v_up[2] = vright[2] * -sr + vup[2] * cr;
        s_up = v_up;
        s_right = v_right;
        break;
      default:
        return;
    }

    int numv = 3*4; // add three quads
    if(*vtx_cnt + nvtx + numv >= MAX_VTX_CNT) return;
    for(int k=0;k<3;k++)
    {
      float vert[4][3];
      vec3_t front;
      CrossProduct(s_up, s_right, front);
      VectorMA (ent->origin, frame->down * scale, k == 1 ? front : s_up,    point);
      VectorMA (point, frame->left * scale,       k == 2 ? front : s_right, point);
      for(int l=0;l<3;l++) vert[0][l] = point[l];

      VectorMA (ent->origin, frame->up * scale, k == 1 ? front : s_up, point);
      VectorMA (point, frame->left * scale,     k == 2 ? front : s_right, point);
      for(int l=0;l<3;l++) vert[1][l] = point[l];

      VectorMA (ent->origin, frame->up * scale, k == 1 ? front : s_up,    point);
      VectorMA (point, frame->right * scale,    k == 2 ? front : s_right, point);
      for(int l=0;l<3;l++) vert[2][l] = point[l];

      VectorMA (ent->origin, frame->down * scale, k == 1 ? front : s_up,    point);
      VectorMA (point, frame->right * scale,      k == 2 ? front : s_right, point);
      for(int l=0;l<3;l++) vert[3][l] = point[l];

      if(vtx)
      {
        rtgeo_tri_t *tri = ((rtgeo_tri_t*)vtx)+nvtx/3;
        tri[0] = (rtgeo_tri_t){0};
        tri[1] = (rtgeo_tri_t){0};
        tri[0].v0.x = float_to_half(vert[0][0]);
        tri[0].v0.y = float_to_half(vert[0][1]);
        tri[0].v0.z = float_to_half(vert[0][2]);
        tri[0].v1.x = float_to_half(vert[1][0]);
        tri[0].v1.y = float_to_half(vert[1][1]);
        tri[0].v1.z = float_to_half(vert[1][2]);
        tri[0].v2.x = float_to_half(vert[2][0]);
        tri[0].v2.y = float_to_half(vert[2][1]);
        tri[0].v2.z = float_to_half(vert[2][2]);
        tri[1].v0.x = float_to_half(vert[2][0]);
        tri[1].v0.y = float_to_half(vert[2][1]);
        tri[1].v0.z = float_to_half(vert[2][2]);
        tri[1].v1.x = float_to_half(vert[3][0]);
        tri[1].v1.y = float_to_half(vert[3][1]);
        tri[1].v1.z = float_to_half(vert[3][2]);
        tri[1].v2.x = float_to_half(vert[0][0]);
        tri[1].v2.y = float_to_half(vert[0][1]);
        tri[1].v2.z = float_to_half(vert[0][2]);

        float n[3], e0[] = {
          vert[2][0] - vert[0][0],
          vert[2][1] - vert[0][1],
          vert[2][2] - vert[0][2]}, e1[] = {
          vert[1][0] - vert[0][0],
          vert[1][1] - vert[0][1],
          vert[1][2] - vert[0][2]};
        cross(e0, e1, n);
        encode_normal(&tri[0].v0.n0, n);
        encode_normal(&tri[0].v1.n0, n);
        encode_normal(&tri[0].v2.n0, n);
        encode_normal(&tri[1].v0.n0, n);
        encode_normal(&tri[1].v1.n0, n);
        encode_normal(&tri[1].v2.n0, n);
        tri[0].v0.s = float_to_half(0);
        tri[0].v0.t = float_to_half(1);
        tri[0].v1.s = float_to_half(0);
        tri[0].v1.t = float_to_half(0);
        tri[0].v2.s = float_to_half(1);
        tri[0].v2.t = float_to_half(0);
        tri[1].v0.s = float_to_half(0);
        tri[1].v0.t = float_to_half(1);
        tri[1].v1.s = float_to_half(1);
        tri[1].v1.t = float_to_half(0);
        tri[1].v2.s = float_to_half(1);
        tri[1].v2.t = float_to_half(1);
        if(frame->gltexture)
        {
          tri[0].v0.tex = MIN(qs_data.tex_cnt-1, frame->gltexture->texnum);
          tri[0].v1.tex = MIN(qs_data.tex_cnt-1, frame->gltexture->texnum); // sprites always emit
          tri[1].v0.tex = MIN(qs_data.tex_cnt-1, frame->gltexture->texnum);
          tri[1].v1.tex = MIN(qs_data.tex_cnt-1, frame->gltexture->texnum);
        }
      }
      nvtx += 6;
    } // end three axes
  } // end sprite model
  *vtx_cnt += nvtx;
}

void modify_roi_out(
    dt_graph_t  *graph,
    dt_module_t *mod)
{
  const int p_wd = dt_module_param_int(mod, dt_module_get_param(mod->so, dt_token("wd")))[0];
  const int p_ht = dt_module_param_int(mod, dt_module_get_param(mod->so, dt_token("ht")))[0];
  // int wd = 1920, ht = 1080;
  int wd = p_wd, ht = p_ht;
  mod->connector[0].roi.scale   = 1.0;
  mod->connector[0].roi.full_wd = wd;
  mod->connector[0].roi.full_ht = ht;
  mod->connector[2].roi.scale   = 1.0;
  mod->connector[2].roi.full_wd = wd;
  mod->connector[2].roi.full_ht = ht;
  mod->img_param = (dt_image_params_t) {
    .black          = {0, 0, 0, 0},
    .white          = {65535,65535,65535,65535},
    .whitebalance   = {1.0, 1.0, 1.0, 1.0},
    .filters        = 0, // anything not 0 or 9 will be bayer starting at R
    .crop_aabb      = {0, 0, wd, ht},
    .cam_to_rec2020 = {1, 0, 0, 0, 1, 0, 0, 0, 1},
    .snd_samplerate = 44100,
    .snd_format     = 2, // SND_PCM_FORMAT_S16_LE
    .snd_channels   = 2, // stereo
    .noise_a        = 1.0,
    .noise_b        = 0.0,
    .orientation    = 0,
  };
  mod->connector[5].roi = mod->connector[0].roi; // debug connector
}

dt_graph_run_t
check_params(
    dt_module_t *module,
    uint32_t     parid,
    uint32_t     num,
    void        *oldval)
{
  if(parid == 12 || parid == 13)
  { // dimensions wd or ht
    int oldsz = *(int*)oldval;
    int newsz = dt_module_param_int(module, parid)[0];
    if(oldsz != newsz) return s_graph_run_all; // we need to update the graph topology
  }
  return s_graph_run_record_cmd_buf; // minimal parameter upload to uniforms
}

void commit_params(
    dt_graph_t  *graph,
    dt_module_t *module)
{
  // print these interesting messages from the map:
  graph->gui_msg = con_lastcenterstring;
  qs_data_t *d = module->data;
  int sv_player_set = 0;
  // float *f = (float *)module->committed_param;
  const int p_pause = dt_module_param_int(module, dt_module_get_param(module->so, dt_token("pause")))[0];
  if(!p_pause || graph->frame < p_pause)
  {
    double newtime = Sys_DoubleTime();
    double time = ((d->oldtime == 0) || p_pause) ? 1.0/60.0 : newtime - d->oldtime;
    Host_Frame(time);
    R_SetupView(); // init some left/right vectors also used for sound
    d->oldtime = newtime;
    // sv_player_set = 1; // we'll always set ourselves, Host_Frame is unreliable it seems.
  }

  if(graph->frame == 0)
  { // careful to only do this at == 0 so sv_player (among others) will not crash
    const char *p_exec = dt_module_param_string(module, dt_module_get_param(module->so, dt_token("exec")));
    if(p_exec[0])
    {
      char buf[256] = {0}, *p = 0, *q = buf;
      strncpy(buf, p_exec, sizeof(buf)-1);
      while((p = strstr(q, ";")))
      {
        p[0] = 0;
        Cmd_ExecuteString(q, src_command);
        Host_Frame(1.0/60.0); // this is required so some stuff is picked up (like sprite spawning for 'game ad' changes)
        q = ++p;
      }
      if(q[0]) Cmd_ExecuteString(q, src_command);
      sv_player_set = 0; // just in case we loaded a map (demo, savegame)
    }
  }

  // hijacked for performance counter rendering
  float *p_duration = (float *)dt_module_param_float(module, dt_module_get_param(module->so, dt_token("spp")));
  p_duration[0] = graph->query[graph->double_buffer^1].last_frame_duration;

  // set sv_player. this has to be done if we're not calling Host_Frame after a map reload
  client_t *host_client = svs.clients;
  for (int i=0;i<svs.maxclients && !sv_player_set; i++, host_client++)
  {
    if (!host_client->active) continue;
    sv_player = host_client->edict;
    sv_player_set = 1;
  }
  if(sv_player_set)
  {
    if(sv_player->v.weapon == 1) // shotgun has torch built in:
      ((int *)dt_module_param_int(module, dt_module_get_param(module->so, dt_token("torch"))))[0] = 1;
    else
      ((int *)dt_module_param_int(module, dt_module_get_param(module->so, dt_token("torch"))))[0] = 0;
    if(sv_player->v.waterlevel >= 3) // apply crazy underwater effect
      ((int *)dt_module_param_int(module, dt_module_get_param(module->so, dt_token("water"))))[0] = 1;
    else
      ((int *)dt_module_param_int(module, dt_module_get_param(module->so, dt_token("water"))))[0] = 0;
    int *p_health = (int *)dt_module_param_int(module, dt_module_get_param(module->so, dt_token("health")));
    p_health[0] = sv_player->v.health;
    int *p_armor = (int *)dt_module_param_int(module, dt_module_get_param(module->so, dt_token("armor")));
    p_armor[0] = sv_player->v.armorvalue;
    if((graph->frame % 20) == 0) p_duration[0] = VectorLength(sv_player->v.velocity);
  }

  int *sky = (int *)dt_module_param_int(module, dt_module_get_param(module->so, dt_token("skybox")));
  for(int i=0;i<6;i++) sky[i] = qs_data.skybox[i];

  ((float *)dt_module_param_float(module, dt_module_get_param(module->so, dt_token("cltime"))))[0] = cl.time;
  float *p_cam = (float *)dt_module_param_float(module, dt_module_get_param(module->so, dt_token("cam")));
  float *p_fog = (float *)dt_module_param_float(module, dt_module_get_param(module->so, dt_token("fog")));
  float *qf = Fog_GetColor();
  p_fog[0] = qf[0]; p_fog[1] = qf[1]; p_fog[2] = qf[2];
  p_fog[3] = Fog_GetDensity();
  p_fog[3] *= p_fog[3]; // quake provides sqrt of collision coefficients, dunno why
#if 0 // our camera
  // put back to params:
  float fwd[] = {p_cam[4], p_cam[5], p_cam[6]};
  float top[] = {0, 0, 1};
  float rgt[3]; cross(top, fwd, rgt);
  float vel = 3.0f;
  if(d->move & (1<<0)) for(int k=0;k<3;k++) p_cam[k] += vel * fwd[k];
  if(d->move & (1<<1)) for(int k=0;k<3;k++) p_cam[k] -= vel * fwd[k];
  if(d->move & (1<<2)) for(int k=0;k<3;k++) p_cam[k] += vel * rgt[k];
  if(d->move & (1<<3)) for(int k=0;k<3;k++) p_cam[k] -= vel * rgt[k];
  if(d->move & (1<<4)) for(int k=0;k<3;k++) p_cam[k] += vel * top[k];
  if(d->move & (1<<5)) for(int k=0;k<3;k++) p_cam[k] -= vel * top[k];
#else // quake camera
  float fwd[3], rgt[3], top[3];
  AngleVectors (r_refdef.viewangles, fwd, rgt, top);
  for(int k=0;k<3;k++) p_cam[k]   = r_refdef.vieworg[k];
  for(int k=0;k<3;k++) p_cam[4+k] = fwd[k];
  for(int k=0;k<3;k++) p_cam[8+k] = top[k];
#endif
}

int read_source(
    dt_module_t             *mod,
    void                    *mapped,
    dt_read_source_params_t *p)
{
  qs_data_t *d = mod->data;

  if(p->node->kernel == dt_token("tex") && p->a < d->tex_cnt)
  { // upload texture array
    memcpy(mapped, d->tex[p->a], sizeof(uint32_t)*d->tex_dim[2*p->a]*d->tex_dim[2*p->a+1]);
    p->node->flags &= ~s_module_request_read_source; // done uploading textures
    d->tex_req[p->a] = 0;
  }
  uint32_t vtx_cnt = 0;
  uint16_t *m16 = mapped;
  if(p->node->kernel == dt_token("dyngeo"))
  {
    // add_geo(cl_entities+cl.viewentity, p->vtx + 3*vtx_cnt, p->idx + idx_cnt, p->ext + 7*(idx_cnt/3), &vtx_cnt); // player model
    add_geo(&cl.viewent, m16 + 3*vtx_cnt, &vtx_cnt); // weapon
    for(int i=0;i<cl_numvisedicts;i++)
      add_geo(cl_visedicts[i], m16 + 3*vtx_cnt, &vtx_cnt);
    for (int i=0; i<cl.num_statics; i++)
      add_geo(cl_static_entities+i, m16 + 3*vtx_cnt, &vtx_cnt);
    add_particles(m16 + 3*vtx_cnt, &vtx_cnt);
  }
  else if(p->node->kernel == dt_token("stcgeo"))
  {
    add_geo(cl_entities+0, m16 + 3*vtx_cnt, &vtx_cnt);
    // if(!qs_data.worldspawn) p->node->flags &= ~s_module_request_read_geo; // done uploading static geo for now
    if(!qs_data.worldspawn) p->node->flags &= ~s_module_request_read_source; // done uploading static geo for now
#if 0 // debug: quake aabb are in +-4096
    float aabb[6] = {FLT_MAX,FLT_MAX,FLT_MAX, -FLT_MAX,-FLT_MAX,-FLT_MAX};
    for(int i=0;i<vtx_cnt;i++) 
    {
      aabb[0] = MIN(aabb[0], p->vtx[3*i+0]);
      aabb[1] = MIN(aabb[1], p->vtx[3*i+1]);
      aabb[2] = MIN(aabb[2], p->vtx[3*i+2]);
      aabb[3] = MAX(aabb[3], p->vtx[3*i+0]);
      aabb[4] = MAX(aabb[4], p->vtx[3*i+1]);
      aabb[5] = MAX(aabb[5], p->vtx[3*i+2]);
    }
    fprintf(stderr, "XXX static geo bounds %g %g %g -- %g %g %g\n",
        aabb[0], aabb[1], aabb[2], aabb[3], aabb[4], aabb[5]);
#endif
  }
  return 0;
}

void
create_nodes(
    dt_graph_t  *graph,
    dt_module_t *module)
{
  qs_data_t *d = module->data;

  // ray tracing kernel:
  int id_rt = dt_node_add(graph, module, "quake", "main", 
    module->connector[0].roi.wd, module->connector[0].roi.ht, 1, 0, 0, 9,
      "output",   "write", "rgba", "f32",  &module->connector[0].roi, // 0
      "tex",      "read",  "*",    "*",    dt_no_roi,                 // 1
      "aov",      "write", "rgba", "f16",  &module->connector[0].roi, // 2
      "nee_in",   "read",  "rgba", "ui32", dt_no_roi,                 // 3
      "nee_out",  "write", "rgba", "ui32", &module->connector[0].roi, // 4
      "mv",       "read",  "rg",   "f16",  dt_no_roi,                 // 5
      "gbuf_in",  "read",  "rgba", "f32",  dt_no_roi,                 // 6
      "gbuf_out", "write", "rgba", "f32",  &module->connector[0].roi, // 7
      "debug",    "write", "rgba", "f16",  &module->connector[0].roi);// 8
      // "oldout",   "read",  "*",    "*",    &module->connector[0].roi);// 9
  graph->module[id_rt].connector[4].flags |= s_conn_clear;
  graph->module[id_rt].connector[7].flags |= s_conn_clear;

  assert(graph->num_nodes < graph->max_nodes);
  const uint32_t id_tex = graph->num_nodes++;
  graph->node[id_tex] = (dt_node_t) {
    .name   = dt_token("quake"),
    .kernel = dt_token("tex"),
    .module = module,
    .wd     = 1,
    .ht     = 1,
    .dp     = 1,
    .flags  = s_module_request_read_source,
    .num_connectors = 1,
    .connector = {{
      .name   = dt_token("tex"),
      .type   = dt_token("source"),
      .chan   = dt_token("rgba"),
      .format = dt_token("ui8"),
      .roi    = {
        .scale   = 1.0,
        .wd      = d->tex_maxw,
        .ht      = d->tex_maxh,
        .full_wd = d->tex_maxw,
        .full_ht = d->tex_maxh,
      },
      .array_length = MAX_GLTEXTURES, // d->tex_cnt,
      .array_dim    = d->tex_dim,
      .array_req    = d->tex_req,
      .flags        = s_conn_dynamic_array | s_conn_feedback, // mark as dynamic allocation suitable for multi-frame processing (double buffered)
      .array_alloc_size = 1500<<20, // something enough for quake
    }},
  };
  // in case quake was already inited but we are called again to create nodes,
  // we'll also need to re-upload all textures. flag them here:
  for(int i=0;i<d->tex_cnt;i++)
    if(d->tex[i]) d->tex_req[i] = 1;

  uint32_t vtx_cnt = 0;
  // we'll statically assign a global buffer size here because we want to avoid a fresh
  // node creation/memory allocation pass. reallocation usually invalidates *all* buffers
  // requiring fresh data upload for everything.
  // i suppose the core allocator might need support for incremental additions otherwise.
  vtx_cnt = MAX_VTX_CNT;

  dt_roi_t roi_geo = { .scale = 1.0, .wd = vtx_cnt/3, .ht = 3*8, .full_wd = vtx_cnt/3, .full_ht = 3*8 };
  const uint32_t id_dyngeo = dt_node_add(graph, module, "quake", "dyngeo", 1, 1, 1, 0, 0, 1,
    "dyngeo", "source", "ssbo", "f16", &roi_geo);
  graph->node[id_dyngeo].flags = s_module_request_read_geo | s_module_request_read_source;

  // the static geometry we count. this means that we'll need to re-create nodes on map change.
  vtx_cnt = 0;
  add_geo(cl_entities+0, 0, &vtx_cnt);
  fprintf(stderr, "[create_nodes] static vertex count %u\n", vtx_cnt);
  vtx_cnt = MAX(3, vtx_cnt); // avoid crash for not initialised model

  roi_geo = (dt_roi_t){ .scale = 1.0, .wd = vtx_cnt/3, .ht = 3*8, .full_wd = vtx_cnt/3, .full_ht = 3*8 };
  const uint32_t id_stcgeo = dt_node_add(graph, module, "quake", "stcgeo", 1, 1, 1, 0, 0, 1,
    "stcgeo", "source", "ssbo", "f16", &roi_geo);
  graph->node[id_stcgeo].flags = s_module_request_read_geo | s_module_request_read_source;

  const uint32_t id_dynbvh = dt_node_add(graph, module, "bvh", "bvh", 1, 1, 1, 0, 0, 1,
    "geo", "sink", "ssbo", "f16", dt_no_roi);
  const uint32_t id_stcbvh = dt_node_add(graph, module, "bvh", "bvh", 1, 1, 1, 0, 0, 1,
    "geo", "sink", "ssbo", "f16", dt_no_roi);
  CONN(dt_node_connect(graph, id_dyngeo, 0, id_dynbvh, 0));
  CONN(dt_node_connect(graph, id_stcgeo, 0, id_stcbvh, 0));

  CONN(dt_node_connect(graph, id_tex, 0, id_rt, 1));
  CONN(dt_node_feedback(graph, id_rt, 4, id_rt, 3)); // nee cache
  CONN(dt_node_feedback(graph, id_rt, 7, id_rt, 6)); // gbuf
  dt_connector_copy(graph, module, 0, id_rt, 0); // wire output buffer
  dt_connector_copy(graph, module, 2, id_rt, 2); // output aov image
  dt_connector_copy(graph, module, 3, id_rt, 5); // motion vectors from outside
  dt_connector_copy(graph, module, 4, id_rt, 7); // gbuf output (n, d, mu_1, mu_2)
  dt_connector_copy(graph, module, 5, id_rt, 8); // wire debug output

  // propagate up so things will start to move at all at the node level:
  module->flags = s_module_request_read_geo;
}

int audio(
    dt_module_t  *mod,
    uint64_t      unused_sample_beg,
    uint32_t      unused_sample_cnt,
    uint8_t     **samples)
{
  qs_data_t *dat = mod->data;
  if(dat->worldspawn)
  {
    mod->graph->gui_msg = 0;
    mod->flags = s_module_request_all; // TODO: move somewhere else so we can run without audio (from cmdline!). needs to be picked up though!
    key_dest = key_game;
    m_state = m_none;
    IN_Activate();
    for(int k=0;k<10;k++) Host_Frame(1.0/60.0); // prewarm/avoid multi-frame init problems
    dat->worldspawn = 0;
  }
  int buffersize = shm->samples * (shm->samplebits / 8);
  int pos = (shm->samplepos * (shm->samplebits / 8));
  int tobufend = buffersize - pos; // bytes to buffer end
  *samples = shm->buffer + pos; // start position

  // sync: keep old buffer pos, check times, submit only what we need
  static double oldtime = 0.0;
  double time = Sys_DoubleTime();
  double newtime = time;
  if(newtime > oldtime + 30.0/60.0) oldtime = 0.0;
  if(oldtime == 0.0)
    oldtime = newtime - 1.0/60.0; // assume some fps
  // compute number of samples and then bytes required for this timeslot
  int len = ((int)(shm->speed * (time - oldtime))) * (shm->samplebits / 8) * shm->channels;
  if(len > tobufend)
  { // clipped at buffer end?
    shm->samplepos = 0;    // wrap around
    len = 4*(tobufend/4);  // clip number of samples we send to multiple of bytes/sample
    newtime = ((double)len) / shm->channels / (shm->samplebits / 8) / shm->speed + oldtime; // also clip time
  }
  else shm->samplepos += len / (shm->samplebits / 8);
  oldtime = newtime;
  return len/4; // return number of samples (compute from byte size /4: stereo and 16 bit)
}
