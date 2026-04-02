// MinecraftPSP - main.cpp
// PSP Entry point, basic game loop

#include <pspctrl.h>
#include <pspdebug.h>
#include <pspdisplay.h>
#include <pspgu.h>
#include <pspgum.h>
#include <pspiofilemgr.h>
#include <pspkernel.h>
#include <psppower.h>
#include <psputility.h>
#include <psputility_osk.h>

#include "input/PSPInput.h"
#include "render/BlockHighlight.h"
#include "render/ChunkRenderer.h"
#include "render/CloudRenderer.h"
#include "world/Level.h"
#include "world/AABB.h"
#include "render/PSPRenderer.h"
#include "render/SkyRenderer.h"
#include "render/TextureAtlas.h"
#include "world/Blocks.h"
#include "world/Mth.h"
#include "world/Random.h"
#include "world/Raycast.h"
#include "game/CreativeInventory.h"
#include <ctype.h>
#include <math.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

// PSP module metadata
PSP_MODULE_INFO("MinecraftPSP", PSP_MODULE_USER, 1, 0);
PSP_MAIN_THREAD_ATTR(PSP_THREAD_ATTR_USER | PSP_THREAD_ATTR_VFPU);
PSP_HEAP_SIZE_KB(-1024); // Use all available RAM minus 1MB for the kernel

// Exit callback (HOME button)
int exit_callback(int arg1, int arg2, void *common) {
  sceKernelExitGame();
  return 0;
}

int callback_thread(SceSize args, void *argp) {
  int cbid = sceKernelCreateCallback("Exit Callback", exit_callback, NULL);
  sceKernelRegisterExitCallback(cbid);
  sceKernelSleepThreadCB();
  return 0;
}

void setup_callbacks() {
  int thid = sceKernelCreateThread("update_thread", callback_thread, 0x11,
                                   0xFA0, PSP_THREAD_ATTR_USER, NULL);
  if (thid >= 0)
    sceKernelStartThread(thid, 0, NULL);
}

// Player state
struct PlayerState {
  float x, y, z;          // position
  float yaw, pitch;       // camera rotation (degrees)
  float velY;             // vertical velocity (gravity)
  bool onGround;
  bool isFlying;          // creative flight active
  float jumpDoubleTapTimer; // countdown for double-tap detection
};

// Global state
static PlayerState g_player;
static Level *g_level = nullptr;
static SkyRenderer *g_skyRenderer = nullptr;
static CloudRenderer *g_cloudRenderer = nullptr;
static ChunkRenderer *g_chunkRenderer = nullptr;
static TextureAtlas *g_atlas = nullptr;
static SimpleTexture g_guiInvCreativeTex;
static SimpleTexture g_guiCursorTex;
static SimpleTexture g_guiSliderTex;
static SimpleTexture g_guiCellTex;
static RayHit g_hitResult;       // Block the player is currently looking at
static uint8_t g_heldBlock = BLOCK_COBBLESTONE; // Block to place
static CreativeInventory g_creativeInv;
static const char *g_inventoryHoverName = nullptr;
static float g_tickAlpha = 0.0f;
static const char *kWorldDir = "ms0:/PSP/GAME/MinecraftPSP/worlds";
static const char *kInvLayoutCfgPath = "ms0:/PSP/GAME/MinecraftPSP/inv_layout.cfg";
static const char *kInvTuneCfgPath = "ms0:/PSP/GAME/MinecraftPSP/inv_tune_mode.cfg";
static const char *kTexInvCreativePath = "res/gui/inventory_creative.png";
static const char *kTexCursorPath = "res/gui/cursor.png";
static const char *kTexSliderPath = "res/gui/slider.png";
static const char *kTexCellPath = "res/gui/cell.png";
static const int kMaxWorldSlots = 8;
static int g_worldSlot = 0;
static bool g_worldExists[kMaxWorldSlots] = {false};
static bool g_pauseOpen = false;
static int g_pauseSel = 0; // 0 continue, 1 save, 2 save & exit
static bool g_inMainMenu = true;
static int g_menuSlotSel = 0;
static bool g_menuCreateSelected = true;
static bool g_menuDeleteConfirm = false;
static bool g_menuDeleteYesSelected = false;
static char g_statusText[96] = {0};
static float g_statusTimer = 0.0f;
static uint32_t g_statusColor = 0xFFFFFFFF;
static float g_autoSaveTimer = 0.0f;
static const float kAutoSaveIntervalSec = 60.0f;
static bool kEnableInvTuneMode = false; // Runtime-loaded from inv_tune_mode.cfg.
static bool g_invTuneMode = false;
static int g_invTuneTarget = 0; // 0=GRID, 1=HOTBAR, 2=DELETE, 3=TITLE
static int g_lastSunLightBucket = -1;
static float g_invCellStep = 21.300f;
static float g_invStretchX = 0.750f;
static float g_invCompressY = 1.100f;
static float g_invOffsetX = -10.084f;
static float g_invOffsetY = 5.204f;
static float g_invHotbarStepX = 21.300f;
static float g_invHotbarStretchX = 0.800f;
static float g_invHotbarOffsetX = 0.0f;
static float g_invHotbarOffsetY = -13.000f;
static float g_invDeleteOffsetX = -37.874f;
static float g_invDeleteOffsetY = -0.484f;
static float g_invTitleOffsetX = -17.314f;
static float g_invTitleOffsetY = 1.532f;

struct HudColVert {
  uint32_t color;
  float x, y, z;
};

struct HudTexVert {
  float u, v;
  float x, y, z;
};

static inline void hudDrawTexture(SimpleTexture &tex, float x, float y, float w, float h) {
  if (!tex.data || tex.width == 0 || tex.height == 0) return;
  tex.bind();
  sceGuColor(0xFFFFFFFF);
  sceGuTexFunc(GU_TFX_MODULATE, GU_TCC_RGBA);
  sceGuTexFilter(GU_NEAREST, GU_NEAREST);
  HudTexVert *v = (HudTexVert *)sceGuGetMemory(2 * sizeof(HudTexVert));
  v[0].u = 0.5f;            v[0].v = 0.5f;             v[0].x = x;     v[0].y = y;     v[0].z = 0.0f;
  v[1].u = tex.width - 0.5f; v[1].v = tex.height - 0.5f; v[1].x = x + w; v[1].y = y + h; v[1].z = 0.0f;
  sceGuDrawArray(GU_SPRITES, GU_TEXTURE_32BITF | GU_VERTEX_32BITF | GU_TRANSFORM_2D, 2, 0, v);
}

static inline void hudDrawRect(float x, float y, float w, float h, uint32_t abgr) {
  sceGuDisable(GU_TEXTURE_2D);
  HudColVert *v = (HudColVert *)sceGuGetMemory(2 * sizeof(HudColVert));
  v[0].color = abgr; v[0].x = x;     v[0].y = y;     v[0].z = 0.0f;
  v[1].color = abgr; v[1].x = x + w; v[1].y = y + h; v[1].z = 0.0f;
  sceGuDrawArray(GU_SPRITES, GU_COLOR_8888 | GU_VERTEX_32BITF | GU_TRANSFORM_2D, 2, 0, v);
  sceGuEnable(GU_TEXTURE_2D);
}

static inline void hudDrawTile(TextureAtlas *atlas, int tx, int ty, float x, float y, float size) {
  if (!atlas) return;
  atlas->bind();
  // Force neutral vertex color so HUD icons are not tinted by previous draws.
  sceGuColor(0xFFFFFFFF);
  sceGuTexFunc(GU_TFX_MODULATE, GU_TCC_RGBA);
  sceGuTexFilter(GU_NEAREST, GU_NEAREST);
  HudTexVert *v = (HudTexVert *)sceGuGetMemory(2 * sizeof(HudTexVert));
  // 2D sprite path on PSP expects texel-space UVs.
  float u0 = (float)(tx * 16) + 0.5f;
  float v0 = (float)(ty * 16) + 0.5f;
  float us = 15.0f;
  float vs = 15.0f;
  v[0].u = u0;      v[0].v = v0;      v[0].x = x;        v[0].y = y;        v[0].z = 0.0f;
  v[1].u = u0 + us; v[1].v = v0 + vs; v[1].x = x + size; v[1].y = y + size; v[1].z = 0.0f;
  sceGuDrawArray(GU_SPRITES, GU_TEXTURE_32BITF | GU_VERTEX_32BITF | GU_TRANSFORM_2D, 2, 0, v);
}

static inline void hudGetIconTile(uint8_t id, int &tx, int &ty) {
  // Some blocks read better in inventory from side view.
  if (id == BLOCK_LOG || id == BLOCK_BOOKSHELF || id == BLOCK_PUMPKIN) {
    tx = g_blockUV[id].side_x;
    ty = g_blockUV[id].side_y;
  } else {
    tx = g_blockUV[id].top_x;
    ty = g_blockUV[id].top_y;
  }
  if (tx == 0 && ty == 0 && id != BLOCK_GRASS) {
    tx = g_blockUV[id].side_x;
    ty = g_blockUV[id].side_y;
    if (tx == 0 && ty == 0) {
      tx = 1; // stone fallback
      ty = 0;
    }
  }
}

static const char* getBlockDisplayName(uint8_t id) {
  switch (id) {
    case BLOCK_STONE: return "Stone";
    case BLOCK_GRASS: return "Grass";
    case BLOCK_DIRT: return "Dirt";
    case BLOCK_COBBLESTONE: return "Cobblestone";
    case BLOCK_WOOD_PLANK: return "Wood Planks";
    case BLOCK_SAND: return "Sand";
    case BLOCK_GRAVEL: return "Gravel";
    case BLOCK_LOG: return "Log";
    case BLOCK_LEAVES: return "Leaves";
    case BLOCK_GLASS: return "Glass";
    case BLOCK_SANDSTONE: return "Sandstone";
    case BLOCK_WOOL: return "Wool";
    case BLOCK_WOOL_ORANGE: return "Orange Wool";
    case BLOCK_WOOL_MAGENTA: return "Magenta Wool";
    case BLOCK_WOOL_LIGHT_BLUE: return "Light Blue Wool";
    case BLOCK_WOOL_YELLOW: return "Yellow Wool";
    case BLOCK_WOOL_LIME: return "Lime Wool";
    case BLOCK_WOOL_PINK: return "Pink Wool";
    case BLOCK_WOOL_GRAY: return "Gray Wool";
    case BLOCK_WOOL_LIGHT_GRAY: return "Light Gray Wool";
    case BLOCK_WOOL_CYAN: return "Cyan Wool";
    case BLOCK_WOOL_PURPLE: return "Purple Wool";
    case BLOCK_WOOL_BLUE: return "Blue Wool";
    case BLOCK_WOOL_BROWN: return "Brown Wool";
    case BLOCK_WOOL_GREEN: return "Green Wool";
    case BLOCK_WOOL_RED: return "Red Wool";
    case BLOCK_WOOL_BLACK: return "Black Wool";
    case BLOCK_GOLD_BLOCK: return "Gold Block";
    case BLOCK_IRON_BLOCK: return "Iron Block";
    case BLOCK_BRICK: return "Bricks";
    case BLOCK_BOOKSHELF: return "Bookshelf";
    case BLOCK_MOSSY_COBBLE: return "Mossy Cobblestone";
    case BLOCK_OBSIDIAN: return "Obsidian";
    case BLOCK_GLOWSTONE: return "Glowstone";
    case BLOCK_NETHERRACK: return "Netherrack";
    case BLOCK_SOULSAND: return "Soul Sand";
    case BLOCK_PUMPKIN: return "Pumpkin";
    case BLOCK_FLOWER: return "Dandelion";
    case BLOCK_ROSE: return "Rose";
    case BLOCK_SAPLING: return "Sapling";
    case BLOCK_TALLGRASS: return "Tall Grass";
    case BLOCK_WATER_STILL: return "Water";
    case BLOCK_WATER_FLOW: return "Flowing Water";
    case BLOCK_LAVA_STILL: return "Lava";
    case BLOCK_LAVA_FLOW: return "Flowing Lava";
    case BLOCK_GOLD_ORE: return "Gold Ore";
    case BLOCK_IRON_ORE: return "Iron Ore";
    case BLOCK_COAL_ORE: return "Coal Ore";
    case BLOCK_DIAMOND_ORE: return "Diamond Ore";
    case BLOCK_DIAMOND_BLOCK: return "Diamond Block";
    case BLOCK_LAPIS_ORE: return "Lapis Ore";
    case BLOCK_REDSTONE_ORE: return "Redstone Ore";
    case BLOCK_TNT: return "TNT";
    case BLOCK_CHEST: return "Chest";
    case BLOCK_CRAFTING_TABLE: return "Crafting Table";
    case BLOCK_FURNACE: return "Furnace";
    case BLOCK_CACTUS: return "Cactus";
    case BLOCK_SNOW: return "Snow Layer";
    case BLOCK_SNOW_BLOCK: return "Snow Block";
    case BLOCK_ICE: return "Ice";
    case BLOCK_CLAY: return "Clay";
    case BLOCK_REEDS: return "Sugar Cane";
    default: return "Unknown Block";
  }
}

static uint8_t glyphRow5x7(char ch, int row) {
  switch (ch) {
    case 'A': { static const uint8_t r[7] = {0x0E,0x11,0x11,0x1F,0x11,0x11,0x11}; return r[row]; }
    case 'B': { static const uint8_t r[7] = {0x1E,0x11,0x11,0x1E,0x11,0x11,0x1E}; return r[row]; }
    case 'C': { static const uint8_t r[7] = {0x0E,0x11,0x10,0x10,0x10,0x11,0x0E}; return r[row]; }
    case 'D': { static const uint8_t r[7] = {0x1E,0x11,0x11,0x11,0x11,0x11,0x1E}; return r[row]; }
    case 'E': { static const uint8_t r[7] = {0x1F,0x10,0x10,0x1E,0x10,0x10,0x1F}; return r[row]; }
    case 'F': { static const uint8_t r[7] = {0x1F,0x10,0x10,0x1E,0x10,0x10,0x10}; return r[row]; }
    case 'G': { static const uint8_t r[7] = {0x0E,0x11,0x10,0x17,0x11,0x11,0x0E}; return r[row]; }
    case 'H': { static const uint8_t r[7] = {0x11,0x11,0x11,0x1F,0x11,0x11,0x11}; return r[row]; }
    case 'I': { static const uint8_t r[7] = {0x1F,0x04,0x04,0x04,0x04,0x04,0x1F}; return r[row]; }
    case 'K': { static const uint8_t r[7] = {0x11,0x12,0x14,0x18,0x14,0x12,0x11}; return r[row]; }
    case 'L': { static const uint8_t r[7] = {0x10,0x10,0x10,0x10,0x10,0x10,0x1F}; return r[row]; }
    case 'M': { static const uint8_t r[7] = {0x11,0x1B,0x15,0x15,0x11,0x11,0x11}; return r[row]; }
    case 'N': { static const uint8_t r[7] = {0x11,0x11,0x19,0x15,0x13,0x11,0x11}; return r[row]; }
    case 'O': { static const uint8_t r[7] = {0x0E,0x11,0x11,0x11,0x11,0x11,0x0E}; return r[row]; }
    case 'P': { static const uint8_t r[7] = {0x1E,0x11,0x11,0x1E,0x10,0x10,0x10}; return r[row]; }
    case 'Q': { static const uint8_t r[7] = {0x0E,0x11,0x11,0x11,0x15,0x12,0x0D}; return r[row]; }
    case 'R': { static const uint8_t r[7] = {0x1E,0x11,0x11,0x1E,0x14,0x12,0x11}; return r[row]; }
    case 'S': { static const uint8_t r[7] = {0x0F,0x10,0x10,0x0E,0x01,0x01,0x1E}; return r[row]; }
    case 'T': { static const uint8_t r[7] = {0x1F,0x04,0x04,0x04,0x04,0x04,0x04}; return r[row]; }
    case 'U': { static const uint8_t r[7] = {0x11,0x11,0x11,0x11,0x11,0x11,0x0E}; return r[row]; }
    case 'V': { static const uint8_t r[7] = {0x11,0x11,0x11,0x11,0x11,0x0A,0x04}; return r[row]; }
    case 'W': { static const uint8_t r[7] = {0x11,0x11,0x11,0x15,0x15,0x15,0x0A}; return r[row]; }
    case 'X': { static const uint8_t r[7] = {0x11,0x11,0x0A,0x04,0x0A,0x11,0x11}; return r[row]; }
    case 'Y': { static const uint8_t r[7] = {0x11,0x11,0x0A,0x04,0x04,0x04,0x04}; return r[row]; }
    case '0': { static const uint8_t r[7] = {0x0E,0x11,0x13,0x15,0x19,0x11,0x0E}; return r[row]; }
    case '1': { static const uint8_t r[7] = {0x04,0x0C,0x04,0x04,0x04,0x04,0x0E}; return r[row]; }
    case '2': { static const uint8_t r[7] = {0x0E,0x11,0x01,0x02,0x04,0x08,0x1F}; return r[row]; }
    case '3': { static const uint8_t r[7] = {0x1E,0x01,0x01,0x0E,0x01,0x01,0x1E}; return r[row]; }
    case '4': { static const uint8_t r[7] = {0x02,0x06,0x0A,0x12,0x1F,0x02,0x02}; return r[row]; }
    case '5': { static const uint8_t r[7] = {0x1F,0x10,0x10,0x1E,0x01,0x01,0x1E}; return r[row]; }
    case '6': { static const uint8_t r[7] = {0x0E,0x10,0x10,0x1E,0x11,0x11,0x0E}; return r[row]; }
    case '7': { static const uint8_t r[7] = {0x1F,0x01,0x02,0x04,0x08,0x08,0x08}; return r[row]; }
    case '8': { static const uint8_t r[7] = {0x0E,0x11,0x11,0x0E,0x11,0x11,0x0E}; return r[row]; }
    case '9': { static const uint8_t r[7] = {0x0E,0x11,0x11,0x0F,0x01,0x01,0x0E}; return r[row]; }
    case ':': { static const uint8_t r[7] = {0x00,0x04,0x04,0x00,0x04,0x04,0x00}; return r[row]; }
    case '.': { static const uint8_t r[7] = {0x00,0x00,0x00,0x00,0x00,0x06,0x06}; return r[row]; }
    case ' ': return 0;
    default: return 0;
  }
}

static void hudDrawText5x7(float x, float y, const char *text, uint32_t color, float scale) {
  if (!text) return;
  float penX = x;
  for (const char *p = text; *p; ++p) {
    char ch = (char)toupper((unsigned char)(*p));
    for (int row = 0; row < 7; ++row) {
      uint8_t bits = glyphRow5x7(ch, row);
      for (int col = 0; col < 5; ++col) {
        if (bits & (1 << (4 - col))) {
          hudDrawRect(penX + col * scale, y + row * scale, scale, scale, color);
        }
      }
    }
    penX += 6.0f * scale;
  }
}

static float hudMeasureText5x7(const char *text, float scale) {
  if (!text) return 0.0f;
  int len = 0;
  while (text[len]) ++len;
  if (len <= 0) return 0.0f;
  return (len * 6.0f - 1.0f) * scale;
}

static void drawHotbarHUD() {
  g_inventoryHoverName = nullptr;
  const float slot = 24.0f;
  const float pad = 3.0f;
  const float totalW = 9 * slot + 8 * pad;
  const float startX = (480.0f - totalW) * 0.5f;
  const float y = 272.0f - slot - 8.0f;

  for (int i = 0; i < 9; ++i) {
    float sx = startX + i * (slot + pad);
    bool selected = (i == g_creativeInv.hotbarSel());
    hudDrawRect(sx - 1, y - 1, slot + 2, slot + 2, selected ? 0xD0FFFFFF : 0x90303030);
    hudDrawRect(sx, y, slot, slot, 0x90000000);
    uint8_t id = g_creativeInv.hotbarAt(i);
    if (id != BLOCK_AIR) {
      int tx, ty;
      hudGetIconTile(id, tx, ty);
      hudDrawTile(g_atlas, tx, ty, sx + 3, y + 3, slot - 6);
    }
  }

  if (g_creativeInv.isOpen()) {
    const int invCount = g_creativeInv.visibleItemCount();
    const int creativeCols = 10;
    const int creativeRows = 5;
    const int itemsPerPage = 50;
    const float panelX = 96.0f + g_invOffsetX;
    const float panelY = 40.0f + g_invOffsetY;
    const float panelW = 240.0f;
    const float panelH = 192.0f;
    const float cellStepX = g_invCellStep;
    const float cellStepY = g_invCellStep;
    const float cellSize = g_invCellStep - 1.0f;
    const float cellX0 = 125.0f + (cellStepX * 0.5f) + g_invOffsetX;
    const float cellY0 = 93.0f - (cellStepY * 0.25f) + g_invOffsetY;
    const float hotbarY = 209.0f + g_invOffsetY + g_invHotbarOffsetY;
    const float sliderX = 372.0f + g_invOffsetX;
    const float sliderW = 16.0f;
    const float iconSize = 16.0f;
    const float iconPad = (cellSize - iconSize) * 0.5f;
    const float stretchRightPerCol = g_invStretchX;
    const float compressUpPerRow = g_invCompressY;
    auto gridX = [&](int col) { return cellX0 + col * cellStepX + col * stretchRightPerCol; };
    auto gridY = [&](int row) { return cellY0 + row * cellStepY - row * compressUpPerRow; };
    if (g_guiInvCreativeTex.data) {
      // inventory_creative.png in this port is authored as full PSP screen with transparent margins
      hudDrawTexture(g_guiInvCreativeTex, 0.0f, 0.0f, 480.0f, 272.0f);
    } else {
      hudDrawRect(panelX, panelY, panelW, panelH, 0xC0202020);
      hudDrawRect(panelX + 2, panelY + 2, panelW - 4, panelH - 4, 0x90000000);
    }

    int base = g_creativeInv.creativePage() * itemsPerPage;
    for (int r = 0; r < creativeRows; ++r) {
      for (int c = 0; c < creativeCols; ++c) {
        int idx = base + r * creativeCols + c;
        float sx = gridX(c);
        float sy = gridY(r);
        if (g_guiCellTex.data) hudDrawTexture(g_guiCellTex, sx, sy, cellSize, cellSize);
        else hudDrawRect(sx, sy, cellSize, cellSize, 0x90303030);
        if (idx >= invCount) continue;
        uint8_t id = g_creativeInv.visibleItemAt(idx);
        int tx, ty;
        hudGetIconTile(id, tx, ty);
        hudDrawTile(g_atlas, tx, ty, sx + iconPad, sy + iconPad, iconSize);
      }
    }

    auto hotbarX = [&](int col) { return (cellX0 + g_invHotbarOffsetX) + col * g_invHotbarStepX + col * g_invHotbarStretchX; };
    for (int i = 0; i < 9; ++i) {
      float sx = hotbarX(i);
      if (g_guiCellTex.data) hudDrawTexture(g_guiCellTex, sx, hotbarY, cellSize, cellSize);
      else hudDrawRect(sx, hotbarY, cellSize, cellSize, 0x90303030);
      uint8_t id = g_creativeInv.hotbarAt(i);
      if (id != BLOCK_AIR) {
        int tx, ty;
        hudGetIconTile(id, tx, ty);
        hudDrawTile(g_atlas, tx, ty, sx + iconPad, hotbarY + iconPad, iconSize);
      }
    }

    if (g_guiSliderTex.data) hudDrawTexture(g_guiSliderTex, sliderX, cellY0, sliderW, 5.0f * cellStepY + (cellSize - cellStepY));
    else hudDrawRect(sliderX, cellY0, sliderW, 5.0f * cellStepY + (cellSize - cellStepY), 0x70303030);
    const float deleteX = sliderX + g_invDeleteOffsetX;
    const float deleteY = hotbarY + g_invDeleteOffsetY;

    float cursorX = gridX(g_creativeInv.cursorX());
    float cursorY = (g_creativeInv.cursorY() < 5) ? gridY(g_creativeInv.cursorY()) : hotbarY;
    if (g_creativeInv.cursorY() == 5 && g_creativeInv.cursorX() == 10) {
      cursorX = deleteX;
      cursorY = deleteY;
    }
    if (g_creativeInv.cursorHasItem()) {
      const float cursorIconSize = iconSize * 0.75f;
      int tx, ty;
      hudGetIconTile(g_creativeInv.cursorItem(), tx, ty);
      hudDrawTile(g_atlas, tx, ty, cursorX + (cellSize - cursorIconSize) * 0.5f,
                  cursorY + (cellSize - cursorIconSize) * 0.5f, cursorIconSize);
    } else {
      if (g_guiCursorTex.data) hudDrawTexture(g_guiCursorTex, cursorX - 1.0f, cursorY - 1.0f, cellSize + 2.0f, cellSize + 2.0f);
      else hudDrawRect(cursorX - 1.0f, cursorY - 1.0f, cellSize + 2.0f, cellSize + 2.0f, 0xD0FFFFFF);
    }

    if (g_creativeInv.cursorY() < 5 && g_creativeInv.cursorX() < 10) {
      int hover = base + g_creativeInv.cursorY() * creativeCols + g_creativeInv.cursorX();
      if (hover >= 0 && hover < invCount) g_inventoryHoverName = getBlockDisplayName(g_creativeInv.visibleItemAt(hover));
    } else if (g_creativeInv.cursorY() == 5 && g_creativeInv.cursorX() < 9) {
      uint8_t hoveredHotbar = g_creativeInv.hotbarAt(g_creativeInv.cursorX());
      if (hoveredHotbar != BLOCK_AIR) g_inventoryHoverName = getBlockDisplayName(hoveredHotbar);
    } else if (g_creativeInv.cursorY() == 5 && g_creativeInv.cursorX() == 10) {
      g_inventoryHoverName = "Delete";
    } else if (g_creativeInv.cursorX() == 10) {
      g_inventoryHoverName = "Page Slider";
    }
    if (g_inventoryHoverName) {
      const float hoverScale = 1.1f;
      const float hoverW = hudMeasureText5x7(g_inventoryHoverName, hoverScale);
      float hoverX = cursorX + cellSize + 6.0f;
      float hoverY = cursorY + (cellSize - 8.0f) * 0.5f;
      if (hoverX + hoverW + 8.0f > 478.0f) hoverX = cursorX - hoverW - 8.0f;
      if (hoverX < 2.0f) hoverX = 2.0f;
      if (hoverY < 2.0f) hoverY = 2.0f;
      if (hoverY > 258.0f) hoverY = 258.0f;
      hudDrawRect(hoverX - 2.0f, hoverY - 2.0f, hoverW + 4.0f, 12.0f, 0xA0000000);
      hudDrawText5x7(hoverX, hoverY, g_inventoryHoverName, 0xFFFFFFFF, hoverScale);
    }

    int maxPage = (invCount / itemsPerPage);
    if (maxPage > 0) {
      float t = (float)g_creativeInv.creativePage() / (float)maxPage;
      hudDrawRect(sliderX + 4.0f, 102.0f + t * 84.0f, 8.0f, 16.0f, 0xD0FFFFFF);
    }

    const float tabX0 = 96.0f + g_invOffsetX;
    const float tabY0 = 40.0f + g_invOffsetY;
    const float tabW = 30.0f;
    const float tabStep = 30.0f;
    hudDrawRect(tabX0 + g_creativeInv.category() * tabStep, tabY0, tabW, 32.0f, 0x50FFFF00);

    // Replace baked "Building Blocks" texture text with runtime pixel-font category label.
    const float titleAreaX = 206.0f + g_invOffsetX;
    const float titleY = 73.0f + g_invOffsetY + g_invTitleOffsetY;
    const float titleAreaW = 120.0f;
    const char *catName = g_creativeInv.categoryName();
    const float titleW = hudMeasureText5x7(catName, 1.1f);
    const float titleX = titleAreaX + (titleAreaW - titleW) * 0.5f + g_invTitleOffsetX;
    hudDrawRect(titleAreaX - 2.0f, titleY - 2.0f, titleAreaW, 12.0f, 0x90D0D0D0);
    hudDrawText5x7(titleX, titleY, g_creativeInv.categoryName(), 0xFF303030, 1.1f);
    if (kEnableInvTuneMode && g_invTuneMode) {
      hudDrawRect(86.0f, 242.0f, 308.0f, 14.0f, 0xA0000000);
      const char *tuneLabel = "TUNE GRID (SELECT)";
      if (g_invTuneTarget == 1) tuneLabel = "TUNE HOTBAR (SELECT)";
      else if (g_invTuneTarget == 2) tuneLabel = "TUNE DELETE (SELECT)";
      else if (g_invTuneTarget == 3) tuneLabel = "TUNE TITLE (SELECT)";
      hudDrawText5x7(90.0f, 245.0f, tuneLabel, 0xFFFFFFFF, 1.0f);
    }
  }
}

static inline bool isWaterId(uint8_t id) {
  return id == BLOCK_WATER_STILL || id == BLOCK_WATER_FLOW;
}

static inline bool isLavaId(uint8_t id) {
  return id == BLOCK_LAVA_STILL || id == BLOCK_LAVA_FLOW;
}

static void worldPathForSlot(int slot, char *out, int outSize) {
  if (!out || outSize <= 0) return;
  snprintf(out, outSize, "%s/world%d.mcpw", kWorldDir, slot + 1);
}

static void resetPlayerStateForNewWorld() {
  g_player.x = 8.0f;
  g_player.y = 65.0f;
  g_player.z = 8.0f;
  g_player.yaw = 0.0f;
  g_player.pitch = 0.0f;
  g_player.velY = 0.0f;
  g_player.onGround = false;
  g_player.isFlying = false;
  g_player.jumpDoubleTapTimer = 0.0f;
}

static bool worldFileExists(int slot) {
  char path[128];
  worldPathForSlot(slot, path, sizeof(path));
  FILE *f = fopen(path, "rb");
  if (!f) return false;
  fclose(f);
  return true;
}

static void worldMetaPathForSlot(int slot, char *out, int outSize) {
  if (!out || outSize <= 0) return;
  snprintf(out, outSize, "%s/world%d.meta", kWorldDir, slot + 1);
}

static void loadInventoryLayoutConfig() {
  FILE *f = fopen(kInvLayoutCfgPath, "rb");
  if (!f) return;
  float step = 0.0f, sx = 0.0f, cy = 0.0f, ox = 0.0f, oy = 0.0f;
  float hs = 0.0f, hst = 0.0f, hox = 0.0f, hoy = 0.0f;
  float dx = 0.0f, dy = 0.0f;
  float tx = 0.0f, ty = 0.0f;
  int n = fscanf(f, "%f %f %f %f %f %f %f %f %f %f %f %f %f",
                 &step, &sx, &cy, &ox, &oy, &hs, &hst, &hox, &hoy, &dx, &dy, &tx, &ty);
  if (n >= 3) {
    if (step >= 16.0f && step <= 28.0f) g_invCellStep = step;
    if (sx >= -2.0f && sx <= 2.0f) g_invStretchX = sx;
    if (cy >= -2.0f && cy <= 2.0f) g_invCompressY = cy;
    if (n >= 5) {
      if (ox >= -80.0f && ox <= 80.0f) g_invOffsetX = ox;
      if (oy >= -80.0f && oy <= 80.0f) g_invOffsetY = oy;
    }
    if (n >= 9) {
      if (hs >= 16.0f && hs <= 30.0f) g_invHotbarStepX = hs;
      if (hst >= -2.0f && hst <= 2.0f) g_invHotbarStretchX = hst;
      if (hox >= -80.0f && hox <= 80.0f) g_invHotbarOffsetX = hox;
      if (hoy >= -80.0f && hoy <= 80.0f) g_invHotbarOffsetY = hoy;
    }
    if (n >= 11) {
      if (dx >= -80.0f && dx <= 80.0f) g_invDeleteOffsetX = dx;
      if (dy >= -80.0f && dy <= 80.0f) g_invDeleteOffsetY = dy;
    }
    if (n >= 13) {
      if (tx >= -80.0f && tx <= 80.0f) g_invTitleOffsetX = tx;
      if (ty >= -80.0f && ty <= 80.0f) g_invTitleOffsetY = ty;
    }
  }
  fclose(f);
}

static void loadInventoryTuneModeConfig() {
  kEnableInvTuneMode = false;
  FILE *f = fopen(kInvTuneCfgPath, "rb");
  if (!f) return;
  char token[16] = {0};
  if (fscanf(f, "%15s", token) == 1) {
    for (int i = 0; token[i]; ++i) token[i] = (char)tolower((unsigned char)token[i]);
    if (strcmp(token, "true") == 0 || strcmp(token, "1") == 0 || strcmp(token, "on") == 0) {
      kEnableInvTuneMode = true;
    }
  }
  fclose(f);
}

static void saveInventoryLayoutConfig() {
  FILE *f = fopen(kInvLayoutCfgPath, "wb");
  if (!f) return;
  fprintf(f, "%.3f %.3f %.3f %.3f %.3f %.3f %.3f %.3f %.3f %.3f %.3f %.3f %.3f\n",
          g_invCellStep, g_invStretchX, g_invCompressY, g_invOffsetX, g_invOffsetY,
          g_invHotbarStepX, g_invHotbarStretchX, g_invHotbarOffsetX, g_invHotbarOffsetY,
          g_invDeleteOffsetX, g_invDeleteOffsetY, g_invTitleOffsetX, g_invTitleOffsetY);
  fclose(f);
}

static void setStatusMessage(const char *msg, float seconds, uint32_t color = 0xFFFFFFFF) {
  if (!msg) msg = "";
  snprintf(g_statusText, sizeof(g_statusText), "%s", msg);
  g_statusTimer = seconds;
  g_statusColor = color;
}

static void markAllChunksDirtyForRelight() {
  if (!g_level) return;
  for (int cx = 0; cx < WORLD_CHUNKS_X; ++cx) {
    for (int cz = 0; cz < WORLD_CHUNKS_Z; ++cz) {
      Chunk *ch = g_level->getChunk(cx, cz);
      if (!ch) continue;
      for (int sy = 0; sy < SUBCHUNK_COUNT; ++sy) ch->dirty[sy] = true;
    }
  }
}

static void asciiToUtf16(const char *src, unsigned short *dst, int maxChars) {
  if (!dst || maxChars <= 0) return;
  int i = 0;
  if (src) {
    while (src[i] && i < maxChars - 1) {
      dst[i] = (unsigned short)(unsigned char)src[i];
      ++i;
    }
  }
  dst[i] = 0;
}

static void utf16ToAscii(const unsigned short *src, char *dst, int maxChars) {
  if (!dst || maxChars <= 0) return;
  int i = 0;
  if (src) {
    while (src[i] && i < maxChars - 1) {
      unsigned short c = src[i];
      dst[i] = (c < 0x80) ? (char)c : '?';
      ++i;
    }
  }
  dst[i] = '\0';
}

static bool showCommandKeyboard(char *out, int outSize) {
  if (!out || outSize <= 1) return false;
  out[0] = '\0';

  static unsigned short inText[128];
  static unsigned short outText[128];
  static unsigned short descText[32];
  static unsigned short initText[2];
  asciiToUtf16("Command (/time set day)", descText, (int)(sizeof(descText) / sizeof(descText[0])));
  asciiToUtf16("", initText, (int)(sizeof(initText) / sizeof(initText[0])));
  memset(inText, 0, sizeof(inText));
  memset(outText, 0, sizeof(outText));

  SceUtilityOskData oskData;
  memset(&oskData, 0, sizeof(oskData));
  oskData.language = PSP_UTILITY_OSK_LANGUAGE_ENGLISH;
  oskData.lines = 1;
  oskData.unk_24 = 1;
  oskData.inputtype = PSP_UTILITY_OSK_INPUTTYPE_ALL;
  oskData.desc = descText;
  oskData.intext = initText;
  oskData.outtextlength = (int)(sizeof(outText) / sizeof(outText[0]));
  oskData.outtextlimit = oskData.outtextlength - 1;
  oskData.outtext = outText;

  SceUtilityOskParams params;
  memset(&params, 0, sizeof(params));
  params.base.size = sizeof(params);
  sceUtilityGetSystemParamInt(PSP_SYSTEMPARAM_ID_INT_LANGUAGE, &params.base.language);
  sceUtilityGetSystemParamInt(PSP_SYSTEMPARAM_ID_INT_UNKNOWN, &params.base.buttonSwap);
  params.base.graphicsThread = 17;
  params.base.accessThread = 19;
  params.base.fontThread = 18;
  params.base.soundThread = 16;
  params.datacount = 1;
  params.data = &oskData;

  int ret = sceUtilityOskInitStart(&params);
  if (ret < 0) return false;
  static unsigned int __attribute__((aligned(16))) oskList[262144];

  while (true) {
    sceGuStart(GU_DIRECT, oskList);
    sceGuClearColor(0xFF000000);
    sceGuClearDepth(0);
    sceGuClear(GU_COLOR_BUFFER_BIT | GU_DEPTH_BUFFER_BIT);
    sceGuFinish();
    sceGuSync(0, 0);

    sceUtilityOskUpdate(1);
    sceDisplayWaitVblankStart();
    PSPRenderer_DialogSwapBuffers();

    int status = sceUtilityOskGetStatus();
    if (status == PSP_UTILITY_DIALOG_VISIBLE) continue;
    if (status == PSP_UTILITY_DIALOG_QUIT) {
      sceUtilityOskShutdownStart();
    } else if (status == PSP_UTILITY_DIALOG_FINISHED || status == PSP_UTILITY_DIALOG_NONE) {
      break;
    }
  }

  if (oskData.result != PSP_UTILITY_OSK_RESULT_CHANGED) return false;
  utf16ToAscii(outText, out, outSize);
  return out[0] != '\0';
}

static void executeConsoleCommand(const char *rawCmd) {
  if (!rawCmd || !rawCmd[0]) return;

  char cmd[128];
  snprintf(cmd, sizeof(cmd), "%s", rawCmd);
  for (int i = 0; cmd[i]; ++i) cmd[i] = (char)tolower((unsigned char)cmd[i]);

  if (cmd[0] != '/') {
    setStatusMessage("Commands must start with /", 2.5f, 0xFF60A0FF);
    return;
  }
  if (strcmp(cmd, "/time set day") == 0) {
    if (g_level) g_level->setTime((g_level->getDay() * TICKS_PER_DAY) + 1000LL);
    setStatusMessage("Set time: day", 2.0f, 0xFF80FF80);
    return;
  }
  if (strcmp(cmd, "/time set night") == 0) {
    if (g_level) g_level->setTime((g_level->getDay() * TICKS_PER_DAY) + 13000LL);
    setStatusMessage("Set time: night", 2.0f, 0xFF80FF80);
    return;
  }

  setStatusMessage("Unknown command", 2.0f, 0xFF6060FF);
}

static int firstExistingWorldSlot() {
  for (int i = 0; i < kMaxWorldSlots; ++i) {
    if (g_worldExists[i]) return i;
  }
  return -1;
}

static int nextEmptyWorldSlot() {
  for (int i = 0; i < kMaxWorldSlots; ++i) {
    if (!g_worldExists[i]) return i;
  }
  return -1;
}

static int findPrevExistingWorldSlot(int start) {
  for (int i = start - 1; i >= 0; --i) {
    if (g_worldExists[i]) return i;
  }
  return -1;
}

static int findNextExistingWorldSlot(int start) {
  for (int i = start + 1; i < kMaxWorldSlots; ++i) {
    if (g_worldExists[i]) return i;
  }
  return -1;
}

static bool savePlayerAndInventoryForSlot(int slot) {
  char path[128];
  worldMetaPathForSlot(slot, path, sizeof(path));
  FILE *f = fopen(path, "wb");
  if (!f) return false;

  const char header[8] = {'M','C','P','S','P','M','E','T'};
  int hotbarSel = g_creativeInv.hotbarSel();
  uint8_t hotbar[9];
  for (int i = 0; i < 9; ++i) hotbar[i] = g_creativeInv.hotbarAt(i);
  if (fwrite(header, 1, sizeof(header), f) != sizeof(header) ||
      fwrite(&g_player.x, sizeof(g_player.x), 1, f) != 1 ||
      fwrite(&g_player.y, sizeof(g_player.y), 1, f) != 1 ||
      fwrite(&g_player.z, sizeof(g_player.z), 1, f) != 1 ||
      fwrite(&g_player.yaw, sizeof(g_player.yaw), 1, f) != 1 ||
      fwrite(&g_player.pitch, sizeof(g_player.pitch), 1, f) != 1 ||
      fwrite(&g_player.isFlying, sizeof(g_player.isFlying), 1, f) != 1 ||
      fwrite(&hotbarSel, sizeof(hotbarSel), 1, f) != 1 ||
      fwrite(hotbar, sizeof(hotbar[0]), 9, f) != 9) {
    fclose(f);
    return false;
  }

  fflush(f);
  fclose(f);
  return true;
}

static bool loadPlayerAndInventoryFromPath(const char *path) {
  FILE *f = fopen(path, "rb");
  if (!f) return false;

  char header[8];
  if (fread(header, 1, sizeof(header), f) != sizeof(header) ||
      memcmp(header, "MCPSPMET", 8) != 0) {
    fclose(f);
    return false;
  }

  int hotbarSel = 0;
  uint8_t hotbar[9];
  if (fread(&g_player.x, sizeof(g_player.x), 1, f) != 1 ||
      fread(&g_player.y, sizeof(g_player.y), 1, f) != 1 ||
      fread(&g_player.z, sizeof(g_player.z), 1, f) != 1 ||
      fread(&g_player.yaw, sizeof(g_player.yaw), 1, f) != 1 ||
      fread(&g_player.pitch, sizeof(g_player.pitch), 1, f) != 1 ||
      fread(&g_player.isFlying, sizeof(g_player.isFlying), 1, f) != 1 ||
      fread(&hotbarSel, sizeof(hotbarSel), 1, f) != 1 ||
      fread(hotbar, sizeof(hotbar[0]), 9, f) != 9) {
    fclose(f);
    return false;
  }

  g_creativeInv.setHotbarSel(hotbarSel);
  for (int i = 0; i < 9; ++i) g_creativeInv.setHotbarAt(i, hotbar[i]);
  g_heldBlock = g_creativeInv.heldBlock();
  fclose(f);
  return true;
}

static bool loadPlayerAndInventoryForSlot(int slot) {
  char path[128];
  worldMetaPathForSlot(slot, path, sizeof(path));
  return loadPlayerAndInventoryFromPath(path);
}

static bool saveWorldAndMetaForSlot(int slot) {
  char path[128];
  worldPathForSlot(slot, path, sizeof(path));
  if (!g_level) return false;
  if (!g_level->saveToFile(path)) return false;
  return savePlayerAndInventoryForSlot(slot);
}

static void refreshWorldSlots() {
  for (int i = 0; i < kMaxWorldSlots; ++i) g_worldExists[i] = worldFileExists(i);
  int firstSlot = firstExistingWorldSlot();
  if (firstSlot >= 0) {
    if (!g_worldExists[g_menuSlotSel]) g_menuSlotSel = firstSlot;
    if (g_menuCreateSelected && !g_menuDeleteConfirm) g_menuCreateSelected = false;
  } else {
    g_menuCreateSelected = true;
  }
}

static bool loadWorldSlot(int slot) {
  char path[128];
  worldPathForSlot(slot, path, sizeof(path));
  if (!g_level) return false;
  bool loadedMain = g_level->loadFromFile(path);
  if (!loadedMain) return false;
  if (!loadPlayerAndInventoryForSlot(slot)) {
    setStatusMessage("Loaded world (meta missing/corrupt)", 3.0f, 0xFF70B0FF);
  }
  g_worldSlot = slot;
  g_autoSaveTimer = 0.0f;
  return true;
}

static void createWorldSlot(int slot) {
  if (!g_level) return;
  resetPlayerStateForNewWorld();
  uint64_t t = sceKernelGetSystemTimeWide();
  Random rng((int64_t)t ^ ((int64_t)slot * 341873128712LL));
  g_level->generate(&rng);
  if (!saveWorldAndMetaForSlot(slot)) {
    setStatusMessage("Create world failed", 3.0f, 0xFF6060FF);
    return;
  }
  setStatusMessage("World created", 2.0f, 0xFF80FF80);
  g_worldSlot = slot;
  g_autoSaveTimer = 0.0f;
  refreshWorldSlots();
}

static void deleteWorldSlot(int slot) {
  char path[128];
  worldPathForSlot(slot, path, sizeof(path));
  sceIoRemove(path);
  worldMetaPathForSlot(slot, path, sizeof(path));
  sceIoRemove(path);
  refreshWorldSlots();
}

// Initialization
static bool game_init() {
  pspDebugScreenInit();
  // Overclock PSP to max for performance
  scePowerSetClockFrequency(333, 333, 166);

  // Init block tables
  Blocks_Init();

  // Init sin/cos lookup table
  Mth::init();

  // Init PSP renderer (sceGu)
  if (!PSPRenderer_Init())
    return false;

  // Load terrain.png from MS0:/PSP/GAME/MinecraftPSP/res/
  g_atlas = new TextureAtlas();
  if (!g_atlas->load("res/terrain.png"))
    return false;

  g_level = new Level();
  g_skyRenderer = new SkyRenderer(g_level);
  g_cloudRenderer = new CloudRenderer(g_level);
  g_guiInvCreativeTex.load(kTexInvCreativePath);
  g_guiCursorTex.load(kTexCursorPath);
  g_guiSliderTex.load(kTexSliderPath);
  g_guiCellTex.load(kTexCellPath);

  // Init chunk renderer
  g_chunkRenderer = new ChunkRenderer(g_atlas);
  g_chunkRenderer->setLevel(g_level);

  // Load existing world or generate a new one.
  sceIoMkdir("ms0:/PSP/GAME/MinecraftPSP", 0777);
  sceIoMkdir(kWorldDir, 0777);
  loadInventoryTuneModeConfig();
  loadInventoryLayoutConfig();
  refreshWorldSlots();

  // Player start position
  resetPlayerStateForNewWorld();
  g_heldBlock = g_creativeInv.heldBlock();
  g_lastSunLightBucket = -1;
  return true;
}

// Game loop update
static void game_update(float dt) {
  PSPInput_Update();
  if (g_statusTimer > 0.0f) {
    g_statusTimer -= dt;
    if (g_statusTimer <= 0.0f) g_statusText[0] = '\0';
  }

  if (g_inMainMenu) {
    if (g_menuDeleteConfirm) {
      if (PSPInput_JustPressed(PSP_CTRL_LEFT) || PSPInput_JustPressed(PSP_CTRL_RIGHT) ||
          PSPInput_JustPressed(PSP_CTRL_UP) || PSPInput_JustPressed(PSP_CTRL_DOWN)) {
        g_menuDeleteYesSelected = !g_menuDeleteYesSelected;
      }
      if (PSPInput_JustPressed(PSP_CTRL_CIRCLE)) {
        g_menuDeleteConfirm = false;
        g_menuDeleteYesSelected = false;
      } else if (PSPInput_JustPressed(PSP_CTRL_CROSS)) {
        if (g_menuDeleteYesSelected && g_worldExists[g_menuSlotSel]) {
          deleteWorldSlot(g_menuSlotSel);
          setStatusMessage("World deleted", 2.0f, 0xFF80FF80);
          int firstSlot = firstExistingWorldSlot();
          if (firstSlot >= 0) {
            g_menuSlotSel = firstSlot;
            g_menuCreateSelected = false;
          } else {
            g_menuCreateSelected = true;
          }
        }
        g_menuDeleteConfirm = false;
        g_menuDeleteYesSelected = false;
      }
      return;
    }

    if (PSPInput_JustPressed(PSP_CTRL_UP)) {
      if (!g_menuCreateSelected) {
        int prevSlot = findPrevExistingWorldSlot(g_menuSlotSel);
        if (prevSlot >= 0) g_menuSlotSel = prevSlot;
        else g_menuCreateSelected = true;
      }
    }
    if (PSPInput_JustPressed(PSP_CTRL_DOWN)) {
      if (g_menuCreateSelected) {
        int firstSlot = firstExistingWorldSlot();
        if (firstSlot >= 0) {
          g_menuSlotSel = firstSlot;
          g_menuCreateSelected = false;
        }
      } else {
        int nextSlot = findNextExistingWorldSlot(g_menuSlotSel);
        if (nextSlot >= 0) g_menuSlotSel = nextSlot;
      }
    }

    if (PSPInput_JustPressed(PSP_CTRL_CROSS)) {
      if (g_menuCreateSelected) {
        int emptySlot = nextEmptyWorldSlot();
        if (emptySlot >= 0) {
          createWorldSlot(emptySlot);
          g_menuSlotSel = emptySlot;
          g_menuCreateSelected = false;
          if (g_worldExists[emptySlot]) g_inMainMenu = false;
        } else {
          setStatusMessage("No free world slots", 2.5f, 0xFF60A0FF);
        }
      } else if (g_worldExists[g_menuSlotSel] && loadWorldSlot(g_menuSlotSel)) {
        if (g_statusTimer <= 0.0f) setStatusMessage("World loaded", 2.0f, 0xFF80FF80);
        g_inMainMenu = false;
      } else if (!g_menuCreateSelected) {
        setStatusMessage("Load failed", 3.0f, 0xFF6060FF);
      }
    }

    if (!g_menuCreateSelected && g_worldExists[g_menuSlotSel] &&
        PSPInput_JustPressed(PSP_CTRL_TRIANGLE)) {
      g_menuDeleteConfirm = true;
      g_menuDeleteYesSelected = false; // default: NO
    }
    return;
  }

  if (PSPInput_IsHeld(PSP_CTRL_SELECT) && PSPInput_JustPressed(PSP_CTRL_START) && !g_pauseOpen) {
    char cmd[128];
    if (showCommandKeyboard(cmd, sizeof(cmd))) executeConsoleCommand(cmd);
    return;
  }

  if (PSPInput_JustPressed(PSP_CTRL_START) && !g_pauseOpen) {
    g_pauseOpen = true;
    g_pauseSel = 0;
  } else if (g_pauseOpen) {
    if (PSPInput_JustPressed(PSP_CTRL_UP)) g_pauseSel = (g_pauseSel + 2) % 3;
    if (PSPInput_JustPressed(PSP_CTRL_DOWN)) g_pauseSel = (g_pauseSel + 1) % 3;
    if (PSPInput_JustPressed(PSP_CTRL_CIRCLE)) g_pauseOpen = false;
    if (PSPInput_JustPressed(PSP_CTRL_CROSS)) {
      if (g_pauseSel == 0) {
        g_pauseOpen = false;
      } else if (g_pauseSel == 1) {
        if (saveWorldAndMetaForSlot(g_worldSlot)) setStatusMessage("Game saved", 2.0f, 0xFF80FF80);
        else setStatusMessage("Save failed", 3.0f, 0xFF6060FF);
        g_autoSaveTimer = 0.0f;
        g_pauseOpen = false;
      } else {
        if (saveWorldAndMetaForSlot(g_worldSlot)) setStatusMessage("Saved and exited", 2.5f, 0xFF80FF80);
        else setStatusMessage("Save failed", 3.0f, 0xFF6060FF);
        g_autoSaveTimer = 0.0f;
        refreshWorldSlots();
        g_inMainMenu = true;
        g_pauseOpen = false;
      }
    }
    return;
  }

  if (g_level) {
    static float s_levelTickAccum = 0.0f;
    const float tickStep = 1.0f / 20.0f; // Minecraft-like 20 TPS
    s_levelTickAccum += dt;
    int ticks = 0;
    while (s_levelTickAccum >= tickStep && ticks < 5) {
      g_level->setSimulationFocus((int)floorf(g_player.x), (int)floorf(g_player.y), (int)floorf(g_player.z), 24);
      g_level->tick();
      s_levelTickAccum -= tickStep;
      ticks++;
    }
    g_tickAlpha = s_levelTickAccum / tickStep;
    if (g_tickAlpha < 0.0f) g_tickAlpha = 0.0f;
    if (g_tickAlpha > 1.0f) g_tickAlpha = 1.0f;

    int sunBucket = (int)(g_level->getSunBrightness() * 15.0f + 0.5f);
    if (g_lastSunLightBucket < 0) {
      g_lastSunLightBucket = sunBucket;
    } else if (sunBucket != g_lastSunLightBucket) {
      markAllChunksDirtyForRelight();
      g_lastSunLightBucket = sunBucket;
    }

    if (!g_creativeInv.isOpen() && !g_pauseOpen) {
      g_autoSaveTimer += dt;
      if (g_autoSaveTimer >= kAutoSaveIntervalSec) {
        if (saveWorldAndMetaForSlot(g_worldSlot)) {
          setStatusMessage("Autosaved", 1.5f, 0xFFA0FFA0);
        } else {
          setStatusMessage("Autosave failed", 2.5f, 0xFF6060FF);
        }
        g_autoSaveTimer = 0.0f;
      }
    }
  }

  bool inWater = false;
  {
    int fx = (int)floorf(g_player.x);
    int fz = (int)floorf(g_player.z);
    int bodyY = (int)floorf(g_player.y + 0.1f);
    inWater = isWaterId(g_level->getBlock(fx, bodyY, fz));
  }

  float baseMoveSpeed = g_player.isFlying ? 10.0f : 5.0f;
  if (inWater && !g_player.isFlying) baseMoveSpeed *= 0.45f;
  float moveSpeed = baseMoveSpeed * dt;
  float lookSpeed = 120.0f * dt;

  // Rotation with right stick (Face Buttons)
  if (!g_creativeInv.isOpen()) {
    float lx = PSPInput_StickX(1);
    float ly = PSPInput_StickY(1);
    g_player.yaw += lx * lookSpeed;
    g_player.pitch += ly * lookSpeed;
    g_player.pitch = Mth::clamp(g_player.pitch, -89.0f, 89.0f);
  }

  // Movement with left stick (Analog)
  float fx = -PSPInput_StickX(0);
  float fz = -PSPInput_StickY(0);

  float yawRad = g_player.yaw * Mth::DEGRAD;

  float dx = (fx * Mth::cos(yawRad) + fz * Mth::sin(yawRad)) * moveSpeed;
  float dz = (-fx * Mth::sin(yawRad) + fz * Mth::cos(yawRad)) * moveSpeed;

  const float R = 0.3f;   // 4J: setSize(0.6, 1.8)
  const float H = 1.8f;   // 4J: player bounding box height

  // Vertical movement
  float dy = 0.0f;
  if (g_player.isFlying) {
    float flySpeed = 10.0f * dt;
    if (PSPInput_IsHeld(PSP_CTRL_SELECT))
      dy = flySpeed;  // Ascend
    if (PSPInput_IsHeld(PSP_CTRL_DOWN))
      dy = -flySpeed;  // Descend
    g_player.velY = 0.0f;
  } else {
    if (inWater) {
      g_player.velY -= 6.0f * dt;
      if (PSPInput_IsHeld(PSP_CTRL_SELECT)) {
        g_player.velY += 9.0f * dt;
      }
      g_player.velY *= 0.85f;
    } else {
      g_player.velY -= 20.0f * dt;
    }
    dy = g_player.velY * dt;
  }

  // Collision
  AABB player_aabb(g_player.x - R, g_player.y, g_player.z - R,
                   g_player.x + R, g_player.y + H, g_player.z + R);

  AABB* expanded = player_aabb.expand(dx, dy, dz);
  std::vector<AABB> cubes = g_level->getCubes(*expanded);
  delete expanded;

  float dy_org = dy;
  for (auto& cube : cubes) dy = cube.clipYCollide(&player_aabb, dy);
  player_aabb.move(0, dy, 0);

  for (auto& cube : cubes) dx = cube.clipXCollide(&player_aabb, dx);
  player_aabb.move(dx, 0, 0);

  for (auto& cube : cubes) dz = cube.clipZCollide(&player_aabb, dz);
  player_aabb.move(0, 0, dz);

  g_player.onGround = (dy_org != dy && dy_org < 0.0f);
  if (g_player.onGround || dy_org != dy) {
    g_player.velY = 0.0f;
  }
  if (inWater && !g_player.isFlying) {
    g_player.velY *= 0.8f;
  }

  g_player.x = (player_aabb.x0 + player_aabb.x1) / 2.0f;
  g_player.y = player_aabb.y0;
  g_player.z = (player_aabb.z0 + player_aabb.z1) / 2.0f;

  // Enforce world bounds natively
  const float WORLD_MAX_X = (float)(WORLD_CHUNKS_X * CHUNK_SIZE_X - 1);
  const float WORLD_MAX_Z = (float)(WORLD_CHUNKS_Z * CHUNK_SIZE_Z - 1);
  if (g_player.x < 0.5f) g_player.x = 0.5f;
  if (g_player.x > WORLD_MAX_X) g_player.x = WORLD_MAX_X;
  if (g_player.z < 0.5f) g_player.z = 0.5f;
  if (g_player.z > WORLD_MAX_Z) g_player.z = WORLD_MAX_Z;

  // Controls: Jump/Fly
  static const float DOUBLE_TAP_WINDOW = 0.35f;
  if (g_player.jumpDoubleTapTimer > 0.0f)
    g_player.jumpDoubleTapTimer -= dt;

  if (PSPInput_JustPressed(PSP_CTRL_SELECT)) {
    if (g_player.jumpDoubleTapTimer > 0.0f) {
      g_player.isFlying = !g_player.isFlying;
      g_player.velY = 0.0f;
      g_player.jumpDoubleTapTimer = 0.0f;
    } else {
      if (!g_player.isFlying && g_player.onGround) {
        g_player.velY = 6.5f;
        g_player.onGround = false;
      } else if (!g_player.isFlying && inWater) {
        g_player.velY = 2.5f;
      }
      g_player.jumpDoubleTapTimer = DOUBLE_TAP_WINDOW;
    }
  }

  // Raycast block target
  {
    float eyeX = g_player.x;
    float eyeY = g_player.y + 1.6f;
    float eyeZ = g_player.z;
    float pitchRad = g_player.pitch * Mth::DEGRAD;
    float dirX = Mth::sin(yawRad) * Mth::cos(pitchRad);
    float dirY = Mth::sin(pitchRad);
    float dirZ = Mth::cos(yawRad) * Mth::cos(pitchRad);
    g_hitResult = raycast(g_level, eyeX, eyeY, eyeZ, dirX, dirY, dirZ, 5.0f);
  }

  // Block action cooldown
  static float breakCooldown = 0.0f;
  if (breakCooldown > 0.0f) breakCooldown -= dt;

  // Inventory/hotbar controls
  if ((PSPInput_IsHeld(PSP_CTRL_LTRIGGER) && PSPInput_JustPressed(PSP_CTRL_RTRIGGER)) ||
      (PSPInput_IsHeld(PSP_CTRL_RTRIGGER) && PSPInput_JustPressed(PSP_CTRL_LTRIGGER))) {
    g_creativeInv.open();
  }
  if (g_creativeInv.isOpen() && PSPInput_JustPressed(PSP_CTRL_CIRCLE)) {
    if (g_creativeInv.cursorHasItem()) {
      g_creativeInv.clearCursorSelection();
    } else {
      g_creativeInv.close();
      g_invTuneMode = false;
      g_invTuneTarget = 0;
    }
  }
  if (PSPInput_JustPressed(PSP_CTRL_RIGHT) && !g_creativeInv.isOpen()) g_creativeInv.cycleHotbarRight();
  if (PSPInput_JustPressed(PSP_CTRL_LEFT) && !g_creativeInv.isOpen()) g_creativeInv.cycleHotbarLeft();
  if (g_creativeInv.isOpen()) {
    if (kEnableInvTuneMode &&
        PSPInput_JustPressed(PSP_CTRL_TRIANGLE) &&
        PSPInput_IsHeld(PSP_CTRL_LTRIGGER) &&
        PSPInput_IsHeld(PSP_CTRL_RTRIGGER) &&
        PSPInput_IsHeld(PSP_CTRL_SELECT)) {
      // Hard-to-trigger debug tuning toggle:
      // hold L + R + SELECT, then press TRIANGLE.
      if (!g_invTuneMode) {
        g_invTuneMode = true;
        g_invTuneTarget = 0;
      } else {
        g_invTuneMode = false;
        g_invTuneTarget = 0;
        saveInventoryLayoutConfig();
      }
    }
    if (kEnableInvTuneMode && g_invTuneMode) {
      const float kWarpStep = 0.05f;
      const float kSpacingStep = 0.10f;
      const float kMoveStep = 0.35f;
      if (PSPInput_JustPressed(PSP_CTRL_SELECT)) g_invTuneTarget = (g_invTuneTarget + 1) % 4;
      if (g_invTuneTarget == 0) {
        if (PSPInput_JustPressed(PSP_CTRL_LEFT)) g_invStretchX -= kWarpStep;
        if (PSPInput_JustPressed(PSP_CTRL_RIGHT)) g_invStretchX += kWarpStep;
        if (PSPInput_JustPressed(PSP_CTRL_UP)) g_invCompressY += kWarpStep;
        if (PSPInput_JustPressed(PSP_CTRL_DOWN)) g_invCompressY -= kWarpStep;
        if (PSPInput_JustPressed(PSP_CTRL_LTRIGGER) && !PSPInput_IsHeld(PSP_CTRL_RTRIGGER)) g_invCellStep -= kSpacingStep;
        if (PSPInput_JustPressed(PSP_CTRL_RTRIGGER) && !PSPInput_IsHeld(PSP_CTRL_LTRIGGER)) g_invCellStep += kSpacingStep;
      } else if (g_invTuneTarget == 1) {
        if (PSPInput_JustPressed(PSP_CTRL_LEFT)) g_invHotbarStretchX -= kWarpStep;
        if (PSPInput_JustPressed(PSP_CTRL_RIGHT)) g_invHotbarStretchX += kWarpStep;
        if (PSPInput_JustPressed(PSP_CTRL_UP)) g_invHotbarOffsetY -= kMoveStep;
        if (PSPInput_JustPressed(PSP_CTRL_DOWN)) g_invHotbarOffsetY += kMoveStep;
        if (PSPInput_JustPressed(PSP_CTRL_LTRIGGER) && !PSPInput_IsHeld(PSP_CTRL_RTRIGGER)) g_invHotbarStepX -= kSpacingStep;
        if (PSPInput_JustPressed(PSP_CTRL_RTRIGGER) && !PSPInput_IsHeld(PSP_CTRL_LTRIGGER)) g_invHotbarStepX += kSpacingStep;
      } else if (g_invTuneTarget == 2) {
        if (PSPInput_JustPressed(PSP_CTRL_LEFT)) g_invDeleteOffsetX -= kMoveStep;
        if (PSPInput_JustPressed(PSP_CTRL_RIGHT)) g_invDeleteOffsetX += kMoveStep;
        if (PSPInput_JustPressed(PSP_CTRL_UP)) g_invDeleteOffsetY -= kMoveStep;
        if (PSPInput_JustPressed(PSP_CTRL_DOWN)) g_invDeleteOffsetY += kMoveStep;
      } else {
        if (PSPInput_JustPressed(PSP_CTRL_LEFT)) g_invTitleOffsetX -= kMoveStep;
        if (PSPInput_JustPressed(PSP_CTRL_RIGHT)) g_invTitleOffsetX += kMoveStep;
        if (PSPInput_JustPressed(PSP_CTRL_UP)) g_invTitleOffsetY -= kMoveStep;
        if (PSPInput_JustPressed(PSP_CTRL_DOWN)) g_invTitleOffsetY += kMoveStep;
      }
      float mx = PSPInput_StickX(0);
      float my = PSPInput_StickY(0);
      if (g_invTuneTarget == 0) {
        if (fabsf(mx) > 0.12f) g_invOffsetX += mx * kMoveStep;
        if (fabsf(my) > 0.12f) g_invOffsetY += my * kMoveStep;
        if (g_invOffsetX < -80.0f) g_invOffsetX = -80.0f;
        if (g_invOffsetX > 80.0f) g_invOffsetX = 80.0f;
        if (g_invOffsetY < -80.0f) g_invOffsetY = -80.0f;
        if (g_invOffsetY > 80.0f) g_invOffsetY = 80.0f;
      } else if (g_invTuneTarget == 1) {
        if (fabsf(mx) > 0.12f) g_invHotbarOffsetX += mx * kMoveStep;
        if (fabsf(my) > 0.12f) g_invHotbarOffsetY += my * kMoveStep;
        if (g_invHotbarOffsetX < -80.0f) g_invHotbarOffsetX = -80.0f;
        if (g_invHotbarOffsetX > 80.0f) g_invHotbarOffsetX = 80.0f;
        if (g_invHotbarOffsetY < -80.0f) g_invHotbarOffsetY = -80.0f;
        if (g_invHotbarOffsetY > 80.0f) g_invHotbarOffsetY = 80.0f;
      } else if (g_invTuneTarget == 2) {
        if (fabsf(mx) > 0.12f) g_invDeleteOffsetX += mx * kMoveStep;
        if (fabsf(my) > 0.12f) g_invDeleteOffsetY += my * kMoveStep;
        if (g_invDeleteOffsetX < -80.0f) g_invDeleteOffsetX = -80.0f;
        if (g_invDeleteOffsetX > 80.0f) g_invDeleteOffsetX = 80.0f;
        if (g_invDeleteOffsetY < -80.0f) g_invDeleteOffsetY = -80.0f;
        if (g_invDeleteOffsetY > 80.0f) g_invDeleteOffsetY = 80.0f;
      } else {
        if (fabsf(mx) > 0.12f) g_invTitleOffsetX += mx * kMoveStep;
        if (fabsf(my) > 0.12f) g_invTitleOffsetY += my * kMoveStep;
        if (g_invTitleOffsetX < -80.0f) g_invTitleOffsetX = -80.0f;
        if (g_invTitleOffsetX > 80.0f) g_invTitleOffsetX = 80.0f;
        if (g_invTitleOffsetY < -80.0f) g_invTitleOffsetY = -80.0f;
        if (g_invTitleOffsetY > 80.0f) g_invTitleOffsetY = 80.0f;
      }
      if (g_invCellStep < 16.0f) g_invCellStep = 16.0f;
      if (g_invCellStep > 28.0f) g_invCellStep = 28.0f;
      if (g_invHotbarStepX < 16.0f) g_invHotbarStepX = 16.0f;
      if (g_invHotbarStepX > 30.0f) g_invHotbarStepX = 30.0f;
    } else {
      if (PSPInput_JustPressed(PSP_CTRL_LTRIGGER) && !PSPInput_IsHeld(PSP_CTRL_RTRIGGER)) g_creativeInv.prevCategory();
      if (PSPInput_JustPressed(PSP_CTRL_RTRIGGER) && !PSPInput_IsHeld(PSP_CTRL_LTRIGGER)) g_creativeInv.nextCategory();
      if (PSPInput_JustPressed(PSP_CTRL_RIGHT)) g_creativeInv.moveRight();
      if (PSPInput_JustPressed(PSP_CTRL_LEFT)) g_creativeInv.moveLeft();
      if (PSPInput_JustPressed(PSP_CTRL_UP)) g_creativeInv.moveUp();
      if (PSPInput_JustPressed(PSP_CTRL_DOWN)) g_creativeInv.moveDown();
      if (PSPInput_JustPressed(PSP_CTRL_CROSS)) g_creativeInv.pressCross();
    }
  }
  g_heldBlock = g_creativeInv.heldBlock();

  // Block breaking
  bool doBreak = false;
  if (PSPInput_IsHeld(PSP_CTRL_LTRIGGER) && breakCooldown <= 0.0f) {
    doBreak = true;
    breakCooldown = 0.15f;
  }

  if (doBreak && g_hitResult.hit) {
    uint8_t oldBlock = g_level->getBlock(g_hitResult.x, g_hitResult.y, g_hitResult.z);
    if (oldBlock != BLOCK_BEDROCK) {
      int bx = g_hitResult.x, by = g_hitResult.y, bz = g_hitResult.z;
      g_level->setBlock(bx, by, bz, BLOCK_AIR);
      g_level->markDirty(bx, by, bz);

      // Cascading plant break (if we broke the soil, the plant pops off)
      uint8_t topId = g_level->getBlock(bx, by + 1, bz);
      if (topId != BLOCK_AIR && !g_blockProps[topId].isSolid() && !g_blockProps[topId].isLiquid()) {
          g_level->setBlock(bx, by + 1, bz, BLOCK_AIR);
          g_level->markDirty(bx, by + 1, bz);
      }
    }
  }

  // Place block
  if (!g_creativeInv.isOpen() &&
      g_heldBlock != BLOCK_AIR &&
      PSPInput_JustPressed(PSP_CTRL_RTRIGGER) &&
      !PSPInput_IsHeld(PSP_CTRL_LTRIGGER) &&
      g_hitResult.hit) {
    int px = g_hitResult.nx;
    int py = g_hitResult.ny;
    int pz = g_hitResult.nz;

    // If we click on a plant, replace the plant directly instead of placing adjacent
    uint8_t hitId = g_level->getBlock(g_hitResult.x, g_hitResult.y, g_hitResult.z);
    if (hitId != BLOCK_AIR && !g_blockProps[hitId].isSolid() && !g_blockProps[hitId].isLiquid()) {
      px = g_hitResult.x;
      py = g_hitResult.y;
      pz = g_hitResult.z;
    }

    // If we are placing a plant, check if the block below is valid soil (grass/dirt/farmland)
    bool canPlace = true;
    if (g_heldBlock == BLOCK_SAPLING || g_heldBlock == BLOCK_TALLGRASS || g_heldBlock == BLOCK_FLOWER || 
        g_heldBlock == BLOCK_ROSE || g_heldBlock == BLOCK_MUSHROOM_BROWN || g_heldBlock == BLOCK_MUSHROOM_RED) {
      uint8_t floorId = g_level->getBlock(px, py - 1, pz);
      if (floorId != BLOCK_GRASS && floorId != BLOCK_DIRT && floorId != BLOCK_FARMLAND) {
        canPlace = false;
      }
    }

    // Don't place if it would overlap with the player
    int playerMinX = (int)floorf(g_player.x - R);
    int playerMaxX = (int)floorf(g_player.x + R);
    int playerMinY = (int)floorf(g_player.y);
    int playerMaxY = (int)floorf(g_player.y + H);
    int playerMinZ = (int)floorf(g_player.z - R);
    int playerMaxZ = (int)floorf(g_player.z + R);

    bool overlaps = (px >= playerMinX && px <= playerMaxX &&
                     py >= playerMinY && py <= playerMaxY &&
                     pz >= playerMinZ && pz <= playerMaxZ);

    uint8_t targetBlock = g_level->getBlock(px, py, pz);
    bool canReplaceTarget = (targetBlock == BLOCK_AIR ||
                             isWaterId(targetBlock) ||
                             isLavaId(targetBlock) ||
                             (!g_blockProps[targetBlock].isSolid() && !g_blockProps[targetBlock].isLiquid()));

    if (canPlace && !overlaps && canReplaceTarget) {
      g_level->setBlock(px, py, pz, g_heldBlock);
      g_level->markDirty(px, py, pz);
    }
  }

}

static void game_render() {
  if (g_inMainMenu) {
    PSPRenderer_BeginFrame(0xFF101018);
    sceGuDisable(GU_DEPTH_TEST);
    sceGuDisable(GU_CULL_FACE);
    sceGuEnable(GU_BLEND);
    sceGuBlendFunc(GU_ADD, GU_SRC_ALPHA, GU_ONE_MINUS_SRC_ALPHA, 0, 0);

    hudDrawText5x7(82.0f, 20.0f, "MINECRAFT PSP - WORLDS", 0xFFFFFFFF, 1.6f);
    hudDrawRect(152.0f, 44.0f, 176.0f, 24.0f, g_menuCreateSelected ? 0xA0606060 : 0x80353535);
    hudDrawText5x7(165.0f, 52.0f, "CREATE WORLD (X)", 0xFFFFFFFF, 1.2f);

    for (int i = 0; i < kMaxWorldSlots; ++i) {
      if (!g_worldExists[i]) continue;
      float y = 70.0f + i * 20.0f;
      if (!g_menuCreateSelected && i == g_menuSlotSel) hudDrawRect(12.0f, y - 2.0f, 188.0f, 16.0f, 0xA0606060);
      char line[64];
      snprintf(line, sizeof(line), "WORLD%d", i + 1);
      hudDrawText5x7(20.0f, y, line, 0xFFFFFFFF, 1.5f);
    }
    hudDrawText5x7(208.0f, 88.0f, "X ENTER", 0xFFC0C0C0, 1.2f);
    hudDrawText5x7(208.0f, 104.0f, "TRIANGLE DELETE", 0xFFC0C0C0, 1.2f);
    hudDrawText5x7(208.0f, 120.0f, "UP/DOWN SELECT", 0xFFC0C0C0, 1.2f);

    if (g_menuDeleteConfirm) {
      hudDrawRect(110.0f, 90.0f, 260.0f, 90.0f, 0xC0000000);
      hudDrawText5x7(128.0f, 104.0f, "DELETE WORLD?", 0xFFFFFFFF, 1.8f);
      hudDrawRect(150.0f, 138.0f, 70.0f, 20.0f, g_menuDeleteYesSelected ? 0xA0606060 : 0x80404040);
      hudDrawRect(260.0f, 138.0f, 70.0f, 20.0f, !g_menuDeleteYesSelected ? 0xA0606060 : 0x80404040);
      hudDrawText5x7(176.0f, 145.0f, "YES", 0xFFFFFFFF, 1.2f);
      hudDrawText5x7(288.0f, 145.0f, "NO", 0xFFFFFFFF, 1.2f);
    }
    if (g_statusTimer > 0.0f && g_statusText[0]) {
      hudDrawRect(92.0f, 236.0f, 296.0f, 16.0f, 0xA0000000);
      hudDrawText5x7(100.0f, 240.0f, g_statusText, g_statusColor, 1.1f);
    }

    sceGuDisable(GU_BLEND);
    sceGuEnable(GU_CULL_FACE);
    sceGuEnable(GU_DEPTH_TEST);
    PSPRenderer_EndFrame();
    return;
  }

  float _tod = g_level->getTimeOfDay();

  // Camera setup
  ScePspFVector3 camPos = {g_player.x, g_player.y + 1.62f, g_player.z}; // 4J: heightOffset = 1.62
  
  float yawRad = g_player.yaw * Mth::DEGRAD;
  float pitchRad = g_player.pitch * Mth::DEGRAD;

  ScePspFVector3 lookDir = {
      Mth::sin(yawRad) * Mth::cos(pitchRad), // X
      Mth::sin(pitchRad),                    // Y
      Mth::cos(yawRad) * Mth::cos(pitchRad)  // Z
  };

  ScePspFVector3 lookAt = {camPos.x + lookDir.x, camPos.y + lookDir.y,
                           camPos.z + lookDir.z};

  // Compute fog color
  uint32_t clearColor = 0xFF000000;
  if (g_skyRenderer) {
      clearColor = g_skyRenderer->getFogColor(_tod, lookDir);
  }
  {
    int wx = (int)floorf(camPos.x);
    int wy = (int)floorf(camPos.y);
    int wz = (int)floorf(camPos.z);
    if (isWaterId(g_level->getBlock(wx, wy, wz))) {
      // Underwater tint/fog approximation.
      clearColor = 0xFF4A1C06;
    }
  }

  PSPRenderer_BeginFrame(clearColor);

  PSPRenderer_SetCamera(&camPos, &lookAt);

  if (g_skyRenderer)
    g_skyRenderer->renderSky(g_player.x, g_player.y, g_player.z, lookDir);

  // Render chunks
  g_chunkRenderer->render(g_player.x, g_player.y, g_player.z);

  // Render block highlight wireframe
  if (g_hitResult.hit) {
    BlockHighlight_Draw(g_hitResult.x, g_hitResult.y, g_hitResult.z, g_hitResult.id);
  }

  if (g_cloudRenderer)
    g_cloudRenderer->renderClouds(g_player.x, g_player.y, g_player.z, g_tickAlpha);

  // 2D HUD pass
  sceGuDisable(GU_DEPTH_TEST);
  sceGuDisable(GU_CULL_FACE);
  sceGuEnable(GU_BLEND);
  sceGuBlendFunc(GU_ADD, GU_SRC_ALPHA, GU_ONE_MINUS_SRC_ALPHA, 0, 0);
  drawHotbarHUD();
  if (!g_creativeInv.isOpen() && g_hitResult.hit) {
    const char *lookName = getBlockDisplayName(g_hitResult.id);
    const float textScale = 1.1f;
    const float nameW = hudMeasureText5x7(lookName, textScale);
    const float nameX = (480.0f - nameW) * 0.5f;
    const float nameY = 12.0f;
    hudDrawRect(nameX - 3.0f, nameY - 2.0f, nameW + 6.0f, 12.0f, 0xA0000000);
    hudDrawText5x7(nameX, nameY, lookName, 0xFFFFFFFF, textScale);
  }
  if (g_pauseOpen) {
    hudDrawRect(110.0f, 64.0f, 260.0f, 140.0f, 0xB0000000);
    hudDrawText5x7(180.0f, 88.0f, "PAUSE", 0xFFFFFFFF, 2.0f);
    float optionY = (g_pauseSel == 0) ? 114.0f : (g_pauseSel == 1 ? 138.0f : 162.0f);
    hudDrawRect(132.0f, optionY, 216.0f, 18.0f, 0x80505050);
    hudDrawText5x7(164.0f, 122.0f, "CONTINUE", 0xFFFFFFFF, 1.5f);
    hudDrawText5x7(164.0f, 148.0f, "SAVE GAME", 0xFFFFFFFF, 1.5f);
    hudDrawText5x7(164.0f, 172.0f, "SAVE AND EXIT", 0xFFFFFFFF, 1.5f);
    hudDrawText5x7(130.0f, 190.0f, "X SELECT  O BACK", 0xFFC0C0C0, 1.2f);
  }
  if (g_statusTimer > 0.0f && g_statusText[0]) {
    hudDrawRect(120.0f, 12.0f, 240.0f, 16.0f, 0xA0000000);
    hudDrawText5x7(128.0f, 16.0f, g_statusText, g_statusColor, 1.1f);
  }
  sceGuDisable(GU_BLEND);
  sceGuEnable(GU_CULL_FACE);
  sceGuEnable(GU_DEPTH_TEST);

  PSPRenderer_EndFrame();
}

// Main entry point
int main(int argc, char *argv[]) {
  setup_callbacks();

  if (!game_init()) {
    pspDebugScreenInit();
    pspDebugScreenPrintf("Init error!\n");
    sceKernelSleepThread();
    return 1;
  }

  uint64_t lastTime = sceKernelGetSystemTimeWide();

  while (true) {
    uint64_t now = sceKernelGetSystemTimeWide();
    float dt = (float)(now - lastTime) / 1000000.0f; // microseconds -> seconds
    if (dt > 0.05f)
      dt = 0.05f; // cap at 20 FPS min
    lastTime = now;

    game_update(dt);
    game_render();
  }

  return 0;
}
