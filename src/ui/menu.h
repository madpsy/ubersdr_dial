#pragma once
#include <lvgl.h>

// Settings menu shared by both screens. The main list shows each setting with
// its current value; tapping one opens its own sheet of choices (round buttons
// on the rim for up to six, a list otherwise). Picking applies it and returns
// to the list where you were. Actions (no choices) just run.
struct MenuChoice {
  const char *label;
  int value;
};

struct MenuItem {
  const char *icon;
  const char *title;
  // Setting: current value, apply a value, and the choices. Leave `choices`
  // null for an action, which runs `set(0)`.
  int (*get)();
  void (*set)(int value);
  const MenuChoice *choices;
  int n_choices;
  bool (*visible)();  // hide when it doesn't apply (omit = always shown)
};

// `slot` is the screen's overlay pointer (so it knows a menu is open and can
// route the knob); the menu keeps it up to date and nulls it when closed.
void menu_open(lv_obj_t *screen, const char *title, uint32_t title_col, const MenuItem *items, int n,
               lv_obj_t **slot);
void menu_close();
// Knob while a menu/sheet is open: scrolls the list.
void menu_knob(int detents);
