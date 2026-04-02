#include "Level.h"
#include "Random.h"
#include "WorldGen.h"
#include "TreeFeature.h"
#include <vector>
#include <algorithm>
#include <functional>
#include <string.h>
#include <stdio.h>
#include <stdint.h>

struct LightNode {
  int x, y, z;
};

bool Level::isWaterBlock(uint8_t id) const {
  return id == BLOCK_WATER_STILL || id == BLOCK_WATER_FLOW;
}

bool Level::isLavaBlock(uint8_t id) const {
  return id == BLOCK_LAVA_STILL || id == BLOCK_LAVA_FLOW;
}

void Level::setSimulationFocus(int wx, int wy, int wz, int radius) {
  m_simFocusX = wx;
  m_simFocusY = wy;
  m_simFocusZ = wz;
  m_simFocusRadius = radius;
}

void Level::tick() {
  m_time += 1;
  if (m_waterDirty) tickWater();
  if (m_lavaDirty) tickLava();
  if (!m_suspendLightingUpdates && !m_lightUpdateQueue.empty()) {
    const int maxLightUpdatesPerTick = 24;
    int processed = 0;
    const int maxX = WORLD_CHUNKS_X * CHUNK_SIZE_X;
    const int maxZ = WORLD_CHUNKS_Z * CHUNK_SIZE_Z;
    while (processed < maxLightUpdatesPerTick && !m_lightUpdateQueue.empty()) {
      int idx = m_lightUpdateQueue.front();
      m_lightUpdateQueue.pop_front();
      int x = idx % maxX;
      int tmp = idx / maxX;
      int z = tmp % maxZ;
      int y = tmp / maxZ;
      if (y >= 0 && y < CHUNK_SIZE_Y) updateLight(x, y, z);
      processed++;
    }
  }
  if (m_waterWakeTicks > 0) m_waterWakeTicks--;
  if (m_lavaWakeTicks > 0) m_lavaWakeTicks--;
}

void Level::tickWater() {
  // Budget per world tick (world ticks run at fixed 20 TPS from main loop).
  const int maxTicksPerWorldTick = 128;
  int processed = 0;
  while (processed < maxTicksPerWorldTick && !m_waterTicks.empty()) {
    WaterTickNode n = m_waterTicks.top();
    if (n.dueTick > (int)m_time) break;
    m_waterTicks.pop();
    if (n.idx < 0 || n.idx >= (int)m_waterDue.size()) continue;
    if (m_waterDue[n.idx] != n.dueTick) continue;
    m_waterDue[n.idx] = -1;

    int maxX = WORLD_CHUNKS_X * CHUNK_SIZE_X;
    int maxZ = WORLD_CHUNKS_Z * CHUNK_SIZE_Z;
    int x = n.idx % maxX;
    int tmp = n.idx / maxX;
    int z = tmp % maxZ;
    int y = tmp / maxZ;
    processWaterCell(x, y, z);
    processed++;
  }
  m_waterDirty = !m_waterTicks.empty();
}

void Level::tickLava() {
  // Lava moves slower than water and has a lower per-tick processing budget.
  const int maxTicksPerWorldTick = 64;
  int processed = 0;
  while (processed < maxTicksPerWorldTick && !m_lavaTicks.empty()) {
    WaterTickNode n = m_lavaTicks.top();
    if (n.dueTick > (int)m_time) break;
    m_lavaTicks.pop();
    if (n.idx < 0 || n.idx >= (int)m_lavaDue.size()) continue;
    if (m_lavaDue[n.idx] != n.dueTick) continue;
    m_lavaDue[n.idx] = -1;

    int maxX = WORLD_CHUNKS_X * CHUNK_SIZE_X;
    int maxZ = WORLD_CHUNKS_Z * CHUNK_SIZE_Z;
    int x = n.idx % maxX;
    int tmp = n.idx / maxX;
    int z = tmp % maxZ;
    int y = tmp / maxZ;
    processLavaCell(x, y, z);
    processed++;
  }
  m_lavaDirty = !m_lavaTicks.empty();
}

void Level::processWaterCell(int x, int y, int z) {
  int maxX = WORLD_CHUNKS_X * CHUNK_SIZE_X;
  int maxZ = WORLD_CHUNKS_Z * CHUNK_SIZE_Z;
  auto inBounds = [&](int wx, int wy, int wz) {
    return wx >= 0 && wx < maxX && wz >= 0 && wz < maxZ && wy >= 0 && wy < CHUNK_SIZE_Y;
  };
  if (!inBounds(x, y, z)) return;
  uint8_t id = getBlock(x, y, z);
  if (!isWaterBlock(id)) return;

  auto getDepth = [&](int wx, int wy, int wz) -> int {
    if (!inBounds(wx, wy, wz)) return -1;
    uint8_t bid = getBlock(wx, wy, wz);
    if (!isWaterBlock(bid)) return -1;
    uint8_t d = getWaterDepth(wx, wy, wz);
    if (d == 0xFF) d = (bid == BLOCK_WATER_STILL) ? 0 : 1;
    return (int)d;
  };
  auto isWaterBlocking = [&](int wx, int wy, int wz) -> bool {
    if (!inBounds(wx, wy, wz)) return true;
    uint8_t bid = getBlock(wx, wy, wz);
    if (bid == BLOCK_AIR) return false;
    return g_blockProps[bid].isSolid();
  };
  auto canSpreadTo = [&](int wx, int wy, int wz) -> bool {
    if (!inBounds(wx, wy, wz)) return false;
    uint8_t bid = getBlock(wx, wy, wz);
    if (isWaterBlock(bid)) return false;
    return !isWaterBlocking(wx, wy, wz);
  };
  auto setWaterCell = [&](int wx, int wy, int wz, uint8_t bid, uint8_t d) {
    m_inWaterSimUpdate = true;
    setBlock(wx, wy, wz, bid);
    m_inWaterSimUpdate = false;
    setWaterDepth(wx, wy, wz, d);
  };

  int depth = getDepth(x, y, z);
  if (depth < 0) return;
  int dropOff = 1;
  bool becomeStatic = true;
  int maxCount = 0;

  if (depth > 0) {
    int highest = -100;
    static const int dx[4] = {-1, 1, 0, 0};
    static const int dz[4] = {0, 0, -1, 1};
    for (int i = 0; i < 4; ++i) {
      int nd = getDepth(x + dx[i], y, z + dz[i]);
      if (nd < 0) continue;
      if (nd == 0) maxCount++;
      if (nd >= 8) nd = 0;
      if (highest < 0 || nd < highest) highest = nd;
    }

    int above = getDepth(x, y + 1, z);
    int newDepth = highest + dropOff;
    if (newDepth >= 8 || highest < 0) newDepth = -1;
    if (above >= 0) newDepth = (above >= 8) ? above : (above + 8);

    if (maxCount >= 2) {
      if (isWaterBlocking(x, y - 1, z)) newDepth = 0;
      else if (isWaterBlock(getBlock(x, y - 1, z)) && getDepth(x, y - 1, z) == 0) newDepth = 0;
    }

    if (newDepth != depth) {
      if (newDepth < 0) {
        setWaterCell(x, y, z, BLOCK_AIR, 0xFF);
        wakeWaterNeighborhood(x, y, z, 5);
        return;
      }

      depth = newDepth;
      setWaterDepth(x, y, z, (uint8_t)depth);
      scheduleWaterTick(x, y, z, 5);
      wakeWaterNeighborhood(x, y, z, 5);
    } else {
      if (becomeStatic) setWaterCell(x, y, z, BLOCK_WATER_STILL, (uint8_t)depth);
    }
  } else {
    setWaterCell(x, y, z, BLOCK_WATER_STILL, 0);
  }

  if (canSpreadTo(x, y - 1, z)) {
    int d = (depth >= 8) ? depth : (depth + 8);
    if (d > 7) d = 7;
    setWaterCell(x, y - 1, z, BLOCK_WATER_FLOW, (uint8_t)d);
    scheduleWaterTick(x, y - 1, z, 5);
  } else if (depth >= 0 && (depth == 0 || isWaterBlocking(x, y - 1, z))) {
    int neighbor = depth + dropOff;
    if (depth >= 8) neighbor = 1;
    if (neighbor >= 8) return;

    static const int dx[4] = {-1, 1, 0, 0};
    static const int dz[4] = {0, 0, -1, 1};
    int dist[4] = {1000, 1000, 1000, 1000};
    bool spread[4] = {false, false, false, false};
    static const int opposite[4] = {1, 0, 3, 2};
    const int maxSlopePass = 6; // expand pit detection by one more block
    std::function<int(int, int, int, int, int)> slopeDistance =
        [&](int wx, int wy, int wz, int pass, int from) -> int {
      int lowest = 1000;
      for (int d = 0; d < 4; ++d) {
        if (from >= 0 && d == opposite[from]) continue;
        int nx = wx + dx[d], nz = wz + dz[d];
        if (isWaterBlocking(nx, wy, nz)) continue;
        int nd = getDepth(nx, wy, nz);
        if (nd == 0) continue;
        if (!isWaterBlocking(nx, wy - 1, nz)) return pass;
        if (pass < maxSlopePass) {
          int v = slopeDistance(nx, wy, nz, pass + 1, d);
          if (v < lowest) lowest = v;
        }
      }
      return lowest;
    };

    for (int d = 0; d < 4; ++d) {
      int nx = x + dx[d], nz = z + dz[d];
      if (isWaterBlocking(nx, y, nz)) continue;
      int nd = getDepth(nx, y, nz);
      if (nd == 0) continue;
      if (!isWaterBlocking(nx, y - 1, nz)) dist[d] = 0;
      else dist[d] = slopeDistance(nx, y, nz, 1, d);
    }
    int lowest = dist[0];
    for (int d = 1; d < 4; ++d) if (dist[d] < lowest) lowest = dist[d];
    for (int d = 0; d < 4; ++d) spread[d] = (dist[d] == lowest);
    for (int d = 0; d < 4; ++d) {
      if (!spread[d]) continue;
      int nx = x + dx[d], nz = z + dz[d];
      if (!canSpreadTo(nx, y, nz)) continue;
      setWaterCell(nx, y, nz, BLOCK_WATER_FLOW, (uint8_t)neighbor);
      scheduleWaterTick(nx, y, nz, 5);
    }
  }
}

void Level::processLavaCell(int x, int y, int z) {
  int maxX = WORLD_CHUNKS_X * CHUNK_SIZE_X;
  int maxZ = WORLD_CHUNKS_Z * CHUNK_SIZE_Z;
  auto inBounds = [&](int wx, int wy, int wz) {
    return wx >= 0 && wx < maxX && wz >= 0 && wz < maxZ && wy >= 0 && wy < CHUNK_SIZE_Y;
  };
  if (!inBounds(x, y, z)) return;
  uint8_t id = getBlock(x, y, z);
  if (!isLavaBlock(id)) return;

  auto getDepth = [&](int wx, int wy, int wz) -> int {
    if (!inBounds(wx, wy, wz)) return -1;
    uint8_t bid = getBlock(wx, wy, wz);
    if (!isLavaBlock(bid)) return -1;
    uint8_t d = m_lavaDepth[waterIndex(wx, wy, wz)];
    if (d == 0xFF) d = (bid == BLOCK_LAVA_STILL) ? 0 : 1;
    return (int)d;
  };
  auto isLavaBlocking = [&](int wx, int wy, int wz) -> bool {
    if (!inBounds(wx, wy, wz)) return true;
    uint8_t bid = getBlock(wx, wy, wz);
    if (bid == BLOCK_AIR) return false;
    return g_blockProps[bid].isSolid();
  };
  auto canSpreadTo = [&](int wx, int wy, int wz) -> bool {
    if (!inBounds(wx, wy, wz)) return false;
    uint8_t bid = getBlock(wx, wy, wz);
    if (isLavaBlock(bid)) return false;
    return !isLavaBlocking(wx, wy, wz);
  };
  auto setLavaCell = [&](int wx, int wy, int wz, uint8_t bid, uint8_t d) {
    m_inWaterSimUpdate = true;
    setBlock(wx, wy, wz, bid);
    m_inWaterSimUpdate = false;
    m_lavaDepth[waterIndex(wx, wy, wz)] = d;
  };

  int depth = getDepth(x, y, z);
  if (depth < 0) return;
  const int dropOff = 2;
  bool becomeStatic = true;
  int maxCount = 0;

  if (depth > 0) {
    int highest = -100;
    static const int dx[4] = {-1, 1, 0, 0};
    static const int dz[4] = {0, 0, -1, 1};
    for (int i = 0; i < 4; ++i) {
      int nd = getDepth(x + dx[i], y, z + dz[i]);
      if (nd < 0) continue;
      if (nd == 0) maxCount++;
      if (nd >= 8) nd = 0;
      if (highest < 0 || nd < highest) highest = nd;
    }

    int above = getDepth(x, y + 1, z);
    int newDepth = highest + dropOff;
    if (newDepth >= 8 || highest < 0) newDepth = -1;
    if (above >= 0) newDepth = (above >= 8) ? above : (above + 8);

    if (maxCount >= 2) {
      if (isLavaBlocking(x, y - 1, z)) newDepth = 0;
      else if (isLavaBlock(getBlock(x, y - 1, z)) && getDepth(x, y - 1, z) == 0) newDepth = 0;
    }

    if (newDepth != depth) {
      if (newDepth < 0) {
        setLavaCell(x, y, z, BLOCK_AIR, 0xFF);
        wakeLavaNeighborhood(x, y, z, 12);
        return;
      }

      depth = newDepth;
      m_lavaDepth[waterIndex(x, y, z)] = (uint8_t)depth;
      scheduleLavaTick(x, y, z, 12);
      wakeLavaNeighborhood(x, y, z, 12);
    } else {
      if (becomeStatic) setLavaCell(x, y, z, BLOCK_LAVA_STILL, (uint8_t)depth);
    }
  } else {
    setLavaCell(x, y, z, BLOCK_LAVA_STILL, 0);
  }

  if (canSpreadTo(x, y - 1, z)) {
    int d = (depth >= 8) ? depth : (depth + 8);
    if (d > 7) d = 7;
    setLavaCell(x, y - 1, z, BLOCK_LAVA_FLOW, (uint8_t)d);
    scheduleLavaTick(x, y - 1, z, 12);
  } else if (depth >= 0 && (depth == 0 || isLavaBlocking(x, y - 1, z))) {
    int neighbor = depth + dropOff;
    if (depth >= 8) neighbor = 2;
    if (neighbor >= 8) return;

    static const int dx[4] = {-1, 1, 0, 0};
    static const int dz[4] = {0, 0, -1, 1};
    int dist[4] = {1000, 1000, 1000, 1000};
    bool spread[4] = {false, false, false, false};
    static const int opposite[4] = {1, 0, 3, 2};
    const int maxSlopePass = 3;
    std::function<int(int, int, int, int, int)> slopeDistance =
        [&](int wx, int wy, int wz, int pass, int from) -> int {
      int lowest = 1000;
      for (int d = 0; d < 4; ++d) {
        if (from >= 0 && d == opposite[from]) continue;
        int nx = wx + dx[d], nz = wz + dz[d];
        if (isLavaBlocking(nx, wy, nz)) continue;
        int nd = getDepth(nx, wy, nz);
        if (nd == 0) continue;
        if (!isLavaBlocking(nx, wy - 1, nz)) return pass;
        if (pass < maxSlopePass) {
          int v = slopeDistance(nx, wy, nz, pass + 1, d);
          if (v < lowest) lowest = v;
        }
      }
      return lowest;
    };

    for (int d = 0; d < 4; ++d) {
      int nx = x + dx[d], nz = z + dz[d];
      if (isLavaBlocking(nx, y, nz)) continue;
      int nd = getDepth(nx, y, nz);
      if (nd == 0) continue;
      if (!isLavaBlocking(nx, y - 1, nz)) dist[d] = 0;
      else dist[d] = slopeDistance(nx, y, nz, 1, d);
    }
    int lowest = dist[0];
    for (int d = 1; d < 4; ++d) if (dist[d] < lowest) lowest = dist[d];
    for (int d = 0; d < 4; ++d) spread[d] = (dist[d] == lowest);
    for (int d = 0; d < 4; ++d) {
      if (!spread[d]) continue;
      int nx = x + dx[d], nz = z + dz[d];
      if (!canSpreadTo(nx, y, nz)) continue;
      setLavaCell(nx, y, nz, BLOCK_LAVA_FLOW, (uint8_t)neighbor);
      scheduleLavaTick(nx, y, nz, 12);
    }
  }
}

Level::Level() {
  memset(m_chunks, 0, sizeof(m_chunks));
  m_waterDepth.resize(WORLD_CHUNKS_X * CHUNK_SIZE_X * CHUNK_SIZE_Y * WORLD_CHUNKS_Z * CHUNK_SIZE_Z, 0xFF);
  m_waterDue.resize(m_waterDepth.size(), -1);
  m_lavaDepth.resize(m_waterDepth.size(), 0xFF);
  m_lavaDue.resize(m_waterDepth.size(), -1);
  for (int cx = 0; cx < WORLD_CHUNKS_X; cx++)
    for (int cz = 0; cz < WORLD_CHUNKS_Z; cz++)
      m_chunks[cx][cz] = new Chunk();
}

Level::~Level() {
  for (int cx = 0; cx < WORLD_CHUNKS_X; cx++)
    for (int cz = 0; cz < WORLD_CHUNKS_Z; cz++)
      delete m_chunks[cx][cz];
}

Chunk* Level::getChunk(int cx, int cz) const {
  if (cx < 0 || cx >= WORLD_CHUNKS_X || cz < 0 || cz >= WORLD_CHUNKS_Z) return nullptr;
  return m_chunks[cx][cz];
}

void Level::markDirty(int wx, int wy, int wz) {
  int cx = wx >> 4;
  int cz = wz >> 4;
  int sy = wy >> 4;
  if (cx >= 0 && cx < WORLD_CHUNKS_X && cz >= 0 && cz < WORLD_CHUNKS_Z && sy >= 0 && sy < SUBCHUNK_COUNT) {
    m_chunks[cx][cz]->dirty[sy] = true;
    
    // Dirty neighbor subchunks
    int lx = wx & 0xF;
    int lz = wz & 0xF;
    int ly = wy & 0xF;
    
    if (lx == 0 && cx > 0) m_chunks[cx - 1][cz]->dirty[sy] = true;
    if (lx == 15 && cx < WORLD_CHUNKS_X - 1) m_chunks[cx + 1][cz]->dirty[sy] = true;
    if (lz == 0 && cz > 0) m_chunks[cx][cz - 1]->dirty[sy] = true;
    if (lz == 15 && cz < WORLD_CHUNKS_Z - 1) m_chunks[cx][cz + 1]->dirty[sy] = true;
    if (ly == 0 && sy > 0) m_chunks[cx][cz]->dirty[sy - 1] = true;
    if (ly == 15 && sy < SUBCHUNK_COUNT - 1) m_chunks[cx][cz]->dirty[sy + 1] = true;
  }
}

uint8_t Level::getBlock(int wx, int wy, int wz) const {
  int cx = wx >> 4;
  int cz = wz >> 4;
  if (cx < 0 || cx >= WORLD_CHUNKS_X || cz < 0 || cz >= WORLD_CHUNKS_Z || wy < 0 || wy >= CHUNK_SIZE_Y) return BLOCK_AIR;
  return m_chunks[cx][cz]->getBlock(wx & 0xF, wy, wz & 0xF);
}

int Level::waterIndex(int wx, int wy, int wz) const {
  return ((wy * (WORLD_CHUNKS_Z * CHUNK_SIZE_Z) + wz) * (WORLD_CHUNKS_X * CHUNK_SIZE_X) + wx);
}

uint8_t Level::getWaterDepth(int wx, int wy, int wz) const {
  int maxX = WORLD_CHUNKS_X * CHUNK_SIZE_X;
  int maxZ = WORLD_CHUNKS_Z * CHUNK_SIZE_Z;
  if (wx < 0 || wx >= maxX || wz < 0 || wz >= maxZ || wy < 0 || wy >= CHUNK_SIZE_Y) return 0xFF;
  return m_waterDepth[waterIndex(wx, wy, wz)];
}

uint8_t Level::getLavaDepth(int wx, int wy, int wz) const {
  int maxX = WORLD_CHUNKS_X * CHUNK_SIZE_X;
  int maxZ = WORLD_CHUNKS_Z * CHUNK_SIZE_Z;
  if (wx < 0 || wx >= maxX || wz < 0 || wz >= maxZ || wy < 0 || wy >= CHUNK_SIZE_Y) return 0xFF;
  return m_lavaDepth[waterIndex(wx, wy, wz)];
}

void Level::setWaterDepth(int wx, int wy, int wz, uint8_t depth) {
  int maxX = WORLD_CHUNKS_X * CHUNK_SIZE_X;
  int maxZ = WORLD_CHUNKS_Z * CHUNK_SIZE_Z;
  if (wx < 0 || wx >= maxX || wz < 0 || wz >= maxZ || wy < 0 || wy >= CHUNK_SIZE_Y) return;
  m_waterDepth[waterIndex(wx, wy, wz)] = depth;
}

void Level::scheduleWaterTick(int wx, int wy, int wz, int delayTicks) {
  int maxX = WORLD_CHUNKS_X * CHUNK_SIZE_X;
  int maxZ = WORLD_CHUNKS_Z * CHUNK_SIZE_Z;
  if (wx < 0 || wx >= maxX || wz < 0 || wz >= maxZ || wy < 0 || wy >= CHUNK_SIZE_Y) return;
  int idx = waterIndex(wx, wy, wz);
  int due = (int)m_time + delayTicks;
  if (idx < 0 || idx >= (int)m_waterDue.size()) return;
  if (m_waterDue[idx] != -1 && m_waterDue[idx] <= due) return;
  m_waterDue[idx] = due;
  m_waterTicks.push({due, idx});
  m_waterDirty = true;
}

void Level::scheduleLavaTick(int wx, int wy, int wz, int delayTicks) {
  int maxX = WORLD_CHUNKS_X * CHUNK_SIZE_X;
  int maxZ = WORLD_CHUNKS_Z * CHUNK_SIZE_Z;
  if (wx < 0 || wx >= maxX || wz < 0 || wz >= maxZ || wy < 0 || wy >= CHUNK_SIZE_Y) return;
  int idx = waterIndex(wx, wy, wz);
  int due = (int)m_time + delayTicks;
  if (idx < 0 || idx >= (int)m_lavaDue.size()) return;
  if (m_lavaDue[idx] != -1 && m_lavaDue[idx] <= due) return;
  m_lavaDue[idx] = due;
  m_lavaTicks.push({due, idx});
  m_lavaDirty = true;
}

void Level::wakeWaterNeighborhood(int wx, int wy, int wz, int delayTicks) {
  static const int dx[7] = {0, -1, 1, 0, 0, 0, 0};
  static const int dy[7] = {0, 0, 0, -1, 1, 0, 0};
  static const int dz[7] = {0, 0, 0, 0, 0, -1, 1};
  for (int i = 0; i < 7; ++i) {
    int nx = wx + dx[i], ny = wy + dy[i], nz = wz + dz[i];
    if (isWaterBlock(getBlock(nx, ny, nz))) scheduleWaterTick(nx, ny, nz, delayTicks);
  }
}

void Level::wakeLavaNeighborhood(int wx, int wy, int wz, int delayTicks) {
  static const int dx[7] = {0, -1, 1, 0, 0, 0, 0};
  static const int dy[7] = {0, 0, 0, -1, 1, 0, 0};
  static const int dz[7] = {0, 0, 0, 0, 0, -1, 1};
  for (int i = 0; i < 7; ++i) {
    int nx = wx + dx[i], ny = wy + dy[i], nz = wz + dz[i];
    if (isLavaBlock(getBlock(nx, ny, nz))) scheduleLavaTick(nx, ny, nz, delayTicks);
  }
}

void Level::setBlock(int wx, int wy, int wz, uint8_t id) {
  int cx = wx >> 4;
  int cz = wz >> 4;
  if (cx < 0 || cx >= WORLD_CHUNKS_X || cz < 0 || cz >= WORLD_CHUNKS_Z || wy < 0 || wy >= CHUNK_SIZE_Y) return;
  uint8_t oldId = m_chunks[cx][cz]->getBlock(wx & 0xF, wy, wz & 0xF);
  if (oldId == id) return;
  
  m_chunks[cx][cz]->setBlock(wx & 0xF, wy, wz & 0xF, id);
  if (isWaterBlock(id)) {
    setWaterDepth(wx, wy, wz, (id == BLOCK_WATER_STILL) ? 0 : 1);
    m_lavaDepth[waterIndex(wx, wy, wz)] = 0xFF;
  } else if (isLavaBlock(id)) {
    m_lavaDepth[waterIndex(wx, wy, wz)] = (id == BLOCK_LAVA_STILL) ? 0 : 1;
    setWaterDepth(wx, wy, wz, 0xFF);
  } else {
    setWaterDepth(wx, wy, wz, 0xFF);
    m_lavaDepth[waterIndex(wx, wy, wz)] = 0xFF;
  }

  bool touchesFluid = isWaterBlock(oldId) || isWaterBlock(id) || isLavaBlock(oldId) || isLavaBlock(id);
  if (!touchesFluid) {
    static const int dx[6] = {-1, 1, 0, 0, 0, 0};
    static const int dy[6] = {0, 0, -1, 1, 0, 0};
    static const int dz[6] = {0, 0, 0, 0, -1, 1};
    for (int i = 0; i < 6; ++i) {
      int nx = wx + dx[i], ny = wy + dy[i], nz = wz + dz[i];
      if (ny < 0 || ny >= CHUNK_SIZE_Y) continue;
      uint8_t n = getBlock(nx, ny, nz);
      if (isWaterBlock(n) || isLavaBlock(n)) {
        touchesFluid = true;
        break;
      }
    }
  }
  if (touchesFluid) {
    if (!m_inWaterSimUpdate) {
      m_waterDirty = true;
      m_lavaDirty = true;
      m_waterWakeX = wx;
      m_waterWakeY = wy;
      m_waterWakeZ = wz;
      m_waterWakeTicks = 16;
      m_lavaWakeX = wx;
      m_lavaWakeY = wy;
      m_lavaWakeZ = wz;
      m_lavaWakeTicks = 20;
      wakeWaterNeighborhood(wx, wy, wz, 5);
      wakeLavaNeighborhood(wx, wy, wz, 12);
    }
  }

  if (m_suspendLightingUpdates || m_inWaterSimUpdate) {
    queueLightUpdate(wx, wy, wz);
    static const int dx[6] = {-1, 1, 0, 0, 0, 0};
    static const int dy[6] = {0, 0, -1, 1, 0, 0};
    static const int dz[6] = {0, 0, 0, 0, -1, 1};
    for (int i = 0; i < 6; ++i) queueLightUpdate(wx + dx[i], wy + dy[i], wz + dz[i]);
  } else {
    updateLight(wx, wy, wz);
  }
}

std::vector<AABB> Level::getCubes(const AABB& box) const {
  std::vector<AABB> boxes;
  int x0 = (int)floorf(box.x0);
  int x1 = (int)floorf(box.x1 + 1.0f);
  int y0 = (int)floorf(box.y0);
  int y1 = (int)floorf(box.y1 + 1.0f);
  int z0 = (int)floorf(box.z0);
  int z1 = (int)floorf(box.z1 + 1.0f);

  if (x0 < 0) x0 = 0;
  if (y0 < 0) y0 = 0;
  if (z0 < 0) z0 = 0;
  if (x1 > WORLD_CHUNKS_X * CHUNK_SIZE_X) x1 = WORLD_CHUNKS_X * CHUNK_SIZE_X;
  if (y1 > CHUNK_SIZE_Y) y1 = CHUNK_SIZE_Y;
  if (z1 > WORLD_CHUNKS_Z * CHUNK_SIZE_Z) z1 = WORLD_CHUNKS_Z * CHUNK_SIZE_Z;

  for (int x = x0; x < x1; x++) {
    for (int y = y0; y < y1; y++) {
      for (int z = z0; z < z1; z++) {
        uint8_t id = getBlock(x, y, z);
        if (id > 0 && g_blockProps[id].isSolid()) {
          // Create bounding box
          boxes.push_back(AABB((double)x, (double)y, (double)z,
                               (double)(x + 1), (double)(y + 1), (double)(z + 1)));
        }
      }
    }
  }
  return boxes;
}

uint8_t Level::getSkyLight(int wx, int wy, int wz) const {
  int cx = wx >> 4;
  int cz = wz >> 4;
  if (cx < 0 || cx >= WORLD_CHUNKS_X || cz < 0 || cz >= WORLD_CHUNKS_Z || wy < 0 || wy >= CHUNK_SIZE_Y) return 15;
  return m_chunks[cx][cz]->getSkyLight(wx & 0xF, wy, wz & 0xF);
}

uint8_t Level::getBlockLight(int wx, int wy, int wz) const {
  int cx = wx >> 4;
  int cz = wz >> 4;
  if (cx < 0 || cx >= WORLD_CHUNKS_X || cz < 0 || cz >= WORLD_CHUNKS_Z || wy < 0 || wy >= CHUNK_SIZE_Y) return 0;
  return m_chunks[cx][cz]->getBlockLight(wx & 0xF, wy, wz & 0xF);
}

void Level::setSkyLight(int wx, int wy, int wz, uint8_t val) {
  int cx = wx >> 4;
  int cz = wz >> 4;
  if (cx < 0 || cx >= WORLD_CHUNKS_X || cz < 0 || cz >= WORLD_CHUNKS_Z || wy < 0 || wy >= CHUNK_SIZE_Y) return;
  uint8_t curBlock = m_chunks[cx][cz]->getBlockLight(wx & 0xF, wy, wz & 0xF);
  m_chunks[cx][cz]->setLight(wx & 0xF, wy, wz & 0xF, val, curBlock);
}

void Level::setBlockLight(int wx, int wy, int wz, uint8_t val) {
  int cx = wx >> 4;
  int cz = wz >> 4;
  if (cx < 0 || cx >= WORLD_CHUNKS_X || cz < 0 || cz >= WORLD_CHUNKS_Z || wy < 0 || wy >= CHUNK_SIZE_Y) return;
  uint8_t curSky = m_chunks[cx][cz]->getSkyLight(wx & 0xF, wy, wz & 0xF);
  m_chunks[cx][cz]->setLight(wx & 0xF, wy, wz & 0xF, curSky, val);
}

bool Level::saveToFile(const char *path) const {
  if (!path) return false;
  FILE *f = fopen(path, "wb");
  if (!f) return false;

  struct SaveHeader {
    char magic[8];
    uint32_t version;
    uint32_t chunksX;
    uint32_t chunksZ;
    uint32_t chunkY;
    int64_t time;
    uint32_t waterSize;
  } hdr;

  memcpy(hdr.magic, "MCPSPWLD", 8);
  hdr.version = 3;
  hdr.chunksX = WORLD_CHUNKS_X;
  hdr.chunksZ = WORLD_CHUNKS_Z;
  hdr.chunkY = CHUNK_SIZE_Y;
  hdr.time = m_time;
  hdr.waterSize = (uint32_t)m_waterDepth.size();
  if (fwrite(&hdr, sizeof(hdr), 1, f) != 1) { fclose(f); return false; }

  for (int cx = 0; cx < WORLD_CHUNKS_X; ++cx) {
    for (int cz = 0; cz < WORLD_CHUNKS_Z; ++cz) {
      const Chunk *ch = m_chunks[cx][cz];
      if (fwrite(ch->blocks, sizeof(ch->blocks), 1, f) != 1) { fclose(f); return false; }
      if (fwrite(ch->light, sizeof(ch->light), 1, f) != 1) { fclose(f); return false; }
    }
  }

  if (!m_waterDepth.empty()) {
    if (fwrite(m_waterDepth.data(), 1, m_waterDepth.size(), f) != m_waterDepth.size()) {
      fclose(f);
      return false;
    }
  }
  if (!m_lavaDepth.empty()) {
    if (fwrite(m_lavaDepth.data(), 1, m_lavaDepth.size(), f) != m_lavaDepth.size()) {
      fclose(f);
      return false;
    }
  }

  fflush(f);
  fclose(f);
  return true;
}

bool Level::loadFromFile(const char *path) {
  if (!path) return false;
  FILE *f = fopen(path, "rb");
  if (!f) return false;

  struct SaveHeader {
    char magic[8];
    uint32_t version;
    uint32_t chunksX;
    uint32_t chunksZ;
    uint32_t chunkY;
    int64_t time;
    uint32_t waterSize;
  } hdr;

  if (fread(&hdr, sizeof(hdr), 1, f) != 1) { fclose(f); return false; }
  if (memcmp(hdr.magic, "MCPSPWLD", 8) != 0 || (hdr.version != 1 && hdr.version != 2 && hdr.version != 3)) { fclose(f); return false; }
  if (hdr.chunksX != WORLD_CHUNKS_X || hdr.chunksZ != WORLD_CHUNKS_Z || hdr.chunkY != CHUNK_SIZE_Y) { fclose(f); return false; }
  if (hdr.waterSize != m_waterDepth.size()) { fclose(f); return false; }

  for (int cx = 0; cx < WORLD_CHUNKS_X; ++cx) {
    for (int cz = 0; cz < WORLD_CHUNKS_Z; ++cz) {
      Chunk *ch = m_chunks[cx][cz];
      ch->cx = cx;
      ch->cz = cz;
      if (fread(ch->blocks, sizeof(ch->blocks), 1, f) != 1) { fclose(f); return false; }
      if (fread(ch->light, sizeof(ch->light), 1, f) != 1) { fclose(f); return false; }
      for (int sy = 0; sy < SUBCHUNK_COUNT; ++sy) ch->dirty[sy] = true;
    }
  }

  if (!m_waterDepth.empty()) {
    if (fread(m_waterDepth.data(), 1, m_waterDepth.size(), f) != m_waterDepth.size()) {
      fclose(f);
      return false;
    }
  }
  if (hdr.version >= 3 && !m_lavaDepth.empty()) {
    if (fread(m_lavaDepth.data(), 1, m_lavaDepth.size(), f) != m_lavaDepth.size()) {
      fclose(f);
      return false;
    }
  }

  fclose(f);
  m_time = hdr.time;
  while (!m_waterTicks.empty()) m_waterTicks.pop();
  while (!m_lavaTicks.empty()) m_lavaTicks.pop();
  std::fill(m_waterDue.begin(), m_waterDue.end(), -1);
  std::fill(m_lavaDue.begin(), m_lavaDue.end(), -1);
  for (int y = 0; y < CHUNK_SIZE_Y; ++y) {
    for (int z = 0; z < WORLD_CHUNKS_Z * CHUNK_SIZE_Z; ++z) {
      for (int x = 0; x < WORLD_CHUNKS_X * CHUNK_SIZE_X; ++x) {
        uint8_t id = getBlock(x, y, z);
        if (isWaterBlock(id)) scheduleWaterTick(x, y, z, 1);
        else if (isLavaBlock(id)) {
          if (hdr.version < 3) m_lavaDepth[waterIndex(x, y, z)] = (id == BLOCK_LAVA_STILL) ? 0 : 1;
          scheduleLavaTick(x, y, z, 4);
        } else if (hdr.version < 3) {
          m_lavaDepth[waterIndex(x, y, z)] = 0xFF;
        }
      }
    }
  }
  m_waterDirty = !m_waterTicks.empty();
  m_lavaDirty = !m_lavaTicks.empty();
  return true;
}

void Level::generate(Random *rng) {
  int64_t seed = rng->nextLong();
  m_suspendLightingUpdates = true;
  m_lightUpdateQueue.clear();

  for (int cx = 0; cx < WORLD_CHUNKS_X; cx++) {
    for (int cz = 0; cz < WORLD_CHUNKS_Z; cz++) {
      Chunk *c = m_chunks[cx][cz];
      c->cx = cx;
      c->cz = cz;
      WorldGen::generateChunk(c->blocks, cx, cz, seed);
      for(int i=0; i<SUBCHUNK_COUNT; i++) c->dirty[i] = true;
    }
  }

  for (int cx = 0; cx < WORLD_CHUNKS_X; cx++) {
    for (int cz = 0; cz < WORLD_CHUNKS_Z; cz++) {
      Random chunkRng(seed ^ ((int64_t)cx * 341873128712LL) ^ ((int64_t)cz * 132897987541LL));
      for (int i = 0; i < 3; i++) {
        int lx = chunkRng.nextInt(CHUNK_SIZE_X);
        int lz = chunkRng.nextInt(CHUNK_SIZE_Z);
        int wx = cx * CHUNK_SIZE_X + lx;
        int wz = cz * CHUNK_SIZE_Z + lz;

        int wy = CHUNK_SIZE_Y - 1;
        while (wy > 0 && getBlock(wx, wy, wz) == BLOCK_AIR) wy--;

        if (wy > 50 && getBlock(wx, wy, wz) == BLOCK_GRASS) {
          setBlock(wx, wy, wz, BLOCK_DIRT);
          TreeFeature::place(this, wx, wy + 1, wz, chunkRng);
        }
      }
    }
  }

  for (int y = 0; y < CHUNK_SIZE_Y; ++y) {
    for (int z = 0; z < WORLD_CHUNKS_Z * CHUNK_SIZE_Z; ++z) {
      for (int x = 0; x < WORLD_CHUNKS_X * CHUNK_SIZE_X; ++x) {
        uint8_t id = getBlock(x, y, z);
        if (id == BLOCK_WATER_STILL) setWaterDepth(x, y, z, 0);
        else if (id == BLOCK_WATER_FLOW) setWaterDepth(x, y, z, 1);
        else setWaterDepth(x, y, z, 0xFF);
        if (id == BLOCK_LAVA_STILL) m_lavaDepth[waterIndex(x, y, z)] = 0;
        else if (id == BLOCK_LAVA_FLOW) m_lavaDepth[waterIndex(x, y, z)] = 1;
        else m_lavaDepth[waterIndex(x, y, z)] = 0xFF;
      }
    }
  }
  while (!m_waterTicks.empty()) m_waterTicks.pop();
  while (!m_lavaTicks.empty()) m_lavaTicks.pop();
  std::fill(m_waterDue.begin(), m_waterDue.end(), -1);
  std::fill(m_lavaDue.begin(), m_lavaDue.end(), -1);

  m_suspendLightingUpdates = false;
  m_lightUpdateQueue.clear();
  computeLighting();
}

void Level::queueLightUpdate(int wx, int wy, int wz) {
  if (wx < 0 || wz < 0 || wy < 0 || wy >= CHUNK_SIZE_Y) return;
  int maxX = WORLD_CHUNKS_X * CHUNK_SIZE_X;
  int maxZ = WORLD_CHUNKS_Z * CHUNK_SIZE_Z;
  if (wx >= maxX || wz >= maxZ) return;
  m_lightUpdateQueue.push_back(waterIndex(wx, wy, wz));
}

void Level::computeLighting() {
  std::vector<LightNode> lightQ;
  lightQ.reserve(65536);

  // 1. Sunlight
  for (int x = 0; x < WORLD_CHUNKS_X * CHUNK_SIZE_X; x++) {
    for (int z = 0; z < WORLD_CHUNKS_Z * CHUNK_SIZE_Z; z++) {
      int curLight = 15;
      for (int y = CHUNK_SIZE_Y - 1; y >= 0; y--) {
        uint8_t id = getBlock(x, y, z);
        if (id != BLOCK_AIR) {
           const BlockProps &bp = g_blockProps[id];
           if (bp.isOpaque()) curLight = 0;
           else if (id == BLOCK_LEAVES) curLight = (curLight >= 2) ? curLight - 2 : 0;
           else if (bp.isLiquid()) curLight = (curLight >= 3) ? curLight - 3 : 0;
        }
        setSkyLight(x, y, z, curLight);
      }
    }
  }

  // 2. Queue borders
  for (int x = 0; x < WORLD_CHUNKS_X * CHUNK_SIZE_X; x++) {
    for (int z = 0; z < WORLD_CHUNKS_Z * CHUNK_SIZE_Z; z++) {
      for (int y = CHUNK_SIZE_Y - 1; y >= 0; y--) {
        if (getSkyLight(x, y, z) == 15) {
           bool needsSpread = false;
           const int dx[] = {-1, 1, 0, 0, 0, 0};
           const int dy[] = {0, 0, -1, 1, 0, 0};
           const int dz[] = {0, 0, 0, 0, -1, 1};
           for(int i = 0; i < 6; i++) {
             int nx = x + dx[i], ny = y + dy[i], nz = z + dz[i];
             if (ny >= 0 && ny < CHUNK_SIZE_Y && nx >= 0 && nx < WORLD_CHUNKS_X * CHUNK_SIZE_X && nz >= 0 && nz < WORLD_CHUNKS_Z * CHUNK_SIZE_Z) {
                 if (getSkyLight(nx, ny, nz) < 15 && !g_blockProps[getBlock(nx, ny, nz)].isOpaque()) {
                     needsSpread = true;
                     break;
                 }
             }
           }
           if (needsSpread) lightQ.push_back({x, y, z});
        }
      }
    }
  }

  // 3. Sky light flood fill
  int head = 0;
  while (head < (int)lightQ.size()) {
    LightNode node = lightQ[head++];
    uint8_t level = getSkyLight(node.x, node.y, node.z);
    if (level <= 1) continue;

    const int dx[] = {-1, 1, 0, 0, 0, 0};
    const int dy[] = {0, 0, -1, 1, 0, 0};
    const int dz[] = {0, 0, 0, 0, -1, 1};

    for (int i = 0; i < 6; i++) {
      int nx = node.x + dx[i];
      int ny = node.y + dy[i];
      int nz = node.z + dz[i];

      if (ny < 0 || ny >= CHUNK_SIZE_Y || nx < 0 || nz < 0 || nx >= WORLD_CHUNKS_X * CHUNK_SIZE_X || nz >= WORLD_CHUNKS_Z * CHUNK_SIZE_Z) continue;
      uint8_t neighborId = getBlock(nx, ny, nz);
      if (g_blockProps[neighborId].isOpaque()) continue;

      int attenuation = getLightAttenuation(neighborId);

      int neighborLevel = getSkyLight(nx, ny, nz);
      if (level - attenuation > neighborLevel) {
        setSkyLight(nx, ny, nz, level - attenuation);
        lightQ.push_back({nx, ny, nz});
      }
    }
  }

  // 4. Block light from all emissive blocks (recomputed from scratch).
  recomputeBlockLightingFromSources();
}

void Level::updateLight(int wx, int wy, int wz) {
  uint8_t id = getBlock(wx, wy, wz);
  uint8_t oldBlockLight = getBlockLight(wx, wy, wz);
  uint8_t newBlockLight = g_blockProps[id].light_emit;
  
  const int dx[] = {-1, 1, 0, 0, 0, 0};
  const int dy[] = {0, 0, -1, 1, 0, 0};
  const int dz[] = {0, 0, 0, 0, -1, 1};
  
  uint8_t maxNeighborLight = 0;
  for(int i=0; i<6; i++) {
    int nx = wx + dx[i];
    int ny = wy + dy[i];
    int nz = wz + dz[i];
    if (ny < 0 || ny >= CHUNK_SIZE_Y || nx < 0 || nz < 0 || nx >= WORLD_CHUNKS_X * CHUNK_SIZE_X || nz >= WORLD_CHUNKS_Z * CHUNK_SIZE_Z) continue;
    uint8_t nl = getBlockLight(nx, ny, nz);
    if(nl > maxNeighborLight) maxNeighborLight = nl;
  }
  
  uint8_t blockAtten = g_blockProps[id].isOpaque() ? 15 : ((id == BLOCK_LEAVES) ? 2 : (g_blockProps[id].isLiquid() ? 3 : 1));
  uint8_t expectedBlockLight = newBlockLight;
  if (maxNeighborLight > blockAtten && (maxNeighborLight - blockAtten) > expectedBlockLight) {
      expectedBlockLight = maxNeighborLight - blockAtten;
  }
  updateBlockLight(wx, wy, wz, oldBlockLight, expectedBlockLight);

  uint8_t oldSkyLight = getSkyLight(wx, wy, wz);
  uint8_t expectedSkyLight = 0;
  if (wy == CHUNK_SIZE_Y - 1) {
      expectedSkyLight = blockAtten < 15 ? 15 : 0;
  } else if (getSkyLight(wx, wy + 1, wz) == 15 && blockAtten < 15) {
      expectedSkyLight = 15;
  } else {
      uint8_t maxNeighborSkyLight = 0;
      for(int i=0; i<6; i++) {
        int nx = wx + dx[i];
        int ny = wy + dy[i];
        int nz = wz + dz[i];
        if (ny < 0 || ny >= CHUNK_SIZE_Y || nx < 0 || nz < 0 || nx >= WORLD_CHUNKS_X * CHUNK_SIZE_X || nz >= WORLD_CHUNKS_Z * CHUNK_SIZE_Z) continue;
        uint8_t nl = getSkyLight(nx, ny, nz);
        if(nl > maxNeighborSkyLight) maxNeighborSkyLight = nl;
      }
      if (maxNeighborSkyLight > blockAtten) expectedSkyLight = maxNeighborSkyLight - blockAtten;
  }
  updateSkyLight(wx, wy, wz, oldSkyLight, expectedSkyLight);
}

int Level::getLightAttenuation(uint8_t blockId) const {
  if (g_blockProps[blockId].isOpaque()) return 15;
  if (blockId == BLOCK_LEAVES) return 2;
  if (g_blockProps[blockId].isLiquid()) return 3;
  return 1;
}

void Level::recomputeBlockLightingFromSources() {
  std::vector<LightNode> lightQ;
  lightQ.reserve(65536);

  // Reset block-light channel and seed every emissive block from BlockProps.
  for (int cx = 0; cx < WORLD_CHUNKS_X; cx++) {
    for (int cz = 0; cz < WORLD_CHUNKS_Z; cz++) {
      for (int lx = 0; lx < CHUNK_SIZE_X; lx++) {
        for (int lz = 0; lz < CHUNK_SIZE_Z; lz++) {
          for (int ly = 0; ly < CHUNK_SIZE_Y; ly++) {
            int wx = cx * CHUNK_SIZE_X + lx;
            int wz = cz * CHUNK_SIZE_Z + lz;
            uint8_t id = m_chunks[cx][cz]->blocks[lx][lz][ly];
            uint8_t emit = g_blockProps[id].light_emit;
            setBlockLight(wx, ly, wz, emit);
            if (emit > 1) lightQ.push_back({wx, ly, wz});
          }
        }
      }
    }
  }

  int head = 0;
  static const int dx[] = {-1, 1, 0, 0, 0, 0};
  static const int dy[] = {0, 0, -1, 1, 0, 0};
  static const int dz[] = {0, 0, 0, 0, -1, 1};
  const int maxX = WORLD_CHUNKS_X * CHUNK_SIZE_X;
  const int maxZ = WORLD_CHUNKS_Z * CHUNK_SIZE_Z;

  while (head < (int)lightQ.size()) {
    LightNode node = lightQ[head++];
    uint8_t level = getBlockLight(node.x, node.y, node.z);
    if (level <= 1) continue;

    for (int i = 0; i < 6; i++) {
      int nx = node.x + dx[i];
      int ny = node.y + dy[i];
      int nz = node.z + dz[i];
      if (ny < 0 || ny >= CHUNK_SIZE_Y || nx < 0 || nz < 0 || nx >= maxX || nz >= maxZ) continue;

      uint8_t neighborId = getBlock(nx, ny, nz);
      if (g_blockProps[neighborId].isOpaque()) continue;

      int attenuation = getLightAttenuation(neighborId);
      int propagated = (int)level - attenuation;
      if (propagated <= 0) continue;

      uint8_t neighborLevel = getBlockLight(nx, ny, nz);
      if (propagated > neighborLevel) {
        setBlockLight(nx, ny, nz, (uint8_t)propagated);
        lightQ.push_back({nx, ny, nz});
      }
    }
  }
}

void Level::updateBlockLight(int wx, int wy, int wz, uint8_t oldLight, uint8_t newLight) {
    (void)oldLight;
    const int maxX = WORLD_CHUNKS_X * CHUNK_SIZE_X;
    const int maxZ = WORLD_CHUNKS_Z * CHUNK_SIZE_Z;
    static const int dx[] = {-1, 1, 0, 0, 0, 0};
    static const int dy[] = {0, 0, -1, 1, 0, 0};
    static const int dz[] = {0, 0, 0, 0, -1, 1};

    std::deque<LightNode> q;
    const int maxQueued = 200000;

    setBlockLight(wx, wy, wz, newLight);
    q.push_back({wx, wy, wz});
    for (int i = 0; i < 6; ++i) {
      int nx = wx + dx[i], ny = wy + dy[i], nz = wz + dz[i];
      if (ny < 0 || ny >= CHUNK_SIZE_Y || nx < 0 || nz < 0 || nx >= maxX || nz >= maxZ) continue;
      q.push_back({nx, ny, nz});
    }

    while (!q.empty()) {
      LightNode n = q.front();
      q.pop_front();
      uint8_t bid = getBlock(n.x, n.y, n.z);
      const BlockProps &bp = g_blockProps[bid];
      uint8_t cur = getBlockLight(n.x, n.y, n.z);
      uint8_t target = bp.light_emit;

      if (!bp.isOpaque()) {
        int att = getLightAttenuation(bid);
        for (int i = 0; i < 6; ++i) {
          int nx = n.x + dx[i], ny = n.y + dy[i], nz = n.z + dz[i];
          if (ny < 0 || ny >= CHUNK_SIZE_Y || nx < 0 || nz < 0 || nx >= maxX || nz >= maxZ) continue;
          uint8_t nl = getBlockLight(nx, ny, nz);
          if (nl > (uint8_t)att) {
            uint8_t cand = (uint8_t)(nl - att);
            if (cand > target) target = cand;
          }
        }
      }

      if (target == cur) continue;
      setBlockLight(n.x, n.y, n.z, target);
      for (int i = 0; i < 6; ++i) {
        int nx = n.x + dx[i], ny = n.y + dy[i], nz = n.z + dz[i];
        if (ny < 0 || ny >= CHUNK_SIZE_Y || nx < 0 || nz < 0 || nx >= maxX || nz >= maxZ) continue;
        q.push_back({nx, ny, nz});
      }

      // Safety valve for low-memory PSP: fall back to authoritative rebuild
      // instead of growing an unbounded local queue.
      if ((int)q.size() > maxQueued) {
        recomputeBlockLightingFromSources();
        return;
      }
    }
}

struct LightRemovalNode {
    short x, y, z;
    uint8_t val;
};

void Level::updateSkyLight(int wx, int wy, int wz, uint8_t oldLight, uint8_t newLight) {
    if (oldLight == newLight) return;

    std::deque<LightRemovalNode> darkQ;
    std::deque<LightNode> lightQ;

    const int dx[] = {-1, 1, 0, 0, 0, 0};
    const int dy[] = {0, 0, -1, 1, 0, 0};
    const int dz[] = {0, 0, 0, 0, -1, 1};

    if (oldLight > newLight) {
        darkQ.push_back({(short)wx, (short)wy, (short)wz, oldLight});
        setSkyLight(wx, wy, wz, 0);
    } else {
        lightQ.push_back({wx, wy, wz});
        setSkyLight(wx, wy, wz, newLight);
    }

    while (!darkQ.empty()) {
        LightRemovalNode node = darkQ.front();
        darkQ.pop_front();
        int x = node.x, y = node.y, z = node.z;
        uint8_t level = node.val;

        for (int i = 0; i < 6; i++) {
            int nx = x + dx[i], ny = y + dy[i], nz = z + dz[i];
            if (ny < 0 || ny >= CHUNK_SIZE_Y || nx < 0 || nz < 0 || nx >= WORLD_CHUNKS_X * CHUNK_SIZE_X || nz >= WORLD_CHUNKS_Z * CHUNK_SIZE_Z) continue;
            
            uint8_t neighborLevel = getSkyLight(nx, ny, nz);
            
            if (neighborLevel != 0 && ((dy[i] == -1 && level == 15 && neighborLevel == 15) || neighborLevel < level)) {
                setSkyLight(nx, ny, nz, 0);
                darkQ.push_back({(short)nx, (short)ny, (short)nz, neighborLevel});
            } else if (neighborLevel >= level) {
                lightQ.push_back({nx, ny, nz});
            }
        }
    }

    while (!lightQ.empty()) {
        LightNode node = lightQ.front();
        lightQ.pop_front();
        int x = node.x, y = node.y, z = node.z;
        uint8_t level = getSkyLight(x, y, z);
        
        for (int i = 0; i < 6; i++) {
            int nx = x + dx[i], ny = y + dy[i], nz = z + dz[i];
            if (ny < 0 || ny >= CHUNK_SIZE_Y || nx < 0 || nz < 0 || nx >= WORLD_CHUNKS_X * CHUNK_SIZE_X || nz >= WORLD_CHUNKS_Z * CHUNK_SIZE_Z) continue;
            
            uint8_t id = getBlock(nx, ny, nz);
            const BlockProps& bp = g_blockProps[id];
            
            int attenuation = bp.isOpaque() ? 15 : ((id == BLOCK_LEAVES) ? 2 : (bp.isLiquid() ? 3 : 1));
            if (dy[i] == -1 && level == 15 && attenuation < 15) attenuation = 0;
            
            int neighborLevel = getSkyLight(nx, ny, nz);
            if (level - attenuation > neighborLevel) {
                setSkyLight(nx, ny, nz, level - attenuation);
                lightQ.push_back({nx, ny, nz});
            }
        }
    }
}
