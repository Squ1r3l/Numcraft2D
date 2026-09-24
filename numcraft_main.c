#include <eadk.h>
#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <stdlib.h>
#include <math.h>
#include "storage.h"

const char eadk_app_name[] __attribute__((section(".rodata.eadk_app_name"))) = "NumCraft";
const uint32_t eadk_api_level __attribute__((section(".rodata.eadk_api_level"))) = 0;

/* ================================================================== */
/* Constants                                                           */
/* ================================================================== */

#define WORLD_HEIGHT   25
#define BLOCK_PX       8
#define VISIBLE_COLS   40      /* 40*8 = 320 = screen width  */
#define HUD_TOP        20
#define HUD_BOTTOM     20

#define MOD_TABLE_SIZE 4096    /* power of two */

enum {
  B_AIR = 0, B_GRASS, B_SAND, B_SNOW, B_DIRT, B_CLAY, B_STONE, B_BEDROCK,
  B_WOOD, B_LEAVES, B_CACTUS, B_WATER, B_ICE,
  B_COAL, B_IRON, B_GOLD, B_DIAMOND, B_EMERALD, B_RUBY,
  B_TORCH, B_BRICK, B_GLASS,
  I_PLANK, I_STICK, I_PICK_WOOD, I_PICK_STONE, I_PICK_IRON, I_PICK_DIAMOND,
  I_APPLE,
  NUM_TYPES
};

/* One compact table drives naming, rendering, physics and mining rules for
   every block/item — adding new content is a single line here. */
typedef struct {
  const char * name;
  uint16_t color;    /* RGB565 (blocks only; unused for pure items) */
  uint8_t solid;     /* blocks player movement ? */
  uint8_t placeable; /* can the player place it from inventory ? */
  uint8_t tier;      /* min pickaxe tier required to mine (0 = hand) */
} BlockInfo;

static const BlockInfo BLOCK_INFO[NUM_TYPES] = {
  [B_AIR]          = { "Air",           0xCEBF, 0, 0, 0 },
  [B_GRASS]        = { "Herbe",         0x2666, 1, 1, 0 },
  [B_SAND]         = { "Sable",         0xEE55, 1, 1, 0 },
  [B_SNOW]         = { "Neige",         0xF7DF, 1, 1, 0 },
  [B_DIRT]         = { "Terre",         0x8B22, 1, 1, 0 },
  [B_CLAY]         = { "Argile",        0xACB0, 1, 1, 0 },
  [B_STONE]        = { "Pierre",        0x8410, 1, 1, 1 },
  [B_BEDROCK]      = { "Bedrock",       0x18E3, 1, 0, 255 },
  [B_WOOD]         = { "Bois",          0x6B2C, 1, 1, 0 },
  [B_LEAVES]       = { "Feuilles",      0x3E86, 0, 1, 0 },
  [B_CACTUS]       = { "Cactus",        0x23C4, 1, 1, 0 },
  [B_WATER]        = { "Eau",           0x337A, 0, 0, 255 },
  [B_ICE]          = { "Glace",         0xBF5E, 1, 1, 0 },
  [B_COAL]         = { "Charbon",       0x2104, 1, 1, 1 },
  [B_IRON]         = { "Fer",           0xC67A, 1, 1, 2 },
  [B_GOLD]         = { "Or",            0xFEA0, 1, 1, 3 },
  [B_DIAMOND]      = { "Diamant",       0x2FFF, 1, 1, 3 },
  [B_EMERALD]      = { "Emeraude",      0x2E8B, 1, 1, 4 },
  [B_RUBY]         = { "Rubis",         0xC0A7, 1, 1, 4 },
  [B_TORCH]        = { "Torche",        0xFC40, 0, 1, 0 },
  [B_BRICK]        = { "Brique",        0xAA47, 1, 1, 0 },
  [B_GLASS]        = { "Verre",         0xD75E, 1, 1, 0 },
  [I_PLANK]        = { "Planche",       0x8B22, 1, 1, 0 },
  [I_STICK]        = { "Baton",         0,      0, 0, 0 },
  [I_PICK_WOOD]    = { "Pioche bois",   0,      0, 0, 0 },
  [I_PICK_STONE]   = { "Pioche pierre", 0,      0, 0, 0 },
  [I_PICK_IRON]    = { "Pioche fer",    0,      0, 0, 0 },
  [I_PICK_DIAMOND] = { "Pioche diamant",0,      0, 0, 0 },
  [I_APPLE]        = { "Pomme",         0,      0, 0, 0 },
};

static bool is_solid(uint8_t b) { return BLOCK_INFO[b].solid; }
static bool is_placeable(uint8_t b) { return BLOCK_INFO[b].placeable; }
static int required_tier(uint8_t b) { return BLOCK_INFO[b].tier; }

typedef struct {
  uint8_t need_type;
  uint8_t need_count;
  uint8_t need_type2;   /* 255 if unused */
  uint8_t need_count2;
  uint8_t out_type;
  uint8_t out_count;
} Recipe;

static const Recipe RECIPES[] = {
  { B_WOOD, 1, 255, 0, I_PLANK, 4 },
  { I_PLANK, 2, 255, 0, I_STICK, 4 },
  { I_PLANK, 3, I_STICK, 2, I_PICK_WOOD, 1 },
  { B_STONE, 3, I_STICK, 2, I_PICK_STONE, 1 },
  { B_IRON, 3, I_STICK, 2, I_PICK_IRON, 1 },
  { B_DIAMOND, 3, I_STICK, 2, I_PICK_DIAMOND, 1 },
  { B_COAL, 1, I_STICK, 1, B_TORCH, 4 },
  { B_CLAY, 4, 255, 0, B_BRICK, 1 },
  { B_SAND, 2, 255, 0, B_GLASS, 1 },
};
#define NUM_RECIPES (sizeof(RECIPES) / sizeof(RECIPES[0]))

/* ================================================================== */
/* Deterministic world generation                                      */
/* ================================================================== */

static uint32_t g_seed;

static uint32_t hash2d(uint32_t seed, int32_t x, int32_t y) {
  uint32_t h = (uint32_t)x * 374761393u;
  h += (uint32_t)y * 668265263u;
  h += seed * 2246822519u;
  h = (h ^ (h >> 13)) * 1274126177u;
  h ^= (h >> 16);
  return h;
}

static float frac01(uint32_t h) {
  return (h & 0xFFFFFFu) / (float)0x1000000u;
}

static float smooth_noise(uint32_t seed, float x) {
  int xi = (int)floorf(x);
  float xf = x - (float)xi;
  float v0 = frac01(hash2d(seed, xi, 0));
  float v1 = frac01(hash2d(seed, xi + 1, 0));
  float t = xf * xf * (3.0f - 2.0f * xf);
  return v0 + (v1 - v0) * t;
}

/* 2D coherent value noise, used for winding cave tunnels (not single-cell
   dice rolls, which just gave scattered "salt and pepper" holes). */
static float smooth_noise2d(uint32_t seed, float x, float y) {
  int xi = (int)floorf(x);
  int yi = (int)floorf(y);
  float xf = x - (float)xi;
  float yf = y - (float)yi;
  float v00 = frac01(hash2d(seed, xi, yi));
  float v10 = frac01(hash2d(seed, xi + 1, yi));
  float v01 = frac01(hash2d(seed, xi, yi + 1));
  float v11 = frac01(hash2d(seed, xi + 1, yi + 1));
  float tx = xf * xf * (3.0f - 2.0f * xf);
  float ty = yf * yf * (3.0f - 2.0f * yf);
  float a = v00 + (v10 - v00) * tx;
  float b = v01 + (v11 - v01) * tx;
  return a + (b - a) * ty;
}

static int surface_row(int32_t x) {
  float n = smooth_noise(g_seed, x * 0.15f);
  int row = 9 + (int)((n - 0.5f) * 7.0f);
  if (row < 3) row = 3;
  if (row > WORLD_HEIGHT - 8) row = WORLD_HEIGHT - 8;
  return row;
}

#define SEA_LEVEL 11

/* Large-scale noise picks a biome per region: 1=desert, 0=plains, 2=snowy */
static int biome_at(int32_t x) {
  float n = smooth_noise(g_seed + 555u, x * 0.02f);
  if (n < 0.33f) return 1;
  if (n < 0.66f) return 0;
  return 2;
}

static bool cactus_column(int32_t x, int * height) {
  float t = frac01(hash2d(g_seed + 777u, x, 0));
  if (t < 0.06f) {
    *height = 1 + (int)(t * 200.0f) % 3;
    return true;
  }
  return false;
}

/* Is column x a "tree" column ? If so: trunk height and species
   (0 = round oak, 1 = tall pine). */
static bool tree_column(int32_t x, int * trunk_h, int * variant) {
  float t = frac01(hash2d(g_seed + 999u, x, 0));
  if (t < 0.09f) {
    if (t < 0.025f) {
      *variant = 1; /* pine: rarer, taller */
      *trunk_h = 5 + (int)(t * 400.0f) % 2;
    } else {
      *variant = 0; /* oak: common, round canopy */
      *trunk_h = 2 + (int)(t * 400.0f) % 2;
    }
    return true;
  }
  return false;
}

/* Does world cell (x,y) fall inside the leafy canopy of a nearby tree ?
   Trees are generated column-by-column, but their canopy spreads into
   neighbouring columns, so we check every candidate trunk within reach. */
static bool tree_leaf_at(int32_t x, int32_t y) {
  for (int32_t tx = x - 2; tx <= x + 2; tx++) {
    int trunk_h, variant;
    if (!tree_column(tx, &trunk_h, &variant)) continue;
    int surf_tx = surface_row(tx);
    int trunk_top = surf_tx - trunk_h; /* row of the topmost log */
    int dx = (int)(x - tx);

    if (variant == 0) {
      /* Oak: small round canopy, 3 blocks wide */
      if (dx == 0 && y == trunk_top - 2) return true;               /* apex */
      if (dx >= -1 && dx <= 1 && y == trunk_top - 1) return true;   /* middle ring */
      if (dx != 0 && dx >= -1 && dx <= 1 && y == trunk_top) return true; /* shoulders */
    } else {
      /* Pine: tall conical canopy, widest at the base */
      if (dx == 0 && y == trunk_top - 3) return true;               /* apex */
      if (dx >= -1 && dx <= 1 && y == trunk_top - 2) return true;   /* ring 1 */
      if (dx >= -1 && dx <= 1 && y == trunk_top - 1) return true;   /* ring 2 */
      if (dx != 0 && dx >= -2 && dx <= 2 && y == trunk_top) return true; /* skirt */
    }
  }
  return false;
}

static uint8_t natural_block(int32_t x, int32_t y) {
  int surf = surface_row(x);
  int biome = biome_at(x);
  bool flooded = surf > SEA_LEVEL;

  if (y < surf) {
    /* Low-lying ground is flooded up to sea level (lakes) */
    if (flooded && y >= SEA_LEVEL) {
      if (biome == 2 && y == SEA_LEVEL) return B_ICE; /* frozen lake top, snowy biome */
      return B_WATER;
    }
    if (biome == 1) {
      /* Desert: no trees, occasional cactus instead */
      int ch;
      if (cactus_column(x, &ch) && y >= surf - ch && y < surf) return B_CACTUS;
      return B_AIR;
    }
    int trunk_h, variant;
    if (tree_column(x, &trunk_h, &variant) && y >= surf - trunk_h && y < surf) return B_WOOD;
    if (tree_leaf_at(x, y)) return B_LEAVES;
    return B_AIR;
  }
  if (y == WORLD_HEIGHT - 1) return B_BEDROCK;
  if (y == surf) {
    if (flooded) return B_CLAY;     /* lake bed */
    if (biome == 1) return B_SAND;
    if (biome == 2) return B_SNOW;
    return B_GRASS;
  }
  if (y < surf + 4) return (biome == 1) ? B_SAND : B_DIRT;

  /* Underground: caves, then ores (rarest first), then stone */
  float cave = smooth_noise2d(g_seed + 31u, x * 0.09f, y * 0.13f);
  if (fabsf(cave - 0.5f) < 0.035f && y < WORLD_HEIGHT - 2) return B_AIR;

  float ore = frac01(hash2d(g_seed + 71u, x, y));
  int depth = y - surf;
  if (depth > 20 && ore < 0.008f) return B_RUBY;
  if (depth > 18 && ore < 0.018f) return B_EMERALD;
  if (depth > 15 && ore < 0.035f) return B_DIAMOND;
  if (depth > 10 && ore < 0.06f) return B_GOLD;
  if (depth > 5 && ore < 0.10f) return B_IRON;
  if (ore < 0.16f) return B_COAL;
  return B_STONE;
}

/* ---- Sparse modification table (player-made changes) --------------- */

typedef struct {
  int32_t x;
  int16_t y;
  uint8_t block;
  uint8_t used;
} ModEntry;

static ModEntry mod_table[MOD_TABLE_SIZE];

static uint32_t mod_hash(int32_t x, int16_t y) {
  uint32_t h = (uint32_t)x * 2654435761u;
  h ^= (uint32_t)(uint16_t)y * 2246822519u;
  h ^= h >> 15;
  h *= 2654435761u;
  h ^= h >> 13;
  return h & (MOD_TABLE_SIZE - 1);
}

static void set_mod(int32_t x, int16_t y, uint8_t block) {
  uint32_t h = mod_hash(x, y);
  for (int i = 0; i < MOD_TABLE_SIZE; i++) {
    uint32_t idx = (h + (uint32_t)i) & (MOD_TABLE_SIZE - 1);
    if (!mod_table[idx].used || (mod_table[idx].x == x && mod_table[idx].y == y)) {
      mod_table[idx].x = x;
      mod_table[idx].y = y;
      mod_table[idx].block = block;
      mod_table[idx].used = 1;
      return;
    }
  }
  /* table full: silently drop, extremely unlikely in a normal session */
}

static bool get_mod(int32_t x, int16_t y, uint8_t * out) {
  uint32_t h = mod_hash(x, y);
  for (int i = 0; i < MOD_TABLE_SIZE; i++) {
    uint32_t idx = (h + (uint32_t)i) & (MOD_TABLE_SIZE - 1);
    if (!mod_table[idx].used) return false;
    if (mod_table[idx].x == x && mod_table[idx].y == y) {
      *out = mod_table[idx].block;
      return true;
    }
  }
  return false;
}

static uint8_t get_block(int32_t x, int32_t y) {
  if (y < 0 || y >= WORLD_HEIGHT) return B_AIR;
  uint8_t b;
  if (get_mod(x, (int16_t)y, &b)) return b;
  return natural_block(x, y);
}

/* ================================================================== */
/* Persistent save / load (unofficial NumWorks Extapp Storage library) */
/* ================================================================== */

#define SAVE_NAME    "ncraft.dat"
#define SAVE_MAGIC   0x4E435233u /* "NCR3" */

typedef struct __attribute__((packed)) {
  uint32_t magic;
  uint32_t seed;
  int32_t  player_x;
  int32_t  player_y;
  int32_t  facing;
  uint32_t selected_block;
  uint32_t inventory[NUM_TYPES];
  int32_t  health;
  int32_t  hunger;
  uint32_t mod_count;
} SaveHeader;

typedef struct __attribute__((packed)) {
  int32_t x;
  int16_t y;
  uint8_t block;
} SaveMod;

static uint8_t g_save_buf[sizeof(SaveHeader) + MOD_TABLE_SIZE * sizeof(SaveMod)];



/* ================================================================== */
/* Player & inventory                                                   */
/* ================================================================== */

static int32_t player_x;
static int32_t player_y;
static int facing = 1; /* +1 right, -1 left */
static uint32_t inventory[NUM_TYPES];
static uint8_t selected_block = B_DIRT;
static char message[64];
static int message_timer = 0;

#define MAX_HEALTH 10
#define MAX_HUNGER 20
static int32_t health = MAX_HEALTH;
static int32_t hunger = MAX_HUNGER;

static int pickaxe_tier(void) {
  int tier = 0;
  if (inventory[I_PICK_WOOD] > 0) tier = 1;
  if (inventory[I_PICK_STONE] > 0) tier = 2;
  if (inventory[I_PICK_IRON] > 0) tier = 3;
  if (inventory[I_PICK_DIAMOND] > 0) tier = 4;
  return tier;
}

static void set_message(const char * fmt_msg) {
  snprintf(message, sizeof(message), "%s", fmt_msg);
  message_timer = 60;
}

static void mine(int32_t x, int32_t y) {
  uint8_t b = get_block(x, y);
  if (b == B_AIR) return;
  if (b == B_BEDROCK) { set_message("Incassable !"); return; }
  if (b == B_WATER) { set_message("C'est de l'eau !"); return; }
  int need = required_tier(b);
  if (need > pickaxe_tier()) {
    set_message("Outil trop faible !");
    return;
  }
  set_mod(x, (int16_t)y, B_AIR);
  uint8_t gained = b;
  if (b == B_LEAVES && (eadk_random() % 100) < 15) {
    gained = I_APPLE;
  }
  if (inventory[gained] < 0xFFFFFFFFu) inventory[gained]++;
  char buf[48];
  snprintf(buf, sizeof(buf), "+1 %s", BLOCK_INFO[gained].name);
  set_message(buf);
}

static void respawn_player(void) {
  health = MAX_HEALTH;
  hunger = MAX_HUNGER;
  player_x = 0;
  player_y = surface_row(0) - 1;
  if (player_y < 0) player_y = 0;
  set_message("Tu es mort... retour au depart !");
}

static void place(int32_t x, int32_t y) {
  if (get_block(x, y) != B_AIR) { set_message("Occupe !"); return; }
  if (!is_placeable(selected_block) || inventory[selected_block] == 0) {
    set_message("Rien a poser !");
    return;
  }
  inventory[selected_block]--;
  set_mod(x, (int16_t)y, selected_block);
}

static void cycle_selected(int dir) {
  for (int i = 0; i < NUM_TYPES; i++) {
    selected_block = (uint8_t)(((int)selected_block + dir + NUM_TYPES) % NUM_TYPES);
    if (is_placeable(selected_block) && inventory[selected_block] > 0) return;
  }
  /* nothing owned: keep as-is */
}

static bool try_craft(const Recipe * r) {
  if (inventory[r->need_type] < r->need_count) return false;
  if (r->need_type2 != 255 && inventory[r->need_type2] < r->need_count2) return false;
  inventory[r->need_type] -= r->need_count;
  if (r->need_type2 != 255) inventory[r->need_type2] -= r->need_count2;
  inventory[r->out_type] += r->out_count;
  return true;
}

static bool save_game(void) {
  SaveHeader h;
  h.magic = SAVE_MAGIC;
  h.seed = g_seed;
  h.player_x = player_x;
  h.player_y = player_y;
  h.facing = facing;
  h.selected_block = selected_block;
  for (int i = 0; i < NUM_TYPES; i++) h.inventory[i] = inventory[i];
  h.health = health;
  h.hunger = hunger;

  uint32_t count = 0;
  size_t offset = sizeof(SaveHeader);
  for (int i = 0; i < MOD_TABLE_SIZE; i++) {
    if (!mod_table[i].used) continue;
    if (offset + sizeof(SaveMod) > sizeof(g_save_buf)) break;
    SaveMod m = { mod_table[i].x, mod_table[i].y, mod_table[i].block };
    memcpy(g_save_buf + offset, &m, sizeof(SaveMod));
    offset += sizeof(SaveMod);
    count++;
  }
  h.mod_count = count;
  memcpy(g_save_buf, &h, sizeof(SaveHeader));

  if (extapp_fileExists(SAVE_NAME)) {
    extapp_fileErase(SAVE_NAME);
  }
  return extapp_fileWrite(SAVE_NAME, (const char *)g_save_buf, offset);
}

static bool load_game(void) {
  size_t len = 0;
  const char * data = extapp_fileRead(SAVE_NAME, &len);
  if (!data || len < sizeof(SaveHeader)) return false;

  SaveHeader h;
  memcpy(&h, data, sizeof(SaveHeader));
  if (h.magic != SAVE_MAGIC) return false;

  size_t expected = sizeof(SaveHeader) + (size_t)h.mod_count * sizeof(SaveMod);
  if (len < expected) return false;

  g_seed = h.seed;
  player_x = h.player_x;
  player_y = h.player_y;
  facing = h.facing;
  selected_block = (uint8_t)h.selected_block;
  for (int i = 0; i < NUM_TYPES; i++) inventory[i] = h.inventory[i];
  health = h.health;
  hunger = h.hunger;

  for (int i = 0; i < MOD_TABLE_SIZE; i++) mod_table[i].used = 0;
  for (uint32_t i = 0; i < h.mod_count; i++) {
    SaveMod m;
    memcpy(&m, data + sizeof(SaveHeader) + (size_t)i * sizeof(SaveMod), sizeof(SaveMod));
    set_mod(m.x, m.y, m.block);
  }
  return true;
}

/* ================================================================== */
/* Rendering                                                            */
/* ================================================================== */

#define LIGHT_RADIUS  5.0f
#define LIGHT_FADE     4.0f

static eadk_color_t block_color(uint8_t b) {
  return (eadk_color_t)BLOCK_INFO[b].color;
}

/* Scale an RGB565 color towards black by a 0..1 brightness factor */
static eadk_color_t darken_color(eadk_color_t c, float brightness) {
  if (brightness >= 0.999f) return c;
  if (brightness <= 0.0f) return (eadk_color_t)0x0000u;
  uint16_t v = (uint16_t)c;
  uint16_t r = (uint16_t)(((v >> 11) & 0x1Fu) * brightness);
  uint16_t g = (uint16_t)(((v >> 5) & 0x3Fu) * brightness);
  uint16_t bch = (uint16_t)((v & 0x1Fu) * brightness);
  return (eadk_color_t)((r << 11) | (g << 5) | bch);
}

static bool craft_menu_open = false;
static int craft_cursor = 0;

#define MAX_VISIBLE_TORCHES 32
#define TORCH_LIGHT_RADIUS   4.0f
#define TORCH_LIGHT_FADE     3.0f

static int32_t g_torch_x[MAX_VISIBLE_TORCHES];
static int32_t g_torch_y[MAX_VISIBLE_TORCHES];
static int g_torch_count;

/* Placed torches light up their surroundings too, not just the player -
   gather the ones near the camera once per frame (cheap: mod_table is
   scanned, but only kept entries within view are stored). */
static void collect_visible_torches(int32_t cam_left) {
  g_torch_count = 0;
  int32_t min_x = cam_left - 6;
  int32_t max_x = cam_left + VISIBLE_COLS + 6;
  for (int i = 0; i < MOD_TABLE_SIZE && g_torch_count < MAX_VISIBLE_TORCHES; i++) {
    if (mod_table[i].used && mod_table[i].block == B_TORCH &&
        mod_table[i].x >= min_x && mod_table[i].x <= max_x) {
      g_torch_x[g_torch_count] = mod_table[i].x;
      g_torch_y[g_torch_count] = mod_table[i].y;
      g_torch_count++;
    }
  }
}

static void render(void) {
  eadk_display_push_rect_uniform(eadk_screen_rect, eadk_color_black);

  int cam_left = player_x - VISIBLE_COLS / 2;
  collect_visible_torches(cam_left);

  for (int col = 0; col < VISIBLE_COLS; col++) {
    int32_t wx = cam_left + col;
    int surf = surface_row(wx);
    for (int row = 0; row < WORLD_HEIGHT; row++) {
      uint8_t b = get_block(wx, row);
      eadk_color_t color = block_color(b);
      if (row > surf) {
        /* Underground: the player carries a "torch" radius, and any placed
           torches add their own light too (whichever is brighter wins).
           Distances are compared squared to avoid needing sqrtf. */
        int dx = (int)(wx - player_x);
        int dy = row - (int)player_y;
        float dist2 = (float)(dx * dx + dy * dy);
        const float r2 = LIGHT_RADIUS * LIGHT_RADIUS;
        const float f2 = (LIGHT_RADIUS + LIGHT_FADE) * (LIGHT_RADIUS + LIGHT_FADE);
        float brightness;
        if (dist2 <= r2) brightness = 1.0f;
        else if (dist2 <= f2) brightness = 1.0f - (dist2 - r2) / (f2 - r2);
        else brightness = 0.0f;

        const float tr2 = TORCH_LIGHT_RADIUS * TORCH_LIGHT_RADIUS;
        const float tf2 = (TORCH_LIGHT_RADIUS + TORCH_LIGHT_FADE) * (TORCH_LIGHT_RADIUS + TORCH_LIGHT_FADE);
        for (int t = 0; t < g_torch_count; t++) {
          int tdx = (int)(wx - g_torch_x[t]);
          int tdy = row - (int)g_torch_y[t];
          float td2 = (float)(tdx * tdx + tdy * tdy);
          float tb;
          if (td2 <= tr2) tb = 1.0f;
          else if (td2 <= tf2) tb = 1.0f - (td2 - tr2) / (tf2 - tr2);
          else tb = 0.0f;
          if (tb > brightness) brightness = tb;
        }
        color = darken_color(color, brightness);
      }
      eadk_rect_t r = { col * BLOCK_PX, HUD_TOP + row * BLOCK_PX, BLOCK_PX, BLOCK_PX };
      eadk_display_push_rect_uniform(r, color);
    }
  }

  /* Player */
  {
    int screen_col = player_x - cam_left;
    eadk_rect_t pr = { screen_col * BLOCK_PX + 1, HUD_TOP + player_y * BLOCK_PX + 1, BLOCK_PX - 2, BLOCK_PX - 2 };
    eadk_display_push_rect_uniform(pr, (eadk_color_t)0xF800u);
  }

  /* Target reticle (block in front) */
  {
    int32_t tx = player_x + facing;
    int screen_col = tx - cam_left;
    if (screen_col >= 0 && screen_col < VISIBLE_COLS) {
      int y = HUD_TOP + player_y * BLOCK_PX;
      int xpix = screen_col * BLOCK_PX;
      eadk_display_push_rect_uniform((eadk_rect_t){xpix, y, BLOCK_PX, 1}, eadk_color_white);
      eadk_display_push_rect_uniform((eadk_rect_t){xpix, y + BLOCK_PX - 1, BLOCK_PX, 1}, eadk_color_white);
      eadk_display_push_rect_uniform((eadk_rect_t){xpix, y, 1, BLOCK_PX}, eadk_color_white);
      eadk_display_push_rect_uniform((eadk_rect_t){xpix + BLOCK_PX - 1, y, 1, BLOCK_PX}, eadk_color_white);
    }
  }

  /* Top HUD bar */
  eadk_display_push_rect_uniform((eadk_rect_t){0, 0, EADK_SCREEN_WIDTH, HUD_TOP}, eadk_color_white);
  char top[80];
  static const char * const tier_names[5] = { "Main", "Bois", "Pierre", "Fer", "Diamant" };
  snprintf(top, sizeof(top), "Seed:%lu X:%ld Y:%ld PV:%ld/%d Faim:%ld/%d %s",
           (unsigned long)g_seed, (long)player_x, (long)player_y,
           (long)health, MAX_HEALTH, (long)hunger, MAX_HUNGER, tier_names[pickaxe_tier()]);
  eadk_display_draw_string(top, (eadk_point_t){2, 3}, false, eadk_color_black, eadk_color_white);

  /* Bottom HUD bar */
  eadk_display_push_rect_uniform((eadk_rect_t){0, EADK_SCREEN_HEIGHT - HUD_BOTTOM, EADK_SCREEN_WIDTH, HUD_BOTTOM}, eadk_color_white);
  char bot[80];
  if (message_timer > 0) {
    snprintf(bot, sizeof(bot), "%s", message);
  } else {
    snprintf(bot, sizeof(bot), "Objet:%s x%lu +/-:chg Toolbox:craft Shift:manger",
             BLOCK_INFO[selected_block].name, (unsigned long)inventory[selected_block]);
  }
  eadk_display_draw_string(bot, (eadk_point_t){2, EADK_SCREEN_HEIGHT - HUD_BOTTOM + 3}, false,
                            eadk_color_black, eadk_color_white);

  if (craft_menu_open) {
    eadk_rect_t box = { 15, 15, EADK_SCREEN_WIDTH - 30, EADK_SCREEN_HEIGHT - 30 };
    eadk_display_push_rect_uniform(box, eadk_color_white);
    eadk_display_draw_string("Etabli - OK: fabriquer, Toolbox: fermer",
                              (eadk_point_t){box.x + 4, box.y + 4}, false, eadk_color_black, eadk_color_white);
    for (size_t i = 0; i < NUM_RECIPES; i++) {
      const Recipe * r = &RECIPES[i];
      char line[64];
      if (r->need_type2 != 255) {
        snprintf(line, sizeof(line), "%dx%s + %dx%s -> %dx%s",
                 r->need_count, BLOCK_INFO[r->need_type].name,
                 r->need_count2, BLOCK_INFO[r->need_type2].name,
                 r->out_count, BLOCK_INFO[r->out_type].name);
      } else {
        snprintf(line, sizeof(line), "%dx%s -> %dx%s",
                 r->need_count, BLOCK_INFO[r->need_type].name,
                 r->out_count, BLOCK_INFO[r->out_type].name);
      }
      eadk_color_t bg = ((int)i == craft_cursor) ? (eadk_color_t)0xC618u : eadk_color_white;
      eadk_rect_t row_rect = { box.x + 2, box.y + 20 + (int)i * 16, box.width - 4, 16 };
      eadk_display_push_rect_uniform(row_rect, bg);
      eadk_display_draw_string(line, (eadk_point_t){box.x + 4, box.y + 22 + (int)i * 16}, false, eadk_color_black, bg);
    }
  }
}

/* ================================================================== */
/* Input helpers                                                        */
/* ================================================================== */

typedef struct {
  bool left, right, up, down, ok, exe, back, toolbox, plus, minus, ans, shift;
} Keys;

static Keys read_keys(void) {
  eadk_keyboard_state_t kb = eadk_keyboard_scan();
  Keys k;
  k.left = eadk_keyboard_key_down(kb, eadk_key_left);
  k.right = eadk_keyboard_key_down(kb, eadk_key_right);
  k.up = eadk_keyboard_key_down(kb, eadk_key_up);
  k.down = eadk_keyboard_key_down(kb, eadk_key_down);
  k.ok = eadk_keyboard_key_down(kb, eadk_key_ok);
  k.exe = eadk_keyboard_key_down(kb, eadk_key_exe);
  k.back = eadk_keyboard_key_down(kb, eadk_key_back);
  k.toolbox = eadk_keyboard_key_down(kb, eadk_key_toolbox);
  k.plus = eadk_keyboard_key_down(kb, eadk_key_plus);
  k.minus = eadk_keyboard_key_down(kb, eadk_key_minus);
  k.ans = eadk_keyboard_key_down(kb, eadk_key_ans);
  k.shift = eadk_keyboard_key_down(kb, eadk_key_shift);
  return k;
}

/* ================================================================== */
/* Seed entry screen                                                    */
/* ================================================================== */

static uint32_t read_seed(void) {
  char digits[10];
  int len = 0;
  bool prev[10] = {0};
  bool prev_exe = false, prev_ok = false;

  while (true) {
    eadk_display_push_rect_uniform(eadk_screen_rect, eadk_color_white);
    eadk_display_draw_string("NumCraft", (eadk_point_t){110, 40}, true, eadk_color_black, eadk_color_white);
    eadk_display_draw_string("Entre une seed (chiffres) puis EXE", (eadk_point_t){20, 90}, false, eadk_color_black, eadk_color_white);
    eadk_display_draw_string("ou appuie sur OK pour une seed aleatoire", (eadk_point_t){20, 106}, false, eadk_color_black, eadk_color_white);
    char buf[16];
    snprintf(buf, sizeof(buf), "> %s", digits);
    eadk_display_draw_string(buf, (eadk_point_t){20, 130}, true, (eadk_color_t)0x001Fu, eadk_color_white);

    eadk_keyboard_state_t kb = eadk_keyboard_scan();
    static const eadk_key_t digit_keys[10] = {
      eadk_key_zero, eadk_key_one, eadk_key_two, eadk_key_three, eadk_key_four,
      eadk_key_five, eadk_key_six, eadk_key_seven, eadk_key_eight, eadk_key_nine,
    };
    for (int i = 0; i < 10; i++) {
      bool down = eadk_keyboard_key_down(kb, digit_keys[i]);
      if (down && !prev[i] && len < 9) {
        digits[len++] = (char)('0' + i);
        digits[len] = '\0';
      }
      prev[i] = down;
    }
    bool exe = eadk_keyboard_key_down(kb, eadk_key_exe);
    bool ok = eadk_keyboard_key_down(kb, eadk_key_ok);
    if (exe && !prev_exe && len > 0) {
      return (uint32_t)strtoul(digits, NULL, 10);
    }
    if (ok && !prev_ok) {
      return eadk_random();
    }
    prev_exe = exe;
    prev_ok = ok;
    eadk_timing_msleep(30);
  }
}

/* ================================================================== */
/* Main                                                                  */
/* ================================================================== */

static void show_load_prompt_and_wait(bool * out_load) {
  bool prev_ok = false, prev_back = false;
  while (true) {
    eadk_display_push_rect_uniform(eadk_screen_rect, eadk_color_white);
    eadk_display_draw_string("NumCraft", (eadk_point_t){110, 40}, true, eadk_color_black, eadk_color_white);
    eadk_display_draw_string("Une sauvegarde a ete trouvee !", (eadk_point_t){30, 90}, false, eadk_color_black, eadk_color_white);
    eadk_display_draw_string("OK : reprendre la partie", (eadk_point_t){30, 110}, false, (eadk_color_t)0x03E0u, eadk_color_white);
    eadk_display_draw_string("Retour : nouvelle partie", (eadk_point_t){30, 126}, false, (eadk_color_t)0xF800u, eadk_color_white);

    eadk_keyboard_state_t kb = eadk_keyboard_scan();
    bool ok = eadk_keyboard_key_down(kb, eadk_key_ok);
    bool back = eadk_keyboard_key_down(kb, eadk_key_back);
    if (ok && !prev_ok) { *out_load = true; return; }
    if (back && !prev_back) { *out_load = false; return; }
    prev_ok = ok;
    prev_back = back;
    eadk_timing_msleep(30);
  }
}

int main(int argc, char * argv[]) {
  (void)argc;
  (void)argv;

  bool loaded = false;
  if (extapp_fileExists(SAVE_NAME)) {
    bool want_load = false;
    show_load_prompt_and_wait(&want_load);
    if (want_load) {
      loaded = load_game();
      if (!loaded) set_message("Sauvegarde illisible, nouvelle partie");
    }
  }

  if (!loaded) {
    g_seed = read_seed();
    player_x = 0;
    player_y = 0;
    {
      int surf = surface_row(0);
      player_y = surf - 1;
      if (player_y < 0) player_y = 0;
    }
    for (int i = 0; i < NUM_TYPES; i++) inventory[i] = 0;
    set_message("Bienvenue dans NumCraft !");
  } else {
    set_message("Partie chargee !");
  }

  bool prev_left = false, prev_right = false, prev_up = false, prev_down = false;
  bool prev_ok = false, prev_exe = false, prev_back = false;
  bool prev_toolbox = false, prev_plus = false, prev_minus = false;
  bool prev_ans = false, prev_shift = false;

  int fall_accum = 0;
  int32_t fall_start_y = -1;
  int hunger_timer = 0;
  int starve_timer = 0;

  while (true) {
    render();

    Keys k = read_keys();

    if (craft_menu_open) {
      if (k.down && !prev_down) { craft_cursor = (craft_cursor + 1) % (int)NUM_RECIPES; }
      if (k.up && !prev_up) { craft_cursor = (craft_cursor - 1 + (int)NUM_RECIPES) % (int)NUM_RECIPES; }
      if (k.ok && !prev_ok) {
        if (try_craft(&RECIPES[craft_cursor])) {
          set_message("Fabrique !");
        } else {
          set_message("Ingredients manquants");
        }
      }
      if ((k.toolbox && !prev_toolbox) || (k.back && !prev_back)) {
        craft_menu_open = false;
      }
    } else {
      if (k.left && !prev_left) {
        facing = -1;
        int32_t tx = player_x - 1;
        if (!is_solid(get_block(tx, player_y))) {
          player_x = tx;
        } else if (!is_solid(get_block(tx, player_y - 1)) && !is_solid(get_block(player_x, player_y - 1))) {
          player_x = tx;
          player_y -= 1;
        }
      }
      if (k.right && !prev_right) {
        facing = 1;
        int32_t tx = player_x + 1;
        if (!is_solid(get_block(tx, player_y))) {
          player_x = tx;
        } else if (!is_solid(get_block(tx, player_y - 1)) && !is_solid(get_block(player_x, player_y - 1))) {
          player_x = tx;
          player_y -= 1;
        }
      }
      if (k.up && !prev_up) {
        mine(player_x, player_y - 1);
      }
      if (k.down && !prev_down) {
        mine(player_x, player_y + 1);
      }
      if (k.ok && !prev_ok) {
        mine(player_x + facing, player_y);
      }
      if (k.exe && !prev_exe) {
        place(player_x + facing, player_y);
      }
      if (k.plus && !prev_plus) cycle_selected(1);
      if (k.minus && !prev_minus) cycle_selected(-1);
      if (k.toolbox && !prev_toolbox) { craft_menu_open = true; craft_cursor = 0; }
      if (k.ans && !prev_ans) {
        set_message(save_game() ? "Partie sauvegardee !" : "Erreur de sauvegarde !");
      }
      if (k.shift && !prev_shift) {
        if (inventory[I_APPLE] > 0) {
          inventory[I_APPLE]--;
          hunger += 6;
          if (hunger > MAX_HUNGER) hunger = MAX_HUNGER;
          set_message("Miam ! +6 faim");
        } else {
          set_message("Pas de pomme a manger");
        }
      }
      if (k.back && !prev_back) {
        save_game();
        return 0;
      }

      /* Gravity: fall if the block below is not solid, with fall damage */
      if (!is_solid(get_block(player_x, player_y + 1)) && player_y < WORLD_HEIGHT - 2) {
        if (fall_start_y < 0) fall_start_y = player_y;
        fall_accum++;
        if (fall_accum >= 3) {
          player_y++;
          fall_accum = 0;
        }
      } else {
        if (fall_start_y >= 0) {
          int fallen = (int)(player_y - fall_start_y);
          if (fallen > 3) {
            int dmg = fallen - 3;
            if (dmg > health) dmg = (int)health;
            health -= dmg;
            char b[48];
            snprintf(b, sizeof(b), "Chute ! -%d PV", dmg);
            set_message(b);
            if (health <= 0) respawn_player();
          }
          fall_start_y = -1;
        }
        fall_accum = 0;
      }

      /* Hunger slowly drains; starving drains health instead */
      hunger_timer++;
      if (hunger_timer >= 200) {
        hunger_timer = 0;
        if (hunger > 0) hunger--;
      }
      if (hunger <= 0) {
        starve_timer++;
        if (starve_timer >= 80) {
          starve_timer = 0;
          health--;
          if (health <= 0) respawn_player();
        }
      } else {
        starve_timer = 0;
      }
    }

    prev_left = k.left; prev_right = k.right; prev_up = k.up; prev_down = k.down;
    prev_ok = k.ok; prev_exe = k.exe; prev_back = k.back;
    prev_toolbox = k.toolbox; prev_plus = k.plus; prev_minus = k.minus;
    prev_ans = k.ans;
    prev_shift = k.shift;

    if (message_timer > 0) message_timer--;

    eadk_timing_msleep(60);
  }
}
