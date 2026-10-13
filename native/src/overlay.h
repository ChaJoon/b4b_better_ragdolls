// `~` overlay (#26): panel registry and a small C widget API over Dear ImGui (overlay.cpp).
//
// A module adds a tab with overlay_add_panel() from its init; the draw function runs on the GAME thread (from
// overlay_tick, only while the window is open and that tab is shown), so it may read UE state and call the module's
// own functions directly, like a chat command handler. Use only the ov_* calls below inside it.
//
// Rules for panels:
//  - Actions go through the chat command path: ov_run("kick #2") runs admin_slash() (same permission checks, same
//    handler as `/kick #2`); its reply lands in the window's log. Settings go through ov_setting(): the module's ini
//    live handler (same as editing b4bcoop.ini) and, when saved, the ini writer (only that key).
//  - Controls a player may not use are drawn disabled with the reason: wrap them in ov_begin_perm(CMD_HOST/CMD_CHEAT)
//    ... ov_end_perm().
//  - Labels follow ImGui: "Kick##3" shows "Kick" with a unique id; the dev command `overlay press <label>` /
//    `overlay set <label> <value>` drives a control by its label (tests); `##id` alone matches the id part.
#pragma once
#ifdef __cplusplus
extern "C" {
#endif

typedef void (*OverlayDrawFn)(void);
// order: tab position (lower first). Built-in: Session 10, Players 20, Camera 30, Flashlight 40, Cheats 50,
// Add-ons 60 (looks.c), Settings 90, Updates 95 (updater.c), Help 100. Up to 24 panels; a second call with the
// same name replaces the draw function, draw NULL removes the tab.
void overlay_add_panel(const char *name, int order, OverlayDrawFn draw);
// Another name for a tab (dev `overlay tab <alias>`, e.g. a tab's old name): shows `panel` and runs select(arg).
void overlay_add_alias(const char *alias, const char *panel, void (*select)(int), int arg);
int overlay_is_open(void);
// that tab shows in a window of its own size: w_em font heights wide, h_frac of the screen's height (both clamped to
// the screen), and log_lines lines of the log under it (0 = the usual 6); the player's resizing is kept per such tab,
// the other tabs keep the window's usual size
void overlay_panel_size(const char *name, float w_em, float h_frac, int log_lines);
void overlay_close(void);               // e.g. a panel's button that opens a game screen (game thread)
void overlay_note(const char *text);   // a line in the window's log (any thread)

// ---- widgets (inside a draw function only) ----
void ov_text(const char *fmt, ...);
void ov_text_dim(const char *fmt, ...);       // secondary text, wrapped
void ov_text_warn(const char *fmt, ...);      // highlighted text, wrapped
void ov_text_dim_line(const char *fmt, ...);  // secondary text on one line, never wrapped (a size, a number)
void ov_heading(const char *text);            // a separator with a label
int ov_button(const char *label);             // 1 when clicked
int ov_button_confirm(const char *label, const char *confirm);   // needs a second click ("confirm" shown) within 3 s
int ov_checkbox(const char *label, int *v);   // 1 when changed
int ov_radio(const char *label, int active);  // 1 when clicked
int ov_selectable(const char *label, int selected);   // a clickable row/text (list selection); 1 when clicked
int ov_slider(const char *label, float *v, float lo, float hi, const char *fmt);   // 1 while changing
int ov_slider_int(const char *label, int *v, int lo, int hi);
int ov_input_text(const char *label, char *buf, int n, const char *hint);          // 1 when Enter was pressed
int ov_input_int(const char *label, int *v, int lo, int hi);                       // 1 when changed
int ov_combo(const char *label, int *cur, const char *const *items, int n);        // 1 when changed
int ov_edit_done(void);                       // the last slider/input: edit finished (released, Enter, left)
int ov_key(const char *label, int *vk);       // key binding: click, press a key (Esc cancels, Backspace = none)
const char *ov_key_name(int vk);              // "L", "~", "F5", "none", ...
void ov_same_line(void);
// next to the last control if the next one (a button with that label, or w_em font heights wide when label is NULL)
// still fits on the line, else it goes below; 1 = same line
int ov_same_line_fit(const char *label, float w_em);
void ov_progress(float frac, float w_em, const char *text);   // a progress bar (w_em 0 = the whole width), text on it
void ov_separator(void);
void ov_width(float em);                      // width of the next control, in font heights (0 = default)
void ov_tooltip(const char *text);            // for the last control (also when disabled)
int ov_header(const char *label, int default_open);   // collapsing section; 1 = open (dev press toggles it)
void ov_push_id(int id);
void ov_pop_id(void);
int ov_table_begin(const char *id, int cols);          // 1 = draw rows, then ov_table_end() (only then)
void ov_table_header(const char *const *names, int n); // column labels, first row
// a table that fills the space left above `below_px` and scrolls by itself (its label row stays); with less than
// min_em font heights for it, a plain table (the whole tab scrolls: one scrollbar, never two)
int ov_table_begin_scroll(const char *id, int cols, float below_px, float min_em);
float ov_cursor_y(void);                               // the next control's y in the panel, pixels (measure a section)
// instead of ov_table_header: column widths, w[i] > 0 a stretch weight, < 0 a fixed width of -w[i] font heights;
// names may be NULL (no labels); header 1 = a label row
void ov_table_columns(const char *const *names, const float *w, int n, int header);
void ov_table_next(void);                              // next cell (wraps to the next row)
void ov_table_end(void);
void ov_copy(const char *text);               // to the clipboard
// Pictures: ov_texture copies an RGBA image (max 512x512) into a texture for the rest of the session (0 = failed or
// the limit of OV_MAX_TEX is reached); ov_image draws it (w/h in font heights; tex 0 = empty space of that size).
#define OV_MAX_TEX 300
int ov_texture(const unsigned char *rgba, int w, int h);
void ov_image(int tex, float w_em, float h_em);
int ov_texture_set(int tex, const unsigned char *rgba, int w, int h);   // new pixels for a texture (same size); 1 = done
int ov_hovered(void);                                   // the last control is under the mouse
// a picture as the last control's tooltip: the texture's part (0,0)-(u1,v1), drawn w_em x h_em
void ov_tooltip_image(int tex, float w_em, float h_em, float u1, float v1);

// ---- permissions (cmds.h CMD_ANYONE / CMD_HOST / CMD_CHEAT / CMD_OWNSAVE): disabled block with a reason ----
int ov_allowed(int perm, const char **why);   // 1 if this machine may run such a command now
int ov_begin_perm(int perm);                  // returns ov_allowed(); always pair with ov_end_perm()
void ov_end_perm(void);
void ov_begin_disabled(int disabled, const char *why);
void ov_end_disabled(void);

// ---- actions and settings ----
void ov_run(const char *fmt, ...);            // a chat command without '/', e.g. ov_run("kick #%d", i)
void ov_setting(const char *key, const char *val, int save);   // live (module's *_live) + b4bcoop.ini when save
void ov_setting_f(const char *key, float v, int save);          // "%.0f" (or "%.2f" below 10)
void ov_setting_key(const char *key, int vk);                   // a key binding (saved): "L", "0xC0", "off"

#ifdef __cplusplus
}
#endif
