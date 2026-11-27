// SPDX-License-Identifier: GPL-3.0-or-later

#include <gtk/gtk.h>
#include <locale.h>

#include "settings.h"

typedef struct {
  GSettings *settings;
  GtkWidget *mode;
  GtkWidget *height;
} SettingsWindow;

static gboolean
use_chinese (void)
{
  const char * const *languages = g_get_language_names ();

  return g_str_has_prefix (languages[0], "zh");
}

static const char *
label_text (const char *english, const char *chinese)
{
  return use_chinese () ? chinese : english;
}

static void
sync_controls (SettingsWindow *window)
{
  char *mode = g_settings_get_string (window->settings, "default-mode");
  char *height = g_settings_get_string (window->settings, "keyboard-height");

  gtk_combo_box_set_active_id (GTK_COMBO_BOX (window->mode), mode);
  gtk_combo_box_set_active_id (GTK_COMBO_BOX (window->height), height);
  g_free (mode);
  g_free (height);
}

static void
on_settings_changed (GSettings *settings, const char *key, gpointer user_data)
{
  (void) settings;
  (void) key;
  sync_controls (user_data);
}

static void
on_combo_changed (GtkComboBox *combo, gpointer user_data)
{
  SettingsWindow *window = user_data;
  const char *key = g_object_get_data (G_OBJECT (combo), "settings-key");
  const char *value = gtk_combo_box_get_active_id (combo);

  if (value)
    g_settings_set_string (window->settings, key, value);
}

static void
on_reset_clicked (GtkButton *button, gpointer user_data)
{
  SettingsWindow *window = user_data;
  (void) button;

  g_settings_reset (window->settings, "default-mode");
  g_settings_reset (window->settings, "keyboard-height");
}

static GtkWidget *
add_setting (GtkWidget *box, const char *title, const char *description)
{
  GtkWidget *group = gtk_box_new (GTK_ORIENTATION_VERTICAL, 6);
  GtkWidget *label = gtk_label_new (title);
  GtkWidget *hint = gtk_label_new (description);
  GtkWidget *combo = gtk_combo_box_text_new ();

  gtk_label_set_xalign (GTK_LABEL (label), 0.0);
  gtk_style_context_add_class (gtk_widget_get_style_context (label), "setting-title");
  gtk_label_set_xalign (GTK_LABEL (hint), 0.0);
  gtk_label_set_line_wrap (GTK_LABEL (hint), TRUE);
  gtk_style_context_add_class (gtk_widget_get_style_context (hint), "dim-label");
  gtk_box_pack_start (GTK_BOX (group), label, FALSE, FALSE, 0);
  gtk_box_pack_start (GTK_BOX (group), hint, FALSE, FALSE, 0);
  gtk_box_pack_start (GTK_BOX (group), combo, FALSE, FALSE, 4);
  gtk_box_pack_start (GTK_BOX (box), group, FALSE, FALSE, 0);
  return combo;
}

static void
settings_window_free (gpointer data)
{
  SettingsWindow *window = data;

  if (window->settings)
    g_signal_handlers_disconnect_by_data (window->settings, window);
  g_clear_object (&window->settings);
  g_free (window);
}

static void
activate (GtkApplication *app, gpointer user_data)
{
  SettingsWindow *state;
  GtkWidget *window;
  GtkWidget *header;
  GtkWidget *scroll;
  GtkWidget *box;
  GtkWidget *label;
  GtkWidget *entry;
  GtkWidget *reset;
  GtkCssProvider *css;
  (void) user_data;

  window = gtk_application_get_active_window (app);
  if (window) {
    gtk_window_present (GTK_WINDOW (window));
    return;
  }

  state = g_new0 (SettingsWindow, 1);
  state->settings = myna_settings_new ();
  window = gtk_application_window_new (app);
  g_object_set_data_full (G_OBJECT (window), "settings-window", state, settings_window_free);
  gtk_window_set_title (GTK_WINDOW (window), label_text ("Myna Settings", "Myna 设置"));
  gtk_window_set_default_size (GTK_WINDOW (window), 360, 580);
  gtk_window_set_icon_name (GTK_WINDOW (window), "input-keyboard-symbolic");

  header = gtk_header_bar_new ();
  gtk_header_bar_set_title (GTK_HEADER_BAR (header), label_text ("Myna Settings", "Myna 设置"));
  gtk_header_bar_set_show_close_button (GTK_HEADER_BAR (header), TRUE);
  gtk_window_set_titlebar (GTK_WINDOW (window), header);

  scroll = gtk_scrolled_window_new (NULL, NULL);
  gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  gtk_container_add (GTK_CONTAINER (window), scroll);
  box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 20);
  gtk_container_set_border_width (GTK_CONTAINER (box), 20);
  gtk_widget_set_halign (box, GTK_ALIGN_FILL);
  gtk_container_add (GTK_CONTAINER (scroll), box);

  if (!state->settings) {
    label = gtk_label_new (label_text ("Myna settings are unavailable. Install the application to use its settings.",
                                       "Myna 设置不可用。请安装应用后使用设置。"));
    gtk_label_set_line_wrap (GTK_LABEL (label), TRUE);
    gtk_box_pack_start (GTK_BOX (box), label, FALSE, FALSE, 0);
    gtk_widget_show_all (window);
    return;
  }

  state->mode = add_setting (box, label_text ("Default input mode", "默认输入模式"),
                             label_text ("Applies when Myna OSK starts.", "在 Myna 键盘启动时生效。"));
  gtk_combo_box_text_append (GTK_COMBO_BOX_TEXT (state->mode), "pinyin", label_text ("Pinyin", "拼音"));
  gtk_combo_box_text_append (GTK_COMBO_BOX_TEXT (state->mode), "latin", label_text ("English", "英文"));
  g_object_set_data (G_OBJECT (state->mode), "settings-key", "default-mode");

  state->height = add_setting (box, label_text ("Keyboard height", "键盘高度"),
                               label_text ("Applies the next time the keyboard opens.", "在键盘下次弹出时生效。"));
  gtk_combo_box_text_append (GTK_COMBO_BOX_TEXT (state->height), "auto", label_text ("Automatic", "自动"));
  gtk_combo_box_text_append (GTK_COMBO_BOX_TEXT (state->height), "compact", label_text ("Compact", "紧凑"));
  gtk_combo_box_text_append (GTK_COMBO_BOX_TEXT (state->height), "tall", label_text ("Tall", "较高"));
  g_object_set_data (G_OBJECT (state->height), "settings-key", "keyboard-height");
  sync_controls (state);
  g_signal_connect (state->mode, "changed", G_CALLBACK (on_combo_changed), state);
  g_signal_connect (state->height, "changed", G_CALLBACK (on_combo_changed), state);
  g_signal_connect (state->settings, "changed", G_CALLBACK (on_settings_changed), state);

  label = gtk_label_new (label_text ("Try typing", "试打"));
  gtk_label_set_xalign (GTK_LABEL (label), 0.0);
  gtk_style_context_add_class (gtk_widget_get_style_context (label), "setting-title");
  gtk_box_pack_start (GTK_BOX (box), label, FALSE, FALSE, 0);
  entry = gtk_entry_new ();
  gtk_entry_set_placeholder_text (GTK_ENTRY (entry), label_text ("Type here to try the keyboard", "在此输入以试用键盘"));
  gtk_box_pack_start (GTK_BOX (box), entry, FALSE, FALSE, 0);

  reset = gtk_button_new_with_label (label_text ("Restore defaults", "恢复默认设置"));
  g_signal_connect (reset, "clicked", G_CALLBACK (on_reset_clicked), state);
  gtk_box_pack_start (GTK_BOX (box), reset, FALSE, FALSE, 0);

  css = gtk_css_provider_new ();
  gtk_css_provider_load_from_data (css, ".setting-title { font-weight: bold; font-size: 16px; }", -1, NULL);
  gtk_style_context_add_provider_for_screen (gdk_screen_get_default (), GTK_STYLE_PROVIDER (css),
                                              GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
  g_object_unref (css);
  gtk_widget_show_all (window);
}

int
main (int argc, char **argv)
{
  GtkApplication *app;
  int status;

  setlocale (LC_ALL, "");
  app = gtk_application_new ("mobi.phosh.MynaOsk.Settings", G_APPLICATION_FLAGS_NONE);
  g_signal_connect (app, "activate", G_CALLBACK (activate), NULL);
  status = g_application_run (G_APPLICATION (app), argc, argv);
  g_object_unref (app);
  return status;
}
