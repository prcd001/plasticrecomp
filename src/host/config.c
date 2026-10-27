/* config.c: plastic.ini (see config.h). */
#define WIN32_LEAN_AND_MEAN
#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "config.h"

#define MAX_KEYS 64
static struct { char key[40]; char val[96]; } g_kv[MAX_KEYS];
static int g_n;
static char g_path[MAX_PATH];
static int g_disabled;

static char* trim(char* s) {
    char* e;
    while (*s == ' ' || *s == '\t') s++;
    e = s + strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r' || e[-1] == '\n')) *--e = 0;
    return s;
}

static int find(const char* key) {
    for (int i = 0; i < g_n; i++)
        if (!_stricmp(g_kv[i].key, key)) return i;
    return -1;
}

static int read_file(const char* path) {
    FILE* f = fopen(path, "r");
    char line[256];
    if (!f) return 0;
    while (fgets(line, sizeof line, f)) {
        char* s = trim(line), *eq;
        if (!*s || *s == '#' || *s == ';' || *s == '[') continue;
        eq = strchr(s, '=');
        if (!eq) continue;
        *eq = 0;
        cfg_set_str(trim(s), trim(eq + 1));
    }
    fclose(f);
    return 1;
}

void cfg_load(const char* exe_dir, const char* game_dir, const char* explicit_path) {
    char a[MAX_PATH], b[MAX_PATH];
    if (g_disabled) return;
    g_n = 0;
    if (explicit_path && *explicit_path) {
        strncpy(g_path, explicit_path, MAX_PATH - 1);
        read_file(g_path);
    } else {
        _snprintf(a, sizeof a - 1, "%s\\plastic.ini", exe_dir);
        _snprintf(b, sizeof b - 1, "%s\\plastic.ini", game_dir);
        a[sizeof a - 1] = b[sizeof b - 1] = 0;
        if (read_file(a)) strcpy(g_path, a);
        else if (read_file(b)) strcpy(g_path, b);
        else strcpy(g_path, a);
    }
    fprintf(stderr, "[config] %s: %d key(s)\n", g_path, g_n);
}

int cfg_has(const char* key) { return find(key) >= 0; }
const char* cfg_get_str(const char* key) { int i = find(key); return i >= 0 ? g_kv[i].val : NULL; }
int cfg_get_int(const char* key, int def) {
    int i = find(key);
    return i >= 0 && g_kv[i].val[0] ? atoi(g_kv[i].val) : def;
}

void cfg_set_str(const char* key, const char* v) {
    int i = find(key);
    if (i < 0) {
        if (g_n >= MAX_KEYS) return;
        i = g_n++;
        strncpy(g_kv[i].key, key, sizeof g_kv[i].key - 1);
        g_kv[i].key[sizeof g_kv[i].key - 1] = 0;
    }
    strncpy(g_kv[i].val, v, sizeof g_kv[i].val - 1);
    g_kv[i].val[sizeof g_kv[i].val - 1] = 0;
}
void cfg_set_int(const char* key, int v) { char t[16]; _snprintf(t, sizeof t - 1, "%d", v); t[15] = 0; cfg_set_str(key, t); }

void cfg_disable(void) { g_disabled = 1; }
const char* cfg_path(void) { return g_path; }

void cfg_save(void) {
    FILE* f;
    if (g_disabled || !g_path[0]) return;
    f = fopen(g_path, "w");
    if (!f) { fprintf(stderr, "[config] cannot write %s\n", g_path); return; }
    fprintf(f, "# plasticrecomp settings: written when the game exits, read at start.\n"
               "# zoom_x 0.25 0.5 1 1.5 2 3 4 (the older `zoom` = index 0..4 of 1x 1.5x 2x 3x 4x is still read when zoom_x is absent); ui_scale in half steps (0 = follow the window);\n"
               "# scaling sharp-bilinear|nearest|smooth|scanlines; fullscreen, hires, vsync 0|1;\n"
               "# confine (F8: keep the mouse in the window in a mission), edge_scroll (F9) 0|1; hud_layout classic|bottom (F7).\n");
    for (int i = 0; i < g_n; i++) fprintf(f, "%s = %s\n", g_kv[i].key, g_kv[i].val);
    fclose(f);
    fprintf(stderr, "[config] wrote %s\n", g_path);
}
