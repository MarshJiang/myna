#pragma once

#include <stdbool.h>
#include <stddef.h>

typedef struct Keyboard Keyboard;
typedef struct MynaLayoutSnapshot MynaLayoutSnapshot;

typedef enum {
  MYNA_MODE_LATIN = 0,
  MYNA_MODE_PINYIN = 1,
} MynaMode;

typedef struct {
  char *mode_label;
  char *preedit;
  size_t candidate_count;
  char **candidates;
  bool can_page_previous;
  bool can_page_next;
} MynaView;

typedef struct {
  char *commit;
  bool request_refresh;
} MynaAction;

typedef struct {
  double width;
  double height;
} MynaLayoutSize;

typedef struct {
  double origin_x;
  double origin_y;
  double scale_x;
  double scale_y;
} MynaLayoutTransform;

typedef struct {
  const char *label;
  const char *action;
  const char *class_name;
  double x;
  double y;
  double width;
  double height;
} MynaLayoutKey;

typedef enum {
  MYNA_LAYOUT_PAGE_MAIN = 0,
  MYNA_LAYOUT_PAGE_SYMBOLS = 1,
  MYNA_LAYOUT_PAGE_NUMBERS = 2,
  MYNA_LAYOUT_PAGE_MORE_SYMBOLS = 3,
} MynaLayoutPage;

Keyboard   *myna_keyboard_new              (void);
void        myna_keyboard_free             (Keyboard *kbd);
MynaMode    myna_keyboard_mode             (const Keyboard *kbd);
bool        myna_keyboard_shifted          (const Keyboard *kbd);
MynaAction *myna_keyboard_set_mode         (Keyboard *kbd, MynaMode mode);
MynaAction *myna_keyboard_toggle_mode      (Keyboard *kbd);
MynaAction *myna_keyboard_press_label      (Keyboard *kbd, const char *label);
MynaAction *myna_keyboard_press_physical_key
                                              (Keyboard *kbd,
                                               int keysym,
                                               const char *text);
MynaAction *myna_keyboard_page_candidates
                                              (Keyboard *kbd,
                                               bool next);
MynaAction *myna_keyboard_select_candidate (Keyboard *kbd, size_t index);
MynaAction *myna_keyboard_clear_composition(Keyboard *kbd);
MynaView   *myna_keyboard_view             (Keyboard *kbd);
void        myna_view_free                 (MynaView *view);
void        myna_action_free               (MynaAction *action);
void        myna_string_free               (char *value);

MynaLayoutSnapshot  *myna_layout_us_base_new       (void);
MynaLayoutSnapshot  *myna_layout_page_new          (MynaLayoutPage page);
MynaLayoutSnapshot  *myna_layout_page_for_mode_new (MynaLayoutPage page,
                                                    MynaMode mode);
MynaLayoutSnapshot  *myna_layout_page_for_keyboard_new
                                                   (MynaLayoutPage page,
                                                    const Keyboard *kbd);
void                 myna_layout_snapshot_free     (MynaLayoutSnapshot *snapshot);
size_t               myna_layout_key_count         (const MynaLayoutSnapshot *snapshot);
MynaLayoutSize       myna_layout_size              (const MynaLayoutSnapshot *snapshot);
MynaLayoutTransform  myna_layout_transform         (const MynaLayoutSnapshot *snapshot,
                                                    double available_width,
                                                    double available_height);
MynaLayoutKey        myna_layout_key_at            (const MynaLayoutSnapshot *snapshot,
                                                    size_t index);
