#pragma once
#include <pspgu.h>
#include <pspmath.h>

// Basic sceGu wrapper

bool PSPRenderer_Init();
void PSPRenderer_BeginFrame(uint32_t skyColor);
void PSPRenderer_SetCamera(const ScePspFVector3 *eye,
                           const ScePspFVector3 *center);

void PSPRenderer_GetViewProjMatrix(ScePspFMatrix4 *outVP);

struct PSPRendererDebugInfo {
  uint32_t fbp0;
  uint32_t fbp1;
  uint32_t zbp;
  uint32_t drawBuffer;
  int bufWidth;
  int scrWidth;
  int scrHeight;
};

void PSPRenderer_GetDebugInfo(PSPRendererDebugInfo *outInfo);
void PSPRenderer_DialogSwapBuffers();

void *PSPRenderer_EndFrame();
void PSPRenderer_Shutdown();
