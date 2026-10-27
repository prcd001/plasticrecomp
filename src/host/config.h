/* config.h: plastic.ini, the host's persisted settings (config.c).
 *
 * "key = value" lines, '#' or ';' comments, [sections] ignored; unknown keys are kept as they are and
 * written back, never an error. Read at start, written when the game exits. The keys the host uses:
 *   zoom        battlefield zoom index 0..4 (1x, 1.5x, 2x, 3x, 4x)
 *   ui_scale    HUD scale in half steps (2 = 1x, 3 = 1.5x ...); 0 = follows the window
 *   scaling     sharp-bilinear | nearest | smooth | scanlines (or 0..3)
 *   fullscreen  0 / 1: borderless fullscreen
 *   hires       0 / 1: the battlefield fills the window
 *   vsync       0 / 1
 * Environment variables (PLASTIC_ZOOM, PLASTIC_UI_HALVES, PLASTIC_SCALING, PLASTIC_FULLSCREEN,
 * PLASTIC_HIRES, PLASTIC_VSYNC) and command-line flags override a value from the file for that run. */
#ifndef PLASTICRECOMP_CONFIG_H
#define PLASTICRECOMP_CONFIG_H

/* Read the file: explicit_path if given, else plastic.ini next to the exe, else in the game folder.
 * Writes go to the file that was read, or to the exe's folder when there was none. */
void        cfg_load(const char* exe_dir, const char* game_dir, const char* explicit_path);
int         cfg_has(const char* key);
int         cfg_get_int(const char* key, int def);
const char* cfg_get_str(const char* key);         /* NULL when absent */
void        cfg_set_int(const char* key, int v);
void        cfg_set_str(const char* key, const char* v);
void        cfg_save(void);                       /* no-op after cfg_disable() */
void        cfg_disable(void);                    /* --no-ini: neither read nor written */
const char* cfg_path(void);

#endif
