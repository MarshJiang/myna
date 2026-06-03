#pragma once

#include <stdbool.h>
#include <stddef.h>

typedef struct Keyboard Keyboard;

typedef enum {
  MYNA_MODE_LATIN = 0,
  MYNA_MODE_PINYIN = 1,
} MynaMode;

typedef struct {
  char *mode_label;
  char *preedit;
  size_t candidate_count;
  char **candidates;
} MynaView;

typedef struct {
  char *commit;
  bool request_refresh;
} MynaAction;

Keyboard   *myna_keyboard_new              (void);
void        myna_keyboard_free             (Keyboard *kbd);
MynaMode    myna_keyboard_mode             (const Keyboard *kbd);
MynaAction *myna_keyboard_set_mode         (Keyboard *kbd, MynaMode mode);
MynaAction *myna_keyboard_toggle_mode      (Keyboard *kbd);
MynaAction *myna_keyboard_press_label      (Keyboard *kbd, const char *label);
MynaAction *myna_keyboard_select_candidate (Keyboard *kbd, size_t index);
MynaView   *myna_keyboard_view             (Keyboard *kbd);
void        myna_view_free                 (MynaView *view);
void        myna_action_free               (MynaAction *action);
void        myna_string_free               (char *value);
