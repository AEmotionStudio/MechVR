// Terrain heights and rocks.
#ifndef WORLD_H
#define WORLD_H
#include "mathx.h"
#define HN 512
#define CELL 4.0f
#define NROCK 44
typedef struct { v3 pos; float yaw, radius; int shape; } Rock;
extern float world_h[HN * HN];
extern Rock rocks[NROCK];
void world_init(void);
float world_height(float x, float z);
float frand(void);            // 0..1
float frands(void);           // -1..1
#endif
