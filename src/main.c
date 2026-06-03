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

typedef struct {
  const char *label;
  const char *action;
  int width;
  const char *style_class;
} KeySpec;

typedef struct {
  GtkApplication *app;
  MynaWayland wayland;
  Keyboard *keyboard;
  GtkWidget *window;
  GtkWidget *candidate_box;
  GtkWidget *preedit_label;
  GtkWidget *mode_button;
  GtkWidget *main_grid;
  GtkWidget *keyboard_area;
  GDBusNodeInfo *introspection;
  GDBusConnection *bus;
  guint registration_id;
  gboolean visible;
  gboolean started;
  gboolean show_on_start;
  LayoutPage page;
  char *session_client_id;
} MynaApp;

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

static int
panel_height_for_screen (void)
{
  GdkDisplay *display = gdk_display_get_default ();
  GdkMonitor *monitor = gdk_display_get_primary_monitor (display);
  GdkRectangle geometry;

  if (!monitor)
    return 280;

  gdk_monitor_get_geometry (monitor, &geometry);

  if (geometry.height <= 720)
    return MAX (220, geometry.height * 36 / 100);

  return CLAMP (geometry.height * 30 / 100, 260, 360);
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
on_mode_clicked (GtkButton *button, gpointer user_data)
{
  MynaApp *self = user_data;
  (void) button;

  handle_action (self, myna_keyboard_toggle_mode (self->keyboard));
}

static void
on_candidate_clicked (GtkButton *button, gpointer user_data)
{
  MynaApp *self = user_data;
  size_t index = GPOINTER_TO_SIZE (g_object_get_data (G_OBJECT (button), "myna-index"));

  handle_action (self, myna_keyboard_select_candidate (self->keyboard, index));
}

static void
on_hide_clicked (GtkButton *button, gpointer user_data)
{
  MynaApp *self = user_data;
  (void) button;

  set_visible (self, FALSE);
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

static void
attach_key (GtkGrid *grid,
            MynaApp *self,
            const char *label,
            const char *action_label,
            int column,
            int row,
            int width,
            const char *style_class)
{
  GtkWidget *button = make_button (label, action_label, style_class);

  g_signal_connect (button, "clicked", G_CALLBACK (on_key_clicked), self);
  gtk_grid_attach (grid, button, column, row, width, 1);
}

static void
attach_row (GtkGrid *grid, MynaApp *self, const KeySpec *keys, guint n_keys, int row, int offset)
{
  int column = offset;

  for (guint i = 0; i < n_keys; i++) {
    attach_key (grid, self, keys[i].label, keys[i].action, column, row, keys[i].width, keys[i].style_class);
    column += keys[i].width;
  }
}

static void
build_keyboard (MynaApp *self)
{
  GtkWidget *grid = gtk_grid_new ();
  self->mode_button = NULL;

  gtk_grid_set_row_spacing (GTK_GRID (grid), 4);
  gtk_grid_set_column_spacing (GTK_GRID (grid), 4);
  gtk_widget_set_hexpand (grid, TRUE);
  gtk_widget_set_vexpand (grid, TRUE);

  if (self->page == LAYOUT_SYMBOLS) {
    static const KeySpec row1[] = {
      { "1", "1", 3, NULL }, { "2", "2", 3, NULL }, { "3", "3", 3, NULL }, { "4", "4", 3, NULL }, { "5", "5", 3, NULL },
      { "6", "6", 3, NULL }, { "7", "7", 3, NULL }, { "8", "8", 3, NULL }, { "9", "9", 3, NULL }, { "0", "0", 3, NULL },
    };
    static const KeySpec row2[] = {
      { "@", "@", 3, NULL }, { "#", "#", 3, NULL }, { "$", "$", 3, NULL }, { "_", "_", 3, NULL }, { "&", "&", 3, NULL },
      { "-", "-", 3, NULL }, { "+", "+", 3, NULL }, { "(", "(", 3, NULL }, { ")", ")", 3, NULL },
    };
    static const KeySpec row3[] = {
      { "123", "Numbers", 4, "action" }, { "*", "*", 3, NULL }, { "\"", "\"", 3, NULL }, { "'", "'", 3, NULL },
      { ":", ":", 3, NULL }, { ";", ";", 3, NULL }, { "!", "!", 3, NULL }, { "?", "?", 3, NULL }, { "⌫", "Backspace", 5, "action" },
    };
    static const KeySpec row4[] = {
      { "ABC", "Main", 5, "action" }, { ",", ",", 3, NULL }, { "space", "Space", 14, "space" }, { ".", ".", 3, NULL }, { "return", "Enter", 5, "action" },
    };

    attach_row (GTK_GRID (grid), self, row1, G_N_ELEMENTS (row1), 0, 0);
    attach_row (GTK_GRID (grid), self, row2, G_N_ELEMENTS (row2), 1, 1);
    attach_row (GTK_GRID (grid), self, row3, G_N_ELEMENTS (row3), 2, 0);
    attach_row (GTK_GRID (grid), self, row4, G_N_ELEMENTS (row4), 3, 0);
  } else if (self->page == LAYOUT_NUMBERS) {
    static const KeySpec row1[] = {
      { "1", "1", 10, NULL }, { "2", "2", 10, NULL }, { "3", "3", 10, NULL },
    };
    static const KeySpec row2[] = {
      { "4", "4", 10, NULL }, { "5", "5", 10, NULL }, { "6", "6", 10, NULL },
    };
    static const KeySpec row3[] = {
      { "7", "7", 10, NULL }, { "8", "8", 10, NULL }, { "9", "9", 10, NULL },
    };
    static const KeySpec row4[] = {
      { "ABC", "Main", 5, "action" }, { "0", "0", 10, NULL }, { "⌫", "Backspace", 5, "action" }, { "return", "Enter", 10, "action" },
    };

    attach_row (GTK_GRID (grid), self, row1, G_N_ELEMENTS (row1), 0, 0);
    attach_row (GTK_GRID (grid), self, row2, G_N_ELEMENTS (row2), 1, 0);
    attach_row (GTK_GRID (grid), self, row3, G_N_ELEMENTS (row3), 2, 0);
    attach_row (GTK_GRID (grid), self, row4, G_N_ELEMENTS (row4), 3, 0);
  } else {
    static const KeySpec row1[] = {
      { "q", "q", 3, NULL }, { "w", "w", 3, NULL }, { "e", "e", 3, NULL }, { "r", "r", 3, NULL }, { "t", "t", 3, NULL },
      { "y", "y", 3, NULL }, { "u", "u", 3, NULL }, { "i", "i", 3, NULL }, { "o", "o", 3, NULL }, { "p", "p", 3, NULL },
    };
    static const KeySpec row2[] = {
      { "a", "a", 3, NULL }, { "s", "s", 3, NULL }, { "d", "d", 3, NULL }, { "f", "f", 3, NULL }, { "g", "g", 3, NULL },
      { "h", "h", 3, NULL }, { "j", "j", 3, NULL }, { "k", "k", 3, NULL }, { "l", "l", 3, NULL },
    };
    static const KeySpec row3[] = {
      { "⇧", "Shift", 4, "action" }, { "z", "z", 3, NULL }, { "x", "x", 3, NULL }, { "c", "c", 3, NULL }, { "v", "v", 3, NULL },
      { "b", "b", 3, NULL }, { "n", "n", 3, NULL }, { "m", "m", 3, NULL }, { "⌫", "Backspace", 5, "action" },
    };
    static const KeySpec row4[] = {
      { "?123", "Symbols", 4, "action" }, { ",", ",", 3, NULL },
      { "space", "Space", 11, "space" }, { ".", ".", 3, NULL }, { "return", "Enter", 5, "action" },
    };

    attach_row (GTK_GRID (grid), self, row1, G_N_ELEMENTS (row1), 0, 0);
    attach_row (GTK_GRID (grid), self, row2, G_N_ELEMENTS (row2), 1, 1);
    attach_row (GTK_GRID (grid), self, row3, G_N_ELEMENTS (row3), 2, 0);

    self->mode_button = make_button ("拼", "Mode", "mode");
    gtk_style_context_add_class (gtk_widget_get_style_context (self->mode_button), "action");
    g_signal_connect (self->mode_button, "clicked", G_CALLBACK (on_mode_clicked), self);
    gtk_grid_attach (GTK_GRID (grid), self->mode_button, 0, 3, 4, 1);
    attach_row (GTK_GRID (grid), self, row4, G_N_ELEMENTS (row4), 3, 4);
  }

  self->main_grid = grid;
}

static void
rebuild_keyboard (MynaApp *self)
{
  if (self->main_grid)
    gtk_widget_destroy (self->main_grid);

  build_keyboard (self);
  gtk_box_pack_start (GTK_BOX (self->keyboard_area), self->main_grid, TRUE, TRUE, 0);
  gtk_widget_show_all (self->main_grid);
}

static void
clear_container (GtkWidget *container)
{
  GList *children = gtk_container_get_children (GTK_CONTAINER (container));

  for (GList *iter = children; iter; iter = iter->next)
    gtk_widget_destroy (GTK_WIDGET (iter->data));

  g_list_free (children);
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

  for (size_t i = 0; i < view->candidate_count; i++) {
    GtkWidget *button = gtk_button_new_with_label (view->candidates[i]);
    gtk_widget_set_hexpand (button, TRUE);
    gtk_style_context_add_class (gtk_widget_get_style_context (button), "candidate");
    if (i == 0)
      gtk_style_context_add_class (gtk_widget_get_style_context (button), "candidate-primary");
    g_object_set_data (G_OBJECT (button), "myna-index", GSIZE_TO_POINTER (i));
    g_signal_connect (button, "clicked", G_CALLBACK (on_candidate_clicked), self);
    gtk_box_pack_start (GTK_BOX (self->candidate_box), button, TRUE, TRUE, 0);
  }

  gtk_widget_show_all (self->candidate_box);
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
    ".hide-key { min-width: 34px; color: #d8d8d8; }"
    ".key { border-radius: 5px; margin: 0; padding: 0; font-size: 18px; min-height: 38px; background: #f2f0ea; color: #1f1f23; border: 0; box-shadow: 0 1px 0 rgba(0,0,0,0.35); }"
    ".key:active { background: #d8d3c8; }"
    ".action { background: #6f737b; color: #ffffff; font-size: 15px; }"
    ".action:active { background: #858a93; }"
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

  self->candidate_box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 2);
  gtk_widget_set_hexpand (self->candidate_box, TRUE);
  gtk_box_pack_start (GTK_BOX (candidate_row), self->candidate_box, TRUE, TRUE, 0);

  GtkWidget *hide_button = gtk_button_new_with_label ("⌄");
  gtk_style_context_add_class (gtk_widget_get_style_context (hide_button), "candidate");
  gtk_style_context_add_class (gtk_widget_get_style_context (hide_button), "hide-key");
  g_signal_connect (hide_button, "clicked", G_CALLBACK (on_hide_clicked), self);
  gtk_box_pack_start (GTK_BOX (candidate_row), hide_button, FALSE, FALSE, 0);

  build_keyboard (self);
  self->keyboard_area = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_set_hexpand (self->keyboard_area, TRUE);
  gtk_widget_set_vexpand (self->keyboard_area, TRUE);
  gtk_box_pack_start (GTK_BOX (root), self->keyboard_area, TRUE, TRUE, 0);
  gtk_box_pack_start (GTK_BOX (self->keyboard_area), self->main_grid, TRUE, TRUE, 0);

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
