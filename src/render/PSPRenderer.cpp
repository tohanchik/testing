// PSPRenderer.cpp - sceGu wrapper

#include "PSPRenderer.h"
#include <malloc.h>
#include <pspdebug.h>
#include <pspdisplay.h>
#include <pspgu.h>
#include <pspgum.h>
#include <pspkernel.h>
#include <string.h>
#include <math.h>
#include <stdint.h>

#define BUF_WIDTH 512
#define SCR_WIDTH 480
#define SCR_HEIGHT 272

// Display list - must be in uncached RAM
static unsigned int __attribute__((aligned(16))) g_list[262144];

// Frame + depth buffers in VRAM
static void *g_fbp0; // frame buffer 0
static void *g_fbp1; // frame buffer 1
static void *g_zbp;  // depth buffer
static void *g_drawBuffer; // currently configured GU draw buffer

// VRAM offset calculation
static void *vrelptr(unsigned int offset) {
  return (void *)(0x44000000 + offset);
}

static inline void *vramOffsetFromPtr(void *ptr) {
  return (void *)((uintptr_t)ptr - (uintptr_t)0x44000000);
}

bool PSPRenderer_Init() {
  // Frame buffer: 512*272*4 bytes = 557056 bytes
  g_fbp0 = vrelptr(0);
  g_fbp1 = vrelptr(BUF_WIDTH * SCR_HEIGHT * 4);
  g_zbp = vrelptr(2 * BUF_WIDTH * SCR_HEIGHT * 4);
  g_drawBuffer = g_fbp0;

  sceGuInit();
  sceGuStart(GU_DIRECT, g_list);

  // Display
  sceGuDrawBuffer(GU_PSM_8888, (void *)0, BUF_WIDTH);
  sceGuDispBuffer(SCR_WIDTH, SCR_HEIGHT, (void *)(BUF_WIDTH * SCR_HEIGHT * 4), BUF_WIDTH);
  sceGuDepthBuffer((void *)(2 * BUF_WIDTH * SCR_HEIGHT * 4), BUF_WIDTH);

  sceGuOffset(2048 - (SCR_WIDTH / 2), 2048 - (SCR_HEIGHT / 2));
  // Match Revival: no overscan, exact screen dimensions
  sceGuViewport(2048, 2048, SCR_WIDTH, SCR_HEIGHT);
  // Hardware clip depth mapping (restricted to avoid VFPU clipping bugs at boundaries)
  sceGuDepthRange(50000, 10000);

  sceGuScissor(0, 0, SCR_WIDTH, SCR_HEIGHT);
  sceGuEnable(GU_SCISSOR_TEST);

  // Depth test
  sceGuDepthFunc(GU_GEQUAL);
  sceGuEnable(GU_DEPTH_TEST);

  // Textures
  sceGuEnable(GU_TEXTURE_2D);
  sceGuTexFunc(GU_TFX_MODULATE, GU_TCC_RGBA); // vertex color * texture = final
  sceGuTexFilter(GU_NEAREST, GU_NEAREST);     // pixel-art style

  // Smooth shading
  sceGuShadeModel(GU_SMOOTH);

  sceGuEnable(GU_CLIP_PLANES);

  // Backface culling on (fixes VFPU screen-corner clipping bug)
  sceGuFrontFace(GU_CCW);
  sceGuEnable(GU_CULL_FACE);

  // Alpha test (for plants/transparents)
  sceGuAlphaFunc(GU_GREATER, 0x80, 0xFF);
  sceGuEnable(GU_ALPHA_TEST);

  sceGuFog(32.0f, 64.0f, 0xFFFFB267);
  sceGuEnable(GU_FOG);

  sceGuFinish();
  sceGuSync(0, 0);

  sceDisplayWaitVblankStart();
  sceGuDisplay(GU_TRUE);

  return true;
}

void PSPRenderer_BeginFrame(uint32_t skyColor) {
  sceGuStart(GU_DIRECT, g_list);

  // Re-bind draw target every frame.
  // On real PSP this avoids occasional front/back buffer desync artifacts
  // after dialog/buffer transitions, which can look like frame flicker/ghosting.
  sceGuDrawBufferList(GU_PSM_8888, vramOffsetFromPtr(g_drawBuffer), BUF_WIDTH);

  // Clear to sky color
  sceGuClearColor(skyColor);
  sceGuClearDepth(0);
  sceGuClear(GU_COLOR_BUFFER_BIT | GU_DEPTH_BUFFER_BIT);

  // Update fog to match sky color dynamically
  sceGuFog(32.0f, 64.0f, skyColor);

  sceGumMatrixMode(GU_PROJECTION);
  sceGumLoadIdentity();

  sceGumPerspective(90.0f, (float)SCR_WIDTH / (float)SCR_HEIGHT, 0.2, 256.0f);

  // Ensure backface culling is ON for the terrain (fixes PSP clipping bug)
  sceGuEnable(GU_CULL_FACE);

  sceGumMatrixMode(GU_VIEW);
  sceGumLoadIdentity();

  sceGumMatrixMode(GU_MODEL);
  sceGumLoadIdentity();
}

void PSPRenderer_SetCamera(const ScePspFVector3 *eye,
                           const ScePspFVector3 *center) {
  sceGumMatrixMode(GU_VIEW);
  sceGumLoadIdentity();
  ScePspFVector3 up = {0.0f, 1.0f, 0.0f};
  sceGumLookAt(const_cast<ScePspFVector3 *>(eye),
               const_cast<ScePspFVector3 *>(center), &up);
  sceGumUpdateMatrix();
  sceGumMatrixMode(GU_MODEL);
}

void PSPRenderer_GetViewProjMatrix(ScePspFMatrix4 *outVP) {
  ScePspFMatrix4 proj, view;

  sceGumMatrixMode(GU_PROJECTION);
  sceGumStoreMatrix(&proj);

  sceGumMatrixMode(GU_VIEW);
  sceGumStoreMatrix(&view);

  // P * V
  gumMultMatrix(outVP, &proj, &view);

  // Restore
  sceGumMatrixMode(GU_MODEL);
}

void PSPRenderer_GetDebugInfo(PSPRendererDebugInfo *outInfo) {
  if (!outInfo) return;
  outInfo->fbp0 = (uint32_t)g_fbp0;
  outInfo->fbp1 = (uint32_t)g_fbp1;
  outInfo->zbp = (uint32_t)g_zbp;
  outInfo->drawBuffer = (uint32_t)g_drawBuffer;
  outInfo->bufWidth = BUF_WIDTH;
  outInfo->scrWidth = SCR_WIDTH;
  outInfo->scrHeight = SCR_HEIGHT;
}

void PSPRenderer_DialogSwapBuffers() {
  sceGuSwapBuffers();
  g_drawBuffer = (g_drawBuffer == g_fbp0) ? g_fbp1 : g_fbp0;
}

void *PSPRenderer_EndFrame() {
  sceGuFinish();
  sceGuSync(0, 0);
  sceDisplayWaitVblankStart();
  sceGuSwapBuffers();

  // After swap, the previously-used draw buffer is the one currently displayed.
  void *shownBuffer = g_drawBuffer;
  g_drawBuffer = (g_drawBuffer == g_fbp0) ? g_fbp1 : g_fbp0;
  return shownBuffer;
}

void PSPRenderer_Shutdown() { sceGuTerm(); }
