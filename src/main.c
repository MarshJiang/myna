#include <gio/gio.h>
#include <gtk/gtk.h>

#include "bridge.h"
#include "layer_surface.h"
#include "myna_rs.h"
#include "rime_shim.h"

#define APP_ID "mobi.phosh.MynaOsk"
#define OSK_BUS_NAME "sm.puri.OSK0"
#define OSK_OBJECT_PATH "/sm/puri/OSK0"

typedef enum {
  LAYOUT_MAIN,
  LAYOUT_SYMBOLS,
  LAYOUT_NUMBERS,
  LAYOUT_MORE_SYMBOLS,
} LayoutPage;

typedef enum {
  CANDIDATE_SOURCE_TOUCH,
  CANDIDATE_SOURCE_PHYSICAL,
} CandidateSource;

typedef struct _MynaApp MynaApp;

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

static gboolean
should_deploy_rime (int argc, char **argv)
{
  for (int i = 1; i < argc; i++) {
    if (g_strcmp0 (argv[i], "--rime-deploy") == 0)
      return TRUE;
  }

  return FALSE;
}

struct _MynaApp {
  GtkApplication *app;
  MynaWayland wayland;
  Keyboard *keyboard;
  GtkWidget *window;
  GtkWidget *candidate_scroller;
  GtkWidget *candidate_box;
  GtkWidget *candidate_prev_button;
  GtkWidget *candidate_next_button;
  GtkWidget *candidate_toggle_button;
  GtkWidget *candidate_panel;
  GtkWidget *preedit_label;
  GtkWidget *mode_button;
  GtkWidget *keyboard_fixed;
  GtkWidget *keyboard_area;
  GPtrArray *placed_keys;
  MynaLayoutSnapshot *layout_snapshot;
  GDBusNodeInfo *introspection;
  GDBusConnection *bus;
  GDBusProxy *session_proxy;
  GDBusProxy *session_client_proxy;
  guint registration_id;
  gboolean visible;
  gboolean started;
  gboolean show_on_start;
  gboolean bus_name_acquired;
  gboolean bus_name_lost;
  gboolean activation_pending;
  gboolean candidate_panel_open;
  gboolean candidate_available;
  CandidateSource candidate_source;
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
static gboolean handle_action (MynaApp *self, MynaAction *action);
static void handle_candidate_page (MynaApp *self, gboolean next);
static gboolean relayout_keyboard_idle (gpointer user_data);
static void register_gnome_session_client (MynaApp *self);
static void unregister_gnome_session_client (MynaApp *self);
static gboolean register_dbus_object (MynaApp *self);
static void on_session_client_signal (GDBusProxy *proxy,
                                      const char *sender_name,
                                      const char *signal_name,
                                      GVariant *parameters,
                                      gpointer user_data);
static bool on_wayland_physical_key (void *user_data, uint32_t keysym, const char *text);

static int
panel_height_for_screen (void)
{
  GdkDisplay *display = gdk_display_get_default ();
  GdkMonitor *monitor = gdk_display_get_primary_monitor (display);
  GdkRectangle geometry;
  int scale;
  int logical_width;
  int logical_height;
  int height;

  if (!monitor && gdk_display_get_n_monitors (display) > 0)
    monitor = gdk_display_get_monitor (display, 0);
  if (!monitor) {
    g_warning ("No monitor available for panel sizing; using fallback height 320");
    return 320;
  }

  gdk_monitor_get_geometry (monitor, &geometry);
  scale = MAX (1, gdk_monitor_get_scale_factor (monitor));
  logical_width = geometry.width / scale;
  logical_height = geometry.height / scale;
  height = CLAMP (logical_width * 280 / 360, 280, 336);

  g_message ("Panel height: monitor=%dx%d scale=%d logical=%dx%d height=%d",
             geometry.width,
             geometry.height,
             scale,
             logical_width,
             logical_height,
             height);

  return height;
}

static void
submit_text (MynaApp *self, const char *text)
{
  if (!text || !*text)
    return;

  if (text[0] == '\b' && text[1] == '\0') {
    myna_wayland_ensure_input_method (&self->wayland);
    if (!myna_wayland_delete_before (&self->wayland, 1) &&
        !myna_wayland_send_backspace (&self->wayland))
      g_warning ("Backspace requested but neither surrounding-text deletion nor virtual keyboard was available");
    return;
  }

  myna_wayland_ensure_input_method (&self->wayland);
  if (!myna_wayland_commit_string (&self->wayland, text))
    g_warning ("Commit requested but input-method-v2 could not accept text: %s", text);
}

static void
on_wayland_visibility_requested (void *user_data, bool visible)
{
  MynaApp *self = user_data;

  if (!self->started)
    return;

  set_visible (self, visible);
}

static bool
on_wayland_physical_key (void *user_data, uint32_t keysym, const char *text)
{
  MynaApp *self = user_data;
  CandidateSource previous_source;
  gboolean handled;

  if (!self->started)
    return false;

  previous_source = self->candidate_source;
  self->candidate_source = CANDIDATE_SOURCE_PHYSICAL;
  handled = handle_action (self, myna_keyboard_press_physical_key (self->keyboard, (int) keysym, text));
  if (!handled)
    self->candidate_source = previous_source;

  return handled;
}

static gboolean
handle_action (MynaApp *self, MynaAction *action)
{
  gboolean handled;

  if (!action)
    return FALSE;

  handled = (action->commit && action->commit[0] != '\0') || action->request_refresh;

  submit_text (self, action->commit);

  if (action->request_refresh)
    refresh_view (self);

  myna_action_free (action);
  return handled;
}

static void
handle_key_action (MynaApp *self, const char *label)
{
  gboolean was_shifted = myna_keyboard_shifted (self->keyboard);

  self->candidate_source = CANDIDATE_SOURCE_TOUCH;
  handle_action (self, myna_keyboard_press_label (self->keyboard, label));
  if (g_strcmp0 (label, "Mode") == 0 || was_shifted != myna_keyboard_shifted (self->keyboard)) {
    rebuild_keyboard (self);
    refresh_view (self);
  }
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
  if (g_strcmp0 (label, "MoreSymbols") == 0) {
    set_page (self, LAYOUT_MORE_SYMBOLS);
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

  handle_key_action (self, label);
}

static void
on_candidate_clicked (GtkButton *button, gpointer user_data)
{
  MynaApp *self = user_data;
  size_t index = GPOINTER_TO_SIZE (g_object_get_data (G_OBJECT (button), "myna-index"));

  self->candidate_source = CANDIDATE_SOURCE_TOUCH;
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

static void
on_candidate_page_clicked (GtkButton *button, gpointer user_data)
{
  MynaApp *self = user_data;
  gboolean next = GPOINTER_TO_INT (g_object_get_data (G_OBJECT (button), "myna-page-next"));

  handle_candidate_page (self, next);
}

static void
handle_candidate_page (MynaApp *self, gboolean next)
{
  self->candidate_source = CANDIDATE_SOURCE_PHYSICAL;
  handle_action (self, myna_keyboard_page_candidates (self->keyboard, next));
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
    gtk_widget_set_can_focus (alt, FALSE);
    gtk_widget_set_focus_on_click (alt, FALSE);
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

static const char *
icon_resource_for_action (const char *action_label)
{
  if (g_strcmp0 (action_label, "Backspace") == 0)
    return "/mobi/phosh/MynaOsk/icons/key-backspace.svg";
  if (g_strcmp0 (action_label, "Enter") == 0)
    return "/mobi/phosh/MynaOsk/icons/key-enter.svg";
  if (g_strcmp0 (action_label, "Shift") == 0)
    return "/mobi/phosh/MynaOsk/icons/key-shift.svg";

  return NULL;
}

static void
add_action_style_class (GtkStyleContext *context, const char *action_label)
{
  if (g_strcmp0 (action_label, "Backspace") == 0)
    gtk_style_context_add_class (context, "backspace");
  else if (g_strcmp0 (action_label, "Enter") == 0)
    gtk_style_context_add_class (context, "return-key");
  else if (g_strcmp0 (action_label, "Shift") == 0)
    gtk_style_context_add_class (context, "shift-key");
  else if (g_strcmp0 (action_label, "Mode") == 0)
    gtk_style_context_add_class (context, "mode-key");
  else if (g_strcmp0 (action_label, "Symbols") == 0 ||
           g_strcmp0 (action_label, "MoreSymbols") == 0 ||
           g_strcmp0 (action_label, "Main") == 0 ||
           g_strcmp0 (action_label, "Numbers") == 0)
    gtk_style_context_add_class (context, "switch-key");
}

static GtkWidget *
make_button (const char *label, const char *action_label, const char *style_class)
{
  const char *icon_resource = icon_resource_for_action (action_label);
  GtkWidget *button = icon_resource ? gtk_button_new () : gtk_button_new_with_label (display_label_for_action (label, action_label));
  GtkStyleContext *context = gtk_widget_get_style_context (button);

  if (icon_resource) {
    GtkWidget *image = gtk_image_new_from_resource (icon_resource);
    gtk_image_set_pixel_size (GTK_IMAGE (image), 22);
    gtk_button_set_image (GTK_BUTTON (button), image);
    gtk_button_set_always_show_image (GTK_BUTTON (button), TRUE);
  }

  gtk_widget_set_hexpand (button, TRUE);
  gtk_widget_set_vexpand (button, TRUE);
  gtk_widget_set_can_focus (button, FALSE);
  gtk_widget_set_focus_on_click (button, FALSE);
  gtk_widget_set_size_request (button, 1, 1);
  gtk_style_context_add_class (context, "key");
  if (style_class && *style_class) {
    char **classes = g_strsplit (style_class, " ", -1);
    for (guint i = 0; classes[i]; i++) {
      if (classes[i][0] != '\0')
        gtk_style_context_add_class (context, classes[i]);
    }
    g_strfreev (classes);
  }
  add_action_style_class (context, action_label);
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
add_snapshot_key (MynaApp *self, GtkFixed *fixed, MynaLayoutKey key)
{
  GtkWidget *button;
  PlacedKey *placed;

  if (!key.label || !key.action)
    return;

  button = create_key_button (self, key.label, key.action, key.class_name);
  placed = g_new0 (PlacedKey, 1);

  placed->button = button;
  placed->x = key.x;
  placed->y = key.y;
  placed->width = key.width;
  placed->height = key.height;
  g_ptr_array_add (self->placed_keys, placed);
  gtk_fixed_put (fixed, button, (int) key.x, (int) key.y);

  if (g_strcmp0 (key.action, "Mode") == 0) {
    self->mode_button = button;
    gtk_style_context_add_class (gtk_widget_get_style_context (button), "action");
  }
}

static gboolean
build_keyboard_from_snapshot (MynaApp *self,
                              GtkFixed *fixed,
                              MynaLayoutSnapshot *snapshot)
{
  MynaLayoutSize size;
  size_t count;

  if (!snapshot)
    return FALSE;

  size = myna_layout_size (snapshot);
  count = myna_layout_key_count (snapshot);
  if (size.width <= 0.0 || size.height <= 0.0 || count == 0)
    return FALSE;

  for (size_t i = 0; i < count; i++)
    add_snapshot_key (self, fixed, myna_layout_key_at (snapshot, i));

  return TRUE;
}

static void
layout_keyboard_buttons (MynaApp *self, GtkAllocation *allocation)
{
  MynaLayoutTransform transform;

  if (!self->placed_keys || !self->layout_snapshot)
    return;
  if (allocation->width < 32 || allocation->height < 32)
    return;

  transform = myna_layout_transform (self->layout_snapshot, allocation->width, allocation->height);
  if (transform.scale_x <= 0.0 || transform.scale_y <= 0.0)
    return;

  for (guint i = 0; i < self->placed_keys->len; i++) {
    PlacedKey *key = g_ptr_array_index (self->placed_keys, i);
    int x = (int) (transform.origin_x + key->x * transform.scale_x);
    int y = (int) (transform.origin_y + key->y * transform.scale_y);
    int width = MAX (1, (int) (key->width * transform.scale_x));
    int height = MAX (1, (int) (key->height * transform.scale_y));

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
  if ((allocation.width < 32 || allocation.height < 32) && self->keyboard_area)
    gtk_widget_get_allocation (self->keyboard_area, &allocation);
  if ((allocation.width < 32 || allocation.height < 32) && self->window) {
    GtkAllocation window_allocation;
    gtk_widget_get_allocation (self->window, &window_allocation);
    allocation.width = window_allocation.width;
    allocation.height = CLAMP (window_allocation.height - 58, 206, 264);
  }
  layout_keyboard_buttons (self, &allocation);
  gtk_widget_queue_draw (self->keyboard_fixed);

  return G_SOURCE_REMOVE;
}

static void
build_keyboard (MynaApp *self)
{
  GtkWidget *fixed = gtk_fixed_new ();
  MynaLayoutPage page = MYNA_LAYOUT_PAGE_MAIN;
  MynaLayoutSnapshot *snapshot;

  self->mode_button = NULL;
  g_clear_pointer (&self->placed_keys, g_ptr_array_unref);
  self->placed_keys = g_ptr_array_new_with_free_func (placed_key_free);
  g_clear_pointer (&self->layout_snapshot, myna_layout_snapshot_free);

  gtk_widget_set_hexpand (fixed, TRUE);
  gtk_widget_set_vexpand (fixed, TRUE);

  if (self->page == LAYOUT_SYMBOLS)
    page = MYNA_LAYOUT_PAGE_SYMBOLS;
  else if (self->page == LAYOUT_NUMBERS)
    page = MYNA_LAYOUT_PAGE_NUMBERS;
  else if (self->page == LAYOUT_MORE_SYMBOLS)
    page = MYNA_LAYOUT_PAGE_MORE_SYMBOLS;

  snapshot = myna_layout_page_for_keyboard_new (page, self->keyboard);
  if (!build_keyboard_from_snapshot (self, GTK_FIXED (fixed), snapshot))
    g_warning ("Failed to build keyboard from Rust layout snapshot");
  self->layout_snapshot = snapshot;

  self->keyboard_fixed = fixed;
}

static void
rebuild_keyboard (MynaApp *self)
{
  if (self->keyboard_fixed) {
    GtkWidget *old_keyboard = self->keyboard_fixed;
    self->keyboard_fixed = NULL;
    gtk_container_remove (GTK_CONTAINER (self->keyboard_area), old_keyboard);
  }

  build_keyboard (self);
  g_signal_connect (self->keyboard_fixed, "size-allocate", G_CALLBACK (on_keyboard_size_allocate), self);
  gtk_stack_add_named (GTK_STACK (self->keyboard_area), self->keyboard_fixed, "keyboard");
  gtk_widget_queue_resize (self->keyboard_fixed);
  gtk_widget_show_all (self->keyboard_fixed);
  gtk_stack_set_visible_child_name (GTK_STACK (self->keyboard_area), self->candidate_panel_open ? "candidates" : "keyboard");
  relayout_keyboard_idle (self);
  g_idle_add (relayout_keyboard_idle, self);
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

    gtk_widget_set_can_focus (button, FALSE);
    gtk_widget_set_focus_on_click (button, FALSE);
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
  if (!self->candidate_available) {
    self->candidate_panel_open = FALSE;
    self->candidate_source = CANDIDATE_SOURCE_TOUCH;
  }

  gboolean show_page_buttons =
      self->candidate_source == CANDIDATE_SOURCE_PHYSICAL &&
      self->candidate_available &&
      (view->can_page_previous || view->can_page_next);
  gtk_widget_set_visible (self->candidate_prev_button, show_page_buttons);
  gtk_widget_set_visible (self->candidate_next_button, show_page_buttons);
  gtk_widget_set_sensitive (self->candidate_prev_button, view->can_page_previous);
  gtk_widget_set_sensitive (self->candidate_next_button, view->can_page_next);

  gtk_button_set_label (
      GTK_BUTTON (self->candidate_toggle_button),
      self->candidate_panel_open ? "‹" : (self->candidate_available ? "›" : "⌄"));
  gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (self->candidate_scroller), GTK_POLICY_AUTOMATIC, GTK_POLICY_NEVER);
  gtk_widget_set_size_request (self->candidate_scroller, 1, 38);

  size_t visible_candidates = has_preedit ? view->candidate_count : 0;
  for (size_t i = 0; i < visible_candidates; i++) {
    GtkWidget *button = gtk_button_new_with_label (view->candidates[i]);
    int column = (int) i;

    gtk_widget_set_can_focus (button, FALSE);
    gtk_widget_set_focus_on_click (button, FALSE);
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

  if (visible) {
    gtk_widget_show_all (self->window);
    refresh_view (self);
  } else {
    self->candidate_panel_open = FALSE;
    self->candidate_source = CANDIDATE_SOURCE_TOUCH;
    handle_action (self, myna_keyboard_clear_composition (self->keyboard));
    gtk_widget_hide (self->window);
  }

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

static gboolean
register_dbus_object (MynaApp *self)
{
  GError *error = NULL;

  if (!self->bus)
    return FALSE;
  if (self->registration_id)
    return TRUE;

  self->introspection = g_dbus_node_info_new_for_xml (introspection_xml, &error);
  if (error) {
    g_warning ("D-Bus introspection failed: %s", error->message);
    g_clear_error (&error);
    return FALSE;
  }

  self->registration_id = g_dbus_connection_register_object (
      self->bus,
      OSK_OBJECT_PATH,
      self->introspection->interfaces[0],
      &dbus_vtable,
      self,
      NULL,
      &error);

  if (error) {
    g_warning ("D-Bus object registration failed: %s", error->message);
    g_clear_error (&error);
    return FALSE;
  }

  g_message ("Registered %s on %s", OSK_BUS_NAME, OSK_OBJECT_PATH);
  return TRUE;
}

static void
on_name_acquired (GDBusConnection *connection, const char *name, gpointer user_data)
{
  MynaApp *self = user_data;
  (void) connection;
  (void) name;

  self->bus_name_acquired = TRUE;
  self->bus_name_lost = FALSE;
  if (self->activation_pending && self->app) {
    self->activation_pending = FALSE;
    g_application_activate (G_APPLICATION (self->app));
  }
}

static void
on_name_lost (GDBusConnection *connection, const char *name, gpointer user_data)
{
  MynaApp *self = user_data;
  (void) connection;

  g_warning ("Failed to own %s; another OSK instance is likely running", name);
  self->bus_name_lost = TRUE;
  if (self->app)
    g_application_quit (G_APPLICATION (self->app));
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
  if (self->session_proxy)
    return;

  g_message ("Registering gnome-session client name=%s autostart_id=%s",
             OSK_BUS_NAME,
             startup_id ? startup_id : "");

  self->session_proxy = g_dbus_proxy_new_for_bus_sync (
      G_BUS_TYPE_SESSION,
      G_DBUS_PROXY_FLAGS_DO_NOT_AUTO_START,
      NULL,
      "org.gnome.SessionManager",
      "/org/gnome/SessionManager",
      "org.gnome.SessionManager",
      NULL,
      &error);

  if (error) {
    g_warning ("gnome-session proxy failed: %s", error->message);
    g_clear_error (&error);
    return;
  }

  reply = g_dbus_proxy_call_sync (
      self->session_proxy,
      "RegisterClient",
      g_variant_new ("(ss)", OSK_BUS_NAME, startup_id ? startup_id : ""),
      G_DBUS_CALL_FLAGS_NONE,
      1000,
      NULL,
      &error);

  if (error) {
    g_warning ("gnome-session registration failed: %s", error->message);
    g_clear_error (&error);
    g_clear_object (&self->session_proxy);
    return;
  }

  g_variant_get (reply, "(o)", &self->session_client_id);
  g_variant_unref (reply);
  g_message ("Registered gnome-session client: %s", self->session_client_id);

  self->session_client_proxy = g_dbus_proxy_new_for_bus_sync (
      G_BUS_TYPE_SESSION,
      G_DBUS_PROXY_FLAGS_NONE,
      NULL,
      "org.gnome.SessionManager",
      self->session_client_id,
      "org.gnome.SessionManager.ClientPrivate",
      NULL,
      &error);

  if (error) {
    g_warning ("gnome-session client proxy failed: %s", error->message);
    g_clear_error (&error);
    return;
  }

  g_signal_connect (self->session_client_proxy,
                    "g-signal",
                    G_CALLBACK (on_session_client_signal),
                    self);
}

static void
send_end_session_response (MynaApp *self)
{
  if (!self->session_client_proxy)
    return;

  g_dbus_proxy_call (
      self->session_client_proxy,
      "EndSessionResponse",
      g_variant_new ("(bs)", TRUE, ""),
      G_DBUS_CALL_FLAGS_NONE,
      G_MAXINT,
      NULL,
      NULL,
      NULL);
}

static void
unregister_gnome_session_client (MynaApp *self)
{
  GError *error = NULL;

  if (self->session_proxy && self->session_client_id) {
    g_dbus_proxy_call_sync (
        self->session_proxy,
        "UnregisterClient",
        g_variant_new ("(o)", self->session_client_id),
        G_DBUS_CALL_FLAGS_NONE,
        G_MAXINT,
        NULL,
        &error);

    if (error) {
      g_warning ("gnome-session unregister failed: %s", error->message);
      g_clear_error (&error);
    }
  }

  g_clear_object (&self->session_client_proxy);
  g_clear_pointer (&self->session_client_id, g_free);
}

static void
on_session_client_signal (GDBusProxy *proxy,
                          const char *sender_name,
                          const char *signal_name,
                          GVariant *parameters,
                          gpointer user_data)
{
  MynaApp *self = user_data;
  (void) proxy;
  (void) sender_name;
  (void) parameters;

  if (g_strcmp0 (signal_name, "QueryEndSession") == 0) {
    send_end_session_response (self);
  } else if (g_strcmp0 (signal_name, "EndSession") == 0 ||
             g_strcmp0 (signal_name, "Stop") == 0) {
    send_end_session_response (self);
    unregister_gnome_session_client (self);
    if (self->app)
      g_application_quit (G_APPLICATION (self->app));
  }
}

static void
install_css (void)
{
  static const char css[] =
    "window { background: #18181b; }"
    ".root { padding: 7px 8px 8px 8px; }"
    ".candidate-strip { min-height: 38px; padding: 0 1px 2px 1px; }"
    ".preedit { color: #f3f4f6; font-size: 15px; padding: 0 8px; min-width: 52px; font-weight: 600; }"
    ".candidate { border-radius: 4px; margin: 0 2px; padding: 5px 9px; font-size: 18px; background: transparent; color: #f4f5f7; border: 0; box-shadow: none; }"
    ".candidate:active { background: rgba(255,255,255,0.12); }"
    ".candidate-primary { background: #f0f2f5; color: #202124; font-weight: 700; }"
    ".candidate-toggle { min-width: 34px; color: #e5e7eb; padding-left: 6px; padding-right: 6px; }"
    ".candidate-page { min-width: 30px; color: #e5e7eb; padding-left: 5px; padding-right: 5px; }"
    ".candidate-page:disabled { opacity: 0.38; }"
    ".hide-key { min-width: 34px; color: #e5e7eb; }"
    ".candidate-cell { border-radius: 4px; margin: 2px; padding: 6px 8px; font-size: 18px; min-height: 42px; background: #f0f2f5; color: #202124; border: 0; }"
    ".candidate-cell:active { background: #dfe3e8; }"
    ".candidate-small { font-size: 15px; }"
    ".candidate-tiny { font-size: 13px; }"
    ".key { border-radius: 4px; margin: 2px; padding: 0; font-family: Cantarell, 'Noto Sans CJK SC', 'Noto Sans Symbols 2', 'Noto Sans Symbols', sans-serif; font-size: 18px; font-weight: 500; min-height: 0; background: #4b4a50; color: #f1f1f4; border: 0; box-shadow: 0 1px 0 rgba(0,0,0,0.55); text-shadow: none; }"
    ".key:active { background: #68656d; }"
    ".action { background: #2f2d34; color: #f5f5f6; font-size: 15px; font-weight: 600; }"
    ".action:active { background: #504d57; }"
    ".active { background: #6d6878; color: #ffffff; }"
    ".active:active { background: #7c7688; color: #ffffff; }"
    ".return-key { background: #1c71d8; color: #ffffff; font-size: 14px; font-weight: 700; }"
    ".return-key:active { background: #3584e4; }"
    ".switch-key { font-size: 14px; letter-spacing: 0; }"
    ".backspace, .shift-key { font-size: 20px; }"
    ".alternate { border-radius: 4px; min-width: 40px; min-height: 36px; font-size: 18px; }"
    ".space { font-size: 14px; color: #e8e8ec; }"
    ".mode { font-weight: 700; color: #f8fafc; }"
    ".mode-key { background: #393741; }";
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
  int keyboard_height;

  self->app = app;
  if (!self->bus_name_acquired) {
    self->activation_pending = TRUE;
    return;
  }
  register_gnome_session_client (self);
  if (self->started) {
    if (self->show_on_start)
      set_visible (self, TRUE);
    return;
  }

  self->started = TRUE;
  self->keyboard = myna_keyboard_new ();

  myna_wayland_set_visibility_callback (&self->wayland, on_wayland_visibility_requested, self);
  myna_wayland_set_physical_key_callback (&self->wayland, on_wayland_physical_key, self);
  if (!myna_wayland_init (&self->wayland))
    g_warning ("Wayland setup was incomplete");

  install_css ();
  height = panel_height_for_screen ();
  keyboard_height = CLAMP (height - 58, 206, 264);
  self->window = myna_layer_surface_new (self->wayland.layer_shell, self->wayland.output, height);
  gtk_application_add_window (app, GTK_WINDOW (self->window));

  root = gtk_box_new (GTK_ORIENTATION_VERTICAL, 5);
  gtk_style_context_add_class (gtk_widget_get_style_context (root), "root");
  gtk_container_add (GTK_CONTAINER (self->window), root);

  candidate_row = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 5);
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

  self->candidate_prev_button = gtk_button_new_with_label ("‹");
  gtk_widget_set_can_focus (self->candidate_prev_button, FALSE);
  gtk_widget_set_focus_on_click (self->candidate_prev_button, FALSE);
  gtk_widget_set_no_show_all (self->candidate_prev_button, TRUE);
  gtk_style_context_add_class (gtk_widget_get_style_context (self->candidate_prev_button), "candidate");
  gtk_style_context_add_class (gtk_widget_get_style_context (self->candidate_prev_button), "candidate-page");
  g_object_set_data (G_OBJECT (self->candidate_prev_button), "myna-page-next", GINT_TO_POINTER (FALSE));
  g_signal_connect (self->candidate_prev_button, "clicked", G_CALLBACK (on_candidate_page_clicked), self);
  gtk_box_pack_start (GTK_BOX (candidate_row), self->candidate_prev_button, FALSE, FALSE, 0);

  self->candidate_next_button = gtk_button_new_with_label ("›");
  gtk_widget_set_can_focus (self->candidate_next_button, FALSE);
  gtk_widget_set_focus_on_click (self->candidate_next_button, FALSE);
  gtk_widget_set_no_show_all (self->candidate_next_button, TRUE);
  gtk_style_context_add_class (gtk_widget_get_style_context (self->candidate_next_button), "candidate");
  gtk_style_context_add_class (gtk_widget_get_style_context (self->candidate_next_button), "candidate-page");
  g_object_set_data (G_OBJECT (self->candidate_next_button), "myna-page-next", GINT_TO_POINTER (TRUE));
  g_signal_connect (self->candidate_next_button, "clicked", G_CALLBACK (on_candidate_page_clicked), self);
  gtk_box_pack_start (GTK_BOX (candidate_row), self->candidate_next_button, FALSE, FALSE, 0);

  self->candidate_toggle_button = gtk_button_new_with_label ("⌄");
  gtk_widget_set_can_focus (self->candidate_toggle_button, FALSE);
  gtk_widget_set_focus_on_click (self->candidate_toggle_button, FALSE);
  gtk_style_context_add_class (gtk_widget_get_style_context (self->candidate_toggle_button), "candidate");
  gtk_style_context_add_class (gtk_widget_get_style_context (self->candidate_toggle_button), "candidate-toggle");
  g_signal_connect (self->candidate_toggle_button, "clicked", G_CALLBACK (on_candidates_toggle_clicked), self);
  gtk_box_pack_start (GTK_BOX (candidate_row), self->candidate_toggle_button, FALSE, FALSE, 0);

  self->candidate_box = gtk_grid_new ();
  gtk_grid_set_row_spacing (GTK_GRID (self->candidate_box), 4);
  gtk_grid_set_column_spacing (GTK_GRID (self->candidate_box), 4);
  gtk_widget_set_hexpand (self->candidate_box, TRUE);
  gtk_container_add (GTK_CONTAINER (self->candidate_scroller), self->candidate_box);

  build_keyboard (self);
  g_signal_connect (self->keyboard_fixed, "size-allocate", G_CALLBACK (on_keyboard_size_allocate), self);
  self->keyboard_area = gtk_stack_new ();
  gtk_stack_set_transition_type (GTK_STACK (self->keyboard_area), GTK_STACK_TRANSITION_TYPE_NONE);
  gtk_widget_set_hexpand (self->keyboard_area, TRUE);
  gtk_widget_set_vexpand (self->keyboard_area, FALSE);
  gtk_widget_set_size_request (self->keyboard_area, 1, keyboard_height);
  gtk_box_pack_start (GTK_BOX (root), self->keyboard_area, TRUE, TRUE, 0);
  gtk_stack_add_named (GTK_STACK (self->keyboard_area), self->keyboard_fixed, "keyboard");

  self->candidate_panel = gtk_grid_new ();
  gtk_grid_set_row_spacing (GTK_GRID (self->candidate_panel), 5);
  gtk_grid_set_column_spacing (GTK_GRID (self->candidate_panel), 5);
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
  GError *error = NULL;
  int status;
  guint bus_id;

  if (should_deploy_rime (argc, argv)) {
    if (!myna_rime_deploy_full ()) {
      g_warning ("Rime full deployment failed");
      return 1;
    }
    g_message ("Rime full deployment finished; check Rime logs for schema warnings");
    return 0;
  }

  if (!gtk_init_check (&argc, &argv)) {
    g_warning ("No display available; exiting without registering OSK");
    return 0;
  }

  app = gtk_application_new (APP_ID, G_APPLICATION_NON_UNIQUE | G_APPLICATION_HANDLES_COMMAND_LINE);
  self.app = app;
  g_application_hold (G_APPLICATION (app));
  g_message ("Starting Myna OSK app_id=%s bus_name=%s", APP_ID, OSK_BUS_NAME);
  self.bus = g_bus_get_sync (G_BUS_TYPE_SESSION, NULL, &error);
  if (error) {
    g_warning ("Failed to connect to session bus: %s", error->message);
    g_clear_error (&error);
  }

  if (self.bus)
    register_dbus_object (&self);

  bus_id = self.bus ? g_bus_own_name_on_connection (
      self.bus,
      OSK_BUS_NAME,
      G_BUS_NAME_OWNER_FLAGS_DO_NOT_QUEUE,
      on_name_acquired,
      on_name_lost,
      &self,
      NULL) : 0;

  while (bus_id && !self.bus_name_acquired && !self.bus_name_lost)
    g_main_context_iteration (NULL, TRUE);

  if (self.bus_name_acquired)
    register_gnome_session_client (&self);

  g_signal_connect (app, "activate", G_CALLBACK (activate), &self);
  g_signal_connect (app, "command-line", G_CALLBACK (command_line), &self);
  status = g_application_run (G_APPLICATION (app), argc, argv);

  if (self.registration_id && self.bus)
    g_dbus_connection_unregister_object (self.bus, self.registration_id);
  unregister_gnome_session_client (&self);
  g_clear_object (&self.session_proxy);
  g_clear_object (&self.bus);
  g_clear_pointer (&self.introspection, g_dbus_node_info_unref);
  g_clear_pointer (&self.layout_snapshot, myna_layout_snapshot_free);
  g_clear_pointer (&self.placed_keys, g_ptr_array_unref);
  if (bus_id)
    g_bus_unown_name (bus_id);
  g_application_release (G_APPLICATION (app));
  myna_keyboard_free (self.keyboard);
  myna_wayland_destroy (&self.wayland);
  g_object_unref (app);

  return status;
}
