#include <gio/gio.h>
#include <gtk/gtk.h>

#include "bridge.h"
#include "layer_surface.h"
#include "myna_rs.h"

#define OSK_BUS_NAME "sm.puri.OSK0"
#define OSK_OBJECT_PATH "/sm/puri/OSK0"

typedef enum {
  LAYOUT_MAIN,
  LAYOUT_SYMBOLS,
  LAYOUT_NUMBERS,
} LayoutPage;

typedef struct _MynaApp MynaApp;

typedef struct {
  const char *label;
  const char *action;
  double width;
  const char *style_class;
} KeySpec;

typedef struct {
  GtkWidget *button;
  double x;
  double y;
  double width;
  double height;
} PlacedKey;

typedef struct {
  MynaApp *app;
  GtkWidget *button;
} LongPressContext;

static void
long_press_context_free (gpointer data, GClosure *closure)
{
  (void) closure;
  g_free (data);
}

struct _MynaApp {
  GtkApplication *app;
  MynaWayland wayland;
  Keyboard *keyboard;
  GtkWidget *window;
  GtkWidget *candidate_scroller;
  GtkWidget *candidate_box;
  GtkWidget *candidate_toggle_button;
  GtkWidget *candidate_panel;
  GtkWidget *preedit_label;
  GtkWidget *mode_button;
  GtkWidget *keyboard_fixed;
  GtkWidget *keyboard_area;
  GPtrArray *placed_keys;
  double layout_width;
  double layout_height;
  GDBusNodeInfo *introspection;
  GDBusConnection *bus;
  guint registration_id;
  gboolean visible;
  gboolean started;
  gboolean show_on_start;
  gboolean candidate_panel_open;
  gboolean candidate_available;
  LayoutPage page;
  char *session_client_id;
};

static const char introspection_xml[] =
  "<node>"
  "  <interface name='sm.puri.OSK0'>"
  "    <method name='SetVisible'>"
  "      <arg name='visible' type='b' direction='in'/>"
  "    </method>"
  "    <method name='GetVisible'>"
  "      <arg name='visible' type='b' direction='out'/>"
  "    </method>"
  "    <property name='Visible' type='b' access='read'/>"
  "  </interface>"
  "</node>";

static void refresh_view (MynaApp *self);
static void rebuild_keyboard (MynaApp *self);
static void set_visible (MynaApp *self, gboolean visible);
static gboolean relayout_keyboard_idle (gpointer user_data);

static int
panel_height_for_screen (void)
{
  GdkDisplay *display = gdk_display_get_default ();
  GdkMonitor *monitor = gdk_display_get_primary_monitor (display);
  GdkRectangle geometry;

  if (!monitor)
    return 320;

  gdk_monitor_get_geometry (monitor, &geometry);

  if (geometry.height <= 720)
    return CLAMP (geometry.height * 40 / 100, 286, 324);

  return CLAMP (geometry.height * 33 / 100, 304, 364);
}

static void
submit_text (MynaApp *self, const char *text)
{
  if (!text || !*text)
    return;

  if (text[0] == '\b' && text[1] == '\0') {
    if (!myna_wayland_delete_before (&self->wayland, 1) &&
        !myna_wayland_send_backspace (&self->wayland))
      g_warning ("Backspace requested but neither surrounding-text deletion nor virtual keyboard was available");
    return;
  }

  if (!myna_wayland_commit_string (&self->wayland, text))
    g_warning ("Commit requested while input-method-v2 is inactive: %s", text);
}

static void
handle_action (MynaApp *self, MynaAction *action)
{
  if (!action)
    return;

  submit_text (self, action->commit);

  if (action->request_refresh)
    refresh_view (self);

  myna_action_free (action);
}

static void
set_page (MynaApp *self, LayoutPage page)
{
  self->page = page;
  rebuild_keyboard (self);
  refresh_view (self);
}

static void
on_key_clicked (GtkButton *button, gpointer user_data)
{
  MynaApp *self = user_data;
  const char *label = g_object_get_data (G_OBJECT (button), "myna-label");

  if (g_object_get_data (G_OBJECT (button), "myna-long-pressed")) {
    g_object_set_data (G_OBJECT (button), "myna-long-pressed", NULL);
    return;
  }

  if (g_strcmp0 (label, "Symbols") == 0) {
    set_page (self, LAYOUT_SYMBOLS);
    return;
  }
  if (g_strcmp0 (label, "Numbers") == 0) {
    set_page (self, LAYOUT_NUMBERS);
    return;
  }
  if (g_strcmp0 (label, "Main") == 0) {
    set_page (self, LAYOUT_MAIN);
    return;
  }
  if (g_strcmp0 (label, "Hide") == 0) {
    set_visible (self, FALSE);
    return;
  }

  handle_action (self, myna_keyboard_press_label (self->keyboard, label));
}

static void
on_candidate_clicked (GtkButton *button, gpointer user_data)
{
  MynaApp *self = user_data;
  size_t index = GPOINTER_TO_SIZE (g_object_get_data (G_OBJECT (button), "myna-index"));

  self->candidate_panel_open = FALSE;
  gtk_stack_set_visible_child_name (GTK_STACK (self->keyboard_area), "keyboard");
  handle_action (self, myna_keyboard_select_candidate (self->keyboard, index));
}

static void
on_candidates_toggle_clicked (GtkButton *button, gpointer user_data)
{
  MynaApp *self = user_data;
  (void) button;

  if (self->candidate_panel_open) {
    self->candidate_panel_open = FALSE;
  } else if (self->candidate_available) {
    self->candidate_panel_open = TRUE;
  } else {
    set_visible (self, FALSE);
    return;
  }
  refresh_view (self);
}

static const char *
longpress_alternates_for (const char *action)
{
  if (g_strcmp0 (action, "a") == 0)
    return "ā á ǎ à";
  if (g_strcmp0 (action, "e") == 0)
    return "ē é ě è";
  if (g_strcmp0 (action, "i") == 0)
    return "ī í ǐ ì";
  if (g_strcmp0 (action, "o") == 0)
    return "ō ó ǒ ò";
  if (g_strcmp0 (action, "u") == 0)
    return "ū ú ǔ ù ü";
  if (g_strcmp0 (action, ",") == 0)
    return "， 、 ；";
  if (g_strcmp0 (action, ".") == 0)
    return "。 ！ ？";
  if (g_strcmp0 (action, "-") == 0)
    return "_ — ·";
  if (g_strcmp0 (action, "0") == 0)
    return "0 + =";
  if (g_strcmp0 (action, "1") == 0)
    return "1 @ #";
  if (g_strcmp0 (action, "2") == 0)
    return "2 / \\";
  if (g_strcmp0 (action, "3") == 0)
    return "3 : ;";
  if (g_strcmp0 (action, "4") == 0)
    return "4 ( )";
  if (g_strcmp0 (action, "5") == 0)
    return "5 [ ]";
  if (g_strcmp0 (action, "6") == 0)
    return "6 { }";
  if (g_strcmp0 (action, "7") == 0)
    return "7 \" '";
  if (g_strcmp0 (action, "8") == 0)
    return "8 * &";
  if (g_strcmp0 (action, "9") == 0)
    return "9 % $";
  return NULL;
}

static void
on_alternate_clicked (GtkButton *button, gpointer user_data)
{
  MynaApp *self = user_data;
  const char *text = gtk_button_get_label (button);

  submit_text (self, text);
}

static void
on_key_long_pressed (GtkGestureLongPress *gesture,
                     double x,
                     double y,
                     gpointer user_data)
{
  LongPressContext *context = user_data;
  MynaApp *self = context->app;
  GtkWidget *button = context->button;
  const char *action = g_object_get_data (G_OBJECT (button), "myna-label");
  const char *alternates = longpress_alternates_for (action);
  GtkWidget *popover;
  GtkWidget *box;
  char **items;

  (void) x;
  (void) y;
  (void) gesture;

  if (!alternates)
    return;

  g_object_set_data (G_OBJECT (button), "myna-long-pressed", GINT_TO_POINTER (1));

  popover = gtk_popover_new (button);
  box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 4);
  gtk_container_set_border_width (GTK_CONTAINER (box), 6);
  gtk_container_add (GTK_CONTAINER (popover), box);

  items = g_strsplit (alternates, " ", -1);
  for (guint i = 0; items[i]; i++) {
    GtkWidget *alt = gtk_button_new_with_label (items[i]);
    gtk_style_context_add_class (gtk_widget_get_style_context (alt), "alternate");
    g_signal_connect (alt, "clicked", G_CALLBACK (on_alternate_clicked), self);
    g_signal_connect_swapped (alt, "clicked", G_CALLBACK (gtk_widget_destroy), popover);
    gtk_box_pack_start (GTK_BOX (box), alt, FALSE, FALSE, 0);
  }
  g_strfreev (items);

  gtk_widget_show_all (popover);
  gtk_popover_popup (GTK_POPOVER (popover));
}

static const char *
display_label_for_action (const char *label, const char *action_label)
{
  if (g_strcmp0 (action_label, "Space") == 0)
    return "space";
  if (g_strcmp0 (action_label, "Enter") == 0)
    return "return";
  return label;
}

static GtkWidget *
make_button (const char *label, const char *action_label, const char *style_class)
{
  GtkWidget *button = gtk_button_new_with_label (display_label_for_action (label, action_label));
  GtkStyleContext *context = gtk_widget_get_style_context (button);

  gtk_widget_set_hexpand (button, TRUE);
  gtk_widget_set_vexpand (button, TRUE);
  gtk_widget_set_size_request (button, 1, 38);
  gtk_style_context_add_class (context, "key");
  if (style_class)
    gtk_style_context_add_class (context, style_class);
  g_object_set_data_full (G_OBJECT (button), "myna-label", g_strdup (action_label), g_free);

  return button;
}

static GtkWidget *
create_key_button (MynaApp *self, const char *label, const char *action_label, const char *style_class)
{
  GtkWidget *button = make_button (label, action_label, style_class);

  g_signal_connect (button, "clicked", G_CALLBACK (on_key_clicked), self);
  if (longpress_alternates_for (action_label)) {
    GtkGesture *gesture = gtk_gesture_long_press_new (button);
    LongPressContext *context = g_new0 (LongPressContext, 1);
    context->app = self;
    context->button = button;
    g_signal_connect_data (gesture, "pressed", G_CALLBACK (on_key_long_pressed), context, long_press_context_free, 0);
  }
  return button;
}

static void
placed_key_free (gpointer data)
{
  g_free (data);
}

static void
add_logical_row (MynaApp *self,
                 GtkFixed *fixed,
                 const KeySpec *keys,
                 guint n_keys,
                 double y,
                 double row_width,
                 double leading_width,
                 double key_height)
{
  double x = (self->layout_width - row_width) / 2.0 + leading_width;

  for (guint i = 0; i < n_keys; i++) {
    GtkWidget *button = create_key_button (self, keys[i].label, keys[i].action, keys[i].style_class);
    PlacedKey *placed = g_new0 (PlacedKey, 1);

    placed->button = button;
    placed->x = x;
    placed->y = y;
    placed->width = keys[i].width;
    placed->height = key_height;
    g_ptr_array_add (self->placed_keys, placed);
    gtk_fixed_put (fixed, button, 0, 0);
    if (g_strcmp0 (keys[i].action, "Mode") == 0) {
      self->mode_button = button;
      gtk_style_context_add_class (gtk_widget_get_style_context (button), "action");
    }

    x += keys[i].width;
  }
}

static double
row_width (const KeySpec *keys, guint n_keys)
{
  double width = 0.0;

  for (guint i = 0; i < n_keys; i++)
    width += keys[i].width;

  return width;
}

static void
layout_keyboard_buttons (MynaApp *self, GtkAllocation *allocation)
{
  double scale_x;
  double scale_y;
  double h_scale;
  double v_scale;
  double scale_x_final;
  double scale_y_final;
  double origin_x;
  double origin_y;

  if (!self->placed_keys || self->layout_width <= 0.0 || self->layout_height <= 0.0)
    return;
  if (allocation->width < 32 || allocation->height < 32)
    return;

  h_scale = allocation->width / self->layout_width;
  v_scale = allocation->height / self->layout_height;
  scale_x = (h_scale / v_scale) < 1.055 ? h_scale : v_scale;
  scale_y = MIN (h_scale, v_scale);
  scale_x_final = scale_x;
  scale_y_final = scale_y;
  origin_x = (allocation->width - self->layout_width * scale_x_final) / 2.0;
  origin_y = (allocation->height - self->layout_height * scale_y_final) / 2.0;

  for (guint i = 0; i < self->placed_keys->len; i++) {
    PlacedKey *key = g_ptr_array_index (self->placed_keys, i);
    int x = (int) (origin_x + key->x * scale_x_final);
    int y = (int) (origin_y + key->y * scale_y_final);
    int width = MAX (1, (int) (key->width * scale_x_final));
    int height = MAX (1, (int) (key->height * scale_y_final));

    gtk_widget_set_size_request (key->button, width, height);
    gtk_fixed_move (GTK_FIXED (self->keyboard_fixed), key->button, x, y);
  }
}

static void
on_keyboard_size_allocate (GtkWidget *widget, GtkAllocation *allocation, gpointer user_data)
{
  MynaApp *self = user_data;
  (void) widget;

  layout_keyboard_buttons (self, allocation);
}

static gboolean
relayout_keyboard_idle (gpointer user_data)
{
  MynaApp *self = user_data;
  GtkAllocation allocation;

  if (!self->keyboard_fixed || !gtk_widget_get_visible (self->keyboard_fixed))
    return G_SOURCE_REMOVE;

  gtk_widget_get_allocation (self->keyboard_fixed, &allocation);
  layout_keyboard_buttons (self, &allocation);
  gtk_widget_queue_draw (self->keyboard_fixed);

  return G_SOURCE_REMOVE;
}

static void
build_keyboard (MynaApp *self)
{
  GtkWidget *fixed = gtk_fixed_new ();
  const double key_height = 52.0;
  const double row_gap = 4.0;
  self->mode_button = NULL;
  g_clear_pointer (&self->placed_keys, g_ptr_array_unref);
  self->placed_keys = g_ptr_array_new_with_free_func (placed_key_free);
  self->layout_width = 0.0;
  self->layout_height = key_height * 4.0 + row_gap * 3.0;

  gtk_widget_set_hexpand (fixed, TRUE);
  gtk_widget_set_vexpand (fixed, TRUE);

  if (self->page == LAYOUT_SYMBOLS) {
    static const KeySpec row1[] = {
      { "1", "1", 35.33, NULL }, { "2", "2", 35.33, NULL }, { "3", "3", 35.33, NULL }, { "4", "4", 35.33, NULL }, { "5", "5", 35.33, NULL },
      { "6", "6", 35.33, NULL }, { "7", "7", 35.33, NULL }, { "8", "8", 35.33, NULL }, { "9", "9", 35.33, NULL }, { "0", "0", 35.33, NULL },
    };
    static const KeySpec row2[] = {
      { "@", "@", 35.33, NULL }, { "#", "#", 35.33, NULL }, { "$", "$", 35.33, NULL }, { "_", "_", 35.33, NULL }, { "&", "&", 35.33, NULL },
      { "-", "-", 35.33, NULL }, { "+", "+", 35.33, NULL }, { "(", "(", 35.33, NULL }, { ")", ")", 35.33, NULL },
    };
    static const KeySpec row3[] = {
      { "123", "Numbers", 52.67, "action" }, { "*", "*", 35.33, NULL }, { "\"", "\"", 35.33, NULL }, { "'", "'", 35.33, NULL },
      { ":", ":", 35.33, NULL }, { ";", ";", 35.33, NULL }, { "!", "!", 35.33, NULL }, { "?", "?", 35.33, NULL }, { "⌫", "Backspace", 52.67, "action" },
    };
    static const KeySpec row4[] = {
      { "ABC", "Main", 62.0, "action" }, { ",", ",", 44.0, NULL }, { "space", "Space", 142.0, "space" }, { ".", ".", 44.0, NULL }, { "return", "Enter", 62.0, "action" },
    };
    const KeySpec *rows[] = { row1, row2, row3, row4 };
    const guint counts[] = { G_N_ELEMENTS (row1), G_N_ELEMENTS (row2), G_N_ELEMENTS (row3), G_N_ELEMENTS (row4) };
    for (guint i = 0; i < 4; i++)
      self->layout_width = MAX (self->layout_width, row_width (rows[i], counts[i]));
    for (guint i = 0; i < 4; i++)
      add_logical_row (self, GTK_FIXED (fixed), rows[i], counts[i], i * (key_height + row_gap), row_width (rows[i], counts[i]), 0.0, key_height);
  } else if (self->page == LAYOUT_NUMBERS) {
    static const KeySpec row1[] = {
      { "1", "1", 110.0, NULL }, { "2", "2", 110.0, NULL }, { "3", "3", 110.0, NULL },
    };
    static const KeySpec row2[] = {
      { "4", "4", 110.0, NULL }, { "5", "5", 110.0, NULL }, { "6", "6", 110.0, NULL },
    };
    static const KeySpec row3[] = {
      { "7", "7", 110.0, NULL }, { "8", "8", 110.0, NULL }, { "9", "9", 110.0, NULL },
    };
    static const KeySpec row4[] = {
      { "ABC", "Main", 62.0, "action" }, { "0", "0", 110.0, NULL }, { "⌫", "Backspace", 62.0, "action" }, { "return", "Enter", 110.0, "action" },
    };
    const KeySpec *rows[] = { row1, row2, row3, row4 };
    const guint counts[] = { G_N_ELEMENTS (row1), G_N_ELEMENTS (row2), G_N_ELEMENTS (row3), G_N_ELEMENTS (row4) };
    for (guint i = 0; i < 4; i++)
      self->layout_width = MAX (self->layout_width, row_width (rows[i], counts[i]));
    for (guint i = 0; i < 4; i++)
      add_logical_row (self, GTK_FIXED (fixed), rows[i], counts[i], i * (key_height + row_gap), row_width (rows[i], counts[i]), 0.0, key_height);
  } else {
    static const KeySpec row1[] = {
      { "q", "q", 35.33, NULL }, { "w", "w", 35.33, NULL }, { "e", "e", 35.33, NULL }, { "r", "r", 35.33, NULL }, { "t", "t", 35.33, NULL },
      { "y", "y", 35.33, NULL }, { "u", "u", 35.33, NULL }, { "i", "i", 35.33, NULL }, { "o", "o", 35.33, NULL }, { "p", "p", 35.33, NULL },
    };
    static const KeySpec row2[] = {
      { "a", "a", 35.33, NULL }, { "s", "s", 35.33, NULL }, { "d", "d", 35.33, NULL }, { "f", "f", 35.33, NULL }, { "g", "g", 35.33, NULL },
      { "h", "h", 35.33, NULL }, { "j", "j", 35.33, NULL }, { "k", "k", 35.33, NULL }, { "l", "l", 35.33, NULL },
    };
    static const KeySpec row3[] = {
      { "⇧", "Shift", 52.67, "action" }, { "z", "z", 35.33, NULL }, { "x", "x", 35.33, NULL }, { "c", "c", 35.33, NULL }, { "v", "v", 35.33, NULL },
      { "b", "b", 35.33, NULL }, { "n", "n", 35.33, NULL }, { "m", "m", 35.33, NULL }, { "⌫", "Backspace", 52.67, "action" },
    };
    static const KeySpec row4[] = {
      { "?123", "Symbols", 62.0, "action" }, { "拼", "Mode", 44.0, "mode" },
      { "space", "Space", 142.0, "space" }, { ".", ".", 44.0, NULL }, { "return", "Enter", 62.0, "action" },
    };
    const KeySpec *rows[] = { row1, row2, row3, row4 };
    const guint counts[] = { G_N_ELEMENTS (row1), G_N_ELEMENTS (row2), G_N_ELEMENTS (row3), G_N_ELEMENTS (row4) };
    for (guint i = 0; i < 4; i++)
      self->layout_width = MAX (self->layout_width, row_width (rows[i], counts[i]));
    for (guint i = 0; i < 4; i++)
      add_logical_row (self, GTK_FIXED (fixed), rows[i], counts[i], i * (key_height + row_gap), row_width (rows[i], counts[i]), 0.0, key_height);
  }

  self->keyboard_fixed = fixed;
}

static void
rebuild_keyboard (MynaApp *self)
{
  if (self->keyboard_fixed)
    gtk_widget_destroy (self->keyboard_fixed);

  build_keyboard (self);
  g_signal_connect (self->keyboard_fixed, "size-allocate", G_CALLBACK (on_keyboard_size_allocate), self);
  gtk_stack_add_named (GTK_STACK (self->keyboard_area), self->keyboard_fixed, "keyboard");
  gtk_widget_queue_resize (self->keyboard_fixed);
  gtk_widget_show_all (self->keyboard_fixed);
  gtk_stack_set_visible_child_name (GTK_STACK (self->keyboard_area), self->candidate_panel_open ? "candidates" : "keyboard");
}

static void
clear_container (GtkWidget *container)
{
  GList *children = gtk_container_get_children (GTK_CONTAINER (container));

  for (GList *iter = children; iter; iter = iter->next)
    gtk_widget_destroy (GTK_WIDGET (iter->data));

  g_list_free (children);
}

static guint
utf8_chars (const char *text)
{
  return text ? (guint) g_utf8_strlen (text, -1) : 0;
}

static guint
candidate_grid_columns (MynaView *view)
{
  guint count = MIN (view->candidate_count, 12);
  double total = 0.0;

  if (count == 0)
    return 1;

  for (guint i = 0; i < count; i++)
    total += utf8_chars (view->candidates[i]);

  double avg = total / count;
  if (avg <= 1.2)
    return 6;
  if (avg <= 2.2)
    return 4;
  if (avg <= 4.0)
    return 3;
  return 2;
}

static void
style_candidate_label (GtkWidget *button, const char *candidate, gboolean compact)
{
  GtkWidget *label = gtk_bin_get_child (GTK_BIN (button));
  guint len = utf8_chars (candidate);

  if (!GTK_IS_LABEL (label))
    return;

  gtk_label_set_ellipsize (GTK_LABEL (label), PANGO_ELLIPSIZE_END);
  gtk_label_set_max_width_chars (GTK_LABEL (label), compact ? 8 : 16);

  if (len > 8)
    gtk_style_context_add_class (gtk_widget_get_style_context (button), "candidate-tiny");
  else if (len > 4)
    gtk_style_context_add_class (gtk_widget_get_style_context (button), "candidate-small");
}

static void
populate_candidate_panel (MynaApp *self, MynaView *view)
{
  guint columns = candidate_grid_columns (view);
  size_t visible_candidates = MIN (view->candidate_count, 24);

  clear_container (self->candidate_panel);
  gtk_grid_set_column_homogeneous (GTK_GRID (self->candidate_panel), TRUE);

  for (size_t i = 0; i < visible_candidates; i++) {
    GtkWidget *button = gtk_button_new_with_label (view->candidates[i]);
    int column = (int) (i % columns);
    int row = (int) (i / columns);

    gtk_widget_set_hexpand (button, TRUE);
    gtk_widget_set_vexpand (button, FALSE);
    gtk_widget_set_halign (button, GTK_ALIGN_FILL);
    gtk_widget_set_valign (button, GTK_ALIGN_START);
    gtk_widget_set_size_request (button, 1, 40);
    gtk_style_context_add_class (gtk_widget_get_style_context (button), "candidate-cell");
    if (i == 0)
      gtk_style_context_add_class (gtk_widget_get_style_context (button), "candidate-primary");
    style_candidate_label (button, view->candidates[i], FALSE);
    g_object_set_data (G_OBJECT (button), "myna-index", GSIZE_TO_POINTER (i));
    g_signal_connect (button, "clicked", G_CALLBACK (on_candidate_clicked), self);
    gtk_grid_attach (GTK_GRID (self->candidate_panel), button, column, row, 1, 1);
  }
}

static void
refresh_view (MynaApp *self)
{
  MynaView *view = myna_keyboard_view (self->keyboard);

  if (!view)
    return;

  if (self->mode_button)
    gtk_button_set_label (GTK_BUTTON (self->mode_button), view->mode_label);
  gtk_label_set_text (GTK_LABEL (self->preedit_label), view->preedit ? view->preedit : "");
  clear_container (self->candidate_box);

  gboolean has_preedit = view->preedit && view->preedit[0] != '\0';
  self->candidate_available = has_preedit && view->candidate_count > 0;
  if (!self->candidate_available)
    self->candidate_panel_open = FALSE;

  gtk_button_set_label (
      GTK_BUTTON (self->candidate_toggle_button),
      self->candidate_panel_open ? "‹" : (self->candidate_available ? "›" : "⌄"));
  gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (self->candidate_scroller), GTK_POLICY_AUTOMATIC, GTK_POLICY_NEVER);
  gtk_widget_set_size_request (self->candidate_scroller, 1, 38);

  size_t visible_candidates = has_preedit ? view->candidate_count : 0;
  for (size_t i = 0; i < visible_candidates; i++) {
    GtkWidget *button = gtk_button_new_with_label (view->candidates[i]);
    int column = (int) i;

    gtk_widget_set_hexpand (button, FALSE);
    gtk_widget_set_halign (button, GTK_ALIGN_START);
    gtk_widget_set_size_request (button, -1, 34);
    gtk_style_context_add_class (gtk_widget_get_style_context (button), "candidate");
    if (i == 0)
      gtk_style_context_add_class (gtk_widget_get_style_context (button), "candidate-primary");
    g_object_set_data (G_OBJECT (button), "myna-index", GSIZE_TO_POINTER (i));
    g_signal_connect (button, "clicked", G_CALLBACK (on_candidate_clicked), self);
    gtk_grid_attach (GTK_GRID (self->candidate_box), button, column, 0, 1, 1);
  }

  if (self->candidate_available)
    populate_candidate_panel (self, view);
  else
    clear_container (self->candidate_panel);
  gtk_stack_set_visible_child_name (GTK_STACK (self->keyboard_area), self->candidate_panel_open ? "candidates" : "keyboard");
  gtk_widget_show_all (self->candidate_box);
  if (self->candidate_panel_open)
    gtk_widget_show_all (self->candidate_panel);
  myna_view_free (view);
}

static void
set_visible (MynaApp *self, gboolean visible)
{
  self->visible = visible;

  if (visible)
    gtk_widget_show_all (self->window);
  else
    gtk_widget_hide (self->window);

  if (visible && self->keyboard_fixed) {
    gtk_widget_queue_resize (self->keyboard_area);
    gtk_widget_queue_resize (self->keyboard_fixed);
    g_idle_add (relayout_keyboard_idle, self);
  }

  g_message ("OSK visibility set to %s", visible ? "true" : "false");

  if (self->bus) {
    GVariantBuilder changed;
    GVariantBuilder invalidated;

    g_variant_builder_init (&changed, G_VARIANT_TYPE ("a{sv}"));
    g_variant_builder_add (&changed, "{sv}", "Visible", g_variant_new_boolean (visible));
    g_variant_builder_init (&invalidated, G_VARIANT_TYPE ("as"));

    g_dbus_connection_emit_signal (
        self->bus,
        NULL,
        OSK_OBJECT_PATH,
        "org.freedesktop.DBus.Properties",
        "PropertiesChanged",
        g_variant_new ("(sa{sv}as)", OSK_BUS_NAME, &changed, &invalidated),
        NULL);
  }
}

static void
dbus_method_call (GDBusConnection *connection,
                  const char *sender,
                  const char *object_path,
                  const char *interface_name,
                  const char *method_name,
                  GVariant *parameters,
                  GDBusMethodInvocation *invocation,
                  gpointer user_data)
{
  MynaApp *self = user_data;
  (void) connection;
  (void) sender;
  (void) object_path;
  (void) interface_name;

  if (g_strcmp0 (method_name, "SetVisible") == 0) {
    gboolean visible;
    g_variant_get (parameters, "(b)", &visible);
    set_visible (self, visible);
    g_dbus_method_invocation_return_value (invocation, NULL);
  } else if (g_strcmp0 (method_name, "GetVisible") == 0) {
    g_dbus_method_invocation_return_value (invocation, g_variant_new ("(b)", self->visible));
  }
}

static GVariant *
dbus_get_property (GDBusConnection *connection,
                   const char *sender,
                   const char *object_path,
                   const char *interface_name,
                   const char *property_name,
                   GError **error,
                   gpointer user_data)
{
  MynaApp *self = user_data;
  (void) connection;
  (void) sender;
  (void) object_path;
  (void) interface_name;
  (void) error;

  if (g_strcmp0 (property_name, "Visible") == 0)
    return g_variant_new_boolean (self->visible);

  return NULL;
}

static const GDBusInterfaceVTable dbus_vtable = {
  .method_call = dbus_method_call,
  .get_property = dbus_get_property,
};

static void register_gnome_session_client (MynaApp *self);

static void
on_bus_acquired (GDBusConnection *connection, const char *name, gpointer user_data)
{
  MynaApp *self = user_data;
  GError *error = NULL;
  (void) name;

  self->bus = g_object_ref (connection);
  self->introspection = g_dbus_node_info_new_for_xml (introspection_xml, &error);
  if (error) {
    g_warning ("D-Bus introspection failed: %s", error->message);
    g_clear_error (&error);
    return;
  }

  self->registration_id = g_dbus_connection_register_object (
      connection,
      OSK_OBJECT_PATH,
      self->introspection->interfaces[0],
      &dbus_vtable,
      self,
      NULL,
      &error);

  if (error) {
    g_warning ("D-Bus object registration failed: %s", error->message);
    g_clear_error (&error);
    return;
  }

  g_message ("Registered %s on %s", OSK_BUS_NAME, OSK_OBJECT_PATH);
  register_gnome_session_client (self);
}

static void
register_gnome_session_client (MynaApp *self)
{
  GError *error = NULL;
  const char *startup_id = g_getenv ("DESKTOP_AUTOSTART_ID");
  GVariant *reply;

  if (!self->bus)
    return;
  if (self->session_client_id)
    return;

  reply = g_dbus_connection_call_sync (
      self->bus,
      "org.gnome.SessionManager",
      "/org/gnome/SessionManager",
      "org.gnome.SessionManager",
      "RegisterClient",
      g_variant_new ("(ss)", OSK_BUS_NAME, startup_id ? startup_id : ""),
      G_VARIANT_TYPE ("(o)"),
      G_DBUS_CALL_FLAGS_NONE,
      1000,
      NULL,
      &error);

  if (error) {
    g_warning ("gnome-session registration failed: %s", error->message);
    g_clear_error (&error);
    return;
  }

  g_variant_get (reply, "(o)", &self->session_client_id);
  g_variant_unref (reply);
  g_message ("Registered gnome-session client: %s", self->session_client_id);
}

static void
install_css (void)
{
  static const char css[] =
    "window { background: #2b2b2f; }"
    ".root { padding: 6px 7px 8px 7px; }"
    ".candidate-strip { min-height: 40px; padding-bottom: 2px; }"
    ".preedit { color: #f6f2ea; font-size: 15px; padding: 0 8px; min-width: 52px; }"
    ".candidate { border-radius: 4px; margin: 0 2px; padding: 5px 8px; font-size: 18px; background: transparent; color: #f5f5f5; border: 0; box-shadow: none; }"
    ".candidate-primary { background: #e9e2d0; color: #1d1d1f; font-weight: 700; }"
    ".candidate-toggle { min-width: 34px; color: #d8d8d8; }"
    ".hide-key { min-width: 34px; color: #d8d8d8; }"
    ".candidate-cell { border-radius: 5px; margin: 2px; padding: 5px 8px; font-size: 18px; min-height: 42px; }"
    ".candidate-small { font-size: 15px; }"
    ".candidate-tiny { font-size: 13px; }"
    ".key { border-radius: 4px; margin: 2px; padding: 0; font-size: 18px; min-height: 38px; background: #f2f0ea; color: #1f1f23; border: 0; box-shadow: 0 1px 0 rgba(0,0,0,0.35); }"
    ".key:active { background: #d8d3c8; }"
    ".action { background: #6f737b; color: #ffffff; font-size: 15px; }"
    ".action:active { background: #858a93; }"
    ".alternate { border-radius: 4px; min-width: 40px; min-height: 36px; font-size: 18px; }"
    ".space { font-size: 14px; }"
    ".mode { font-weight: 700; }";
  GtkCssProvider *provider = gtk_css_provider_new ();

  gtk_css_provider_load_from_data (provider, css, -1, NULL);
  gtk_style_context_add_provider_for_screen (
      gdk_screen_get_default (),
      GTK_STYLE_PROVIDER (provider),
      GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
  g_object_unref (provider);
}

static void
activate (GtkApplication *app, gpointer user_data)
{
  MynaApp *self = user_data;
  GtkWidget *root;
  GtkWidget *candidate_row;
  int height;

  self->app = app;
  self->started = TRUE;
  self->keyboard = myna_keyboard_new ();

  if (!myna_wayland_init (&self->wayland))
    g_warning ("Wayland setup was incomplete");

  install_css ();
  height = panel_height_for_screen ();
  self->window = myna_layer_surface_new (self->wayland.layer_shell, self->wayland.output, height);
  gtk_application_add_window (app, GTK_WINDOW (self->window));

  root = gtk_box_new (GTK_ORIENTATION_VERTICAL, 6);
  gtk_style_context_add_class (gtk_widget_get_style_context (root), "root");
  gtk_container_add (GTK_CONTAINER (self->window), root);

  candidate_row = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 4);
  gtk_style_context_add_class (gtk_widget_get_style_context (candidate_row), "candidate-strip");
  gtk_widget_set_hexpand (candidate_row, TRUE);
  gtk_box_pack_start (GTK_BOX (root), candidate_row, FALSE, TRUE, 0);

  self->preedit_label = gtk_label_new ("");
  gtk_label_set_xalign (GTK_LABEL (self->preedit_label), 0.0);
  gtk_style_context_add_class (gtk_widget_get_style_context (self->preedit_label), "preedit");
  gtk_box_pack_start (GTK_BOX (candidate_row), self->preedit_label, FALSE, TRUE, 0);

  self->candidate_scroller = gtk_scrolled_window_new (NULL, NULL);
  gtk_scrolled_window_set_shadow_type (GTK_SCROLLED_WINDOW (self->candidate_scroller), GTK_SHADOW_NONE);
  gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (self->candidate_scroller), GTK_POLICY_AUTOMATIC, GTK_POLICY_NEVER);
  gtk_widget_set_hexpand (self->candidate_scroller, TRUE);
  gtk_box_pack_start (GTK_BOX (candidate_row), self->candidate_scroller, TRUE, TRUE, 0);

  self->candidate_toggle_button = gtk_button_new_with_label ("⌄");
  gtk_style_context_add_class (gtk_widget_get_style_context (self->candidate_toggle_button), "candidate");
  gtk_style_context_add_class (gtk_widget_get_style_context (self->candidate_toggle_button), "candidate-toggle");
  g_signal_connect (self->candidate_toggle_button, "clicked", G_CALLBACK (on_candidates_toggle_clicked), self);
  gtk_box_pack_start (GTK_BOX (candidate_row), self->candidate_toggle_button, FALSE, FALSE, 0);

  self->candidate_box = gtk_grid_new ();
  gtk_grid_set_row_spacing (GTK_GRID (self->candidate_box), 3);
  gtk_grid_set_column_spacing (GTK_GRID (self->candidate_box), 3);
  gtk_widget_set_hexpand (self->candidate_box, TRUE);
  gtk_container_add (GTK_CONTAINER (self->candidate_scroller), self->candidate_box);

  build_keyboard (self);
  g_signal_connect (self->keyboard_fixed, "size-allocate", G_CALLBACK (on_keyboard_size_allocate), self);
  self->keyboard_area = gtk_stack_new ();
  gtk_stack_set_transition_type (GTK_STACK (self->keyboard_area), GTK_STACK_TRANSITION_TYPE_NONE);
  gtk_widget_set_hexpand (self->keyboard_area, TRUE);
  gtk_widget_set_vexpand (self->keyboard_area, FALSE);
  gtk_widget_set_size_request (self->keyboard_area, 1, MAX (224, height - 58));
  gtk_box_pack_start (GTK_BOX (root), self->keyboard_area, TRUE, TRUE, 0);
  gtk_stack_add_named (GTK_STACK (self->keyboard_area), self->keyboard_fixed, "keyboard");

  self->candidate_panel = gtk_grid_new ();
  gtk_grid_set_row_spacing (GTK_GRID (self->candidate_panel), 4);
  gtk_grid_set_column_spacing (GTK_GRID (self->candidate_panel), 4);
  gtk_widget_set_hexpand (self->candidate_panel, TRUE);
  gtk_widget_set_vexpand (self->candidate_panel, FALSE);
  gtk_widget_set_valign (self->candidate_panel, GTK_ALIGN_START);
  gtk_stack_add_named (GTK_STACK (self->keyboard_area), self->candidate_panel, "candidates");
  gtk_stack_set_visible_child_name (GTK_STACK (self->keyboard_area), "keyboard");
  gtk_widget_queue_resize (self->keyboard_fixed);

  refresh_view (self);
  set_visible (self, self->show_on_start);
}

static int
command_line (GApplication *app, GApplicationCommandLine *command_line, gpointer user_data)
{
  MynaApp *self = user_data;
  char **argv;
  int argc;

  argv = g_application_command_line_get_arguments (command_line, &argc);
  for (int i = 1; i < argc; i++) {
    if (g_strcmp0 (argv[i], "--show") == 0)
      self->show_on_start = TRUE;
  }
  g_strfreev (argv);

  g_application_activate (app);
  return 0;
}

int
main (int argc, char **argv)
{
  MynaApp self = {0};
  GtkApplication *app;
  int status;
  guint bus_id;

  app = gtk_application_new ("mobi.phosh.MynaOsk", G_APPLICATION_NON_UNIQUE | G_APPLICATION_HANDLES_COMMAND_LINE);
  bus_id = g_bus_own_name (
      G_BUS_TYPE_SESSION,
      OSK_BUS_NAME,
      G_BUS_NAME_OWNER_FLAGS_NONE,
      on_bus_acquired,
      NULL,
      NULL,
      &self,
      NULL);

  g_signal_connect (app, "activate", G_CALLBACK (activate), &self);
  g_signal_connect (app, "command-line", G_CALLBACK (command_line), &self);
  status = g_application_run (G_APPLICATION (app), argc, argv);

  if (self.registration_id && self.bus)
    g_dbus_connection_unregister_object (self.bus, self.registration_id);
  g_clear_object (&self.bus);
  g_clear_pointer (&self.introspection, g_dbus_node_info_unref);
  g_clear_pointer (&self.session_client_id, g_free);
  g_bus_unown_name (bus_id);
  myna_keyboard_free (self.keyboard);
  myna_wayland_destroy (&self.wayland);
  g_object_unref (app);

  return status;
}
