#include "bridge.h"

#include <gdk/gdkwayland.h>
#include <glib.h>
#include <glib/gstdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <xkbcommon/xkbcommon.h>

static void
input_activate (void *data, struct zwp_input_method_v2 *input_method)
{
  MynaWayland *self = data;
  if (self->input_method == input_method) {
    self->pending_input_active = true;
    self->has_surrounding_text = false;
    g_debug ("input-method-v2 activate pending");
  }
}

static void
input_deactivate (void *data, struct zwp_input_method_v2 *input_method)
{
  MynaWayland *self = data;
  if (self->input_method == input_method) {
    self->pending_input_active = false;
    self->has_surrounding_text = false;
    g_debug ("input-method-v2 deactivate pending");
  }
}

static void
input_surrounding_text (void *data,
                        struct zwp_input_method_v2 *input_method,
                        const char *text,
                        uint32_t cursor,
                        uint32_t anchor)
{
  MynaWayland *self = data;
  (void) anchor;

  if (self->input_method != input_method)
    return;

  g_free (self->surrounding_text);
  self->surrounding_text = g_strdup (text ? text : "");
  self->surrounding_cursor = cursor;
  self->has_surrounding_text = true;
  g_debug ("input-method-v2 surrounding_text cursor=%u bytes=%zu", cursor, strlen (self->surrounding_text));
}

static void
input_text_change_cause (void *data,
                         struct zwp_input_method_v2 *input_method,
                         uint32_t cause)
{
  (void) data;
  (void) input_method;
  (void) cause;
}

static void
input_content_type (void *data,
                    struct zwp_input_method_v2 *input_method,
                    uint32_t hint,
                    uint32_t purpose)
{
  (void) data;
  (void) input_method;
  (void) hint;
  (void) purpose;
}

static void
input_done (void *data, struct zwp_input_method_v2 *input_method)
{
  MynaWayland *self = data;
  if (self->input_method == input_method) {
    self->input_active = self->pending_input_active;
    self->serial++;
    g_debug ("input-method-v2 done serial=%u active=%s",
             self->serial,
             self->input_active ? "true" : "false");
  }
}

static void
input_unavailable (void *data, struct zwp_input_method_v2 *input_method)
{
  MynaWayland *self = data;
  if (self->input_method == input_method) {
    zwp_input_method_v2_destroy (self->input_method);
    self->input_method = NULL;
    self->pending_input_active = false;
    self->input_active = false;
    g_warning ("input-method-v2 unavailable; another input method is probably already associated with this seat");
  }
}

static const struct zwp_input_method_v2_listener input_listener = {
  .activate = input_activate,
  .deactivate = input_deactivate,
  .surrounding_text = input_surrounding_text,
  .text_change_cause = input_text_change_cause,
  .content_type = input_content_type,
  .done = input_done,
  .unavailable = input_unavailable,
};

static void
registry_global (void *data,
                 struct wl_registry *registry,
                 uint32_t name,
                 const char *interface,
                 uint32_t version)
{
  MynaWayland *self = data;

  if (strcmp (interface, wl_seat_interface.name) == 0) {
    self->seat = wl_registry_bind (registry, name, &wl_seat_interface, MIN (version, 7));
  } else if (strcmp (interface, wl_output_interface.name) == 0 && !self->output) {
    self->output = wl_registry_bind (registry, name, &wl_output_interface, MIN (version, 3));
  } else if (strcmp (interface, zwlr_layer_shell_v1_interface.name) == 0) {
    self->layer_shell = wl_registry_bind (registry, name, &zwlr_layer_shell_v1_interface, 1);
  } else if (strcmp (interface, zwp_input_method_manager_v2_interface.name) == 0) {
    self->input_method_manager = wl_registry_bind (
        registry,
        name,
        &zwp_input_method_manager_v2_interface,
        1);
  } else if (strcmp (interface, zwp_virtual_keyboard_manager_v1_interface.name) == 0) {
    self->virtual_keyboard_manager = wl_registry_bind (
        registry,
        name,
        &zwp_virtual_keyboard_manager_v1_interface,
        1);
  }
}

static void
registry_global_remove (void *data, struct wl_registry *registry, uint32_t name)
{
  (void) data;
  (void) registry;
  (void) name;
}

static const struct wl_registry_listener registry_listener = {
  .global = registry_global,
  .global_remove = registry_global_remove,
};

static bool
setup_virtual_keyboard_keymap (MynaWayland *self)
{
  struct xkb_context *context = NULL;
  struct xkb_keymap *keymap = NULL;
  char *keymap_text = NULL;
  char *tmp_name = NULL;
  int fd = -1;
  size_t size;
  bool ok = false;

  if (!self->virtual_keyboard)
    return false;

  context = xkb_context_new (XKB_CONTEXT_NO_FLAGS);
  if (!context)
    goto out;

  keymap = xkb_keymap_new_from_names (context, NULL, XKB_KEYMAP_COMPILE_NO_FLAGS);
  if (!keymap)
    goto out;

  keymap_text = xkb_keymap_get_as_string (keymap, XKB_KEYMAP_FORMAT_TEXT_V1);
  if (!keymap_text)
    goto out;

  size = strlen (keymap_text) + 1;
  fd = g_file_open_tmp ("myna-keymap-XXXXXX", &tmp_name, NULL);
  if (fd < 0)
    goto out;
  if (tmp_name)
    g_unlink (tmp_name);
  if (write (fd, keymap_text, size) != (ssize_t) size)
    goto out;
  lseek (fd, 0, SEEK_SET);

  zwp_virtual_keyboard_v1_keymap (self->virtual_keyboard, WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1, fd, size);
  ok = true;

out:
  if (fd >= 0)
    close (fd);
  g_free (tmp_name);
  free (keymap_text);
  if (keymap)
    xkb_keymap_unref (keymap);
  if (context)
    xkb_context_unref (context);
  return ok;
}

bool
myna_wayland_init (MynaWayland *self)
{
  memset (self, 0, sizeof (*self));

  self->display = gdk_wayland_display_get_wl_display (gdk_display_get_default ());
  if (!self->display) {
    g_warning ("No Wayland display available");
    return false;
  }

  self->registry = wl_display_get_registry (self->display);
  wl_registry_add_listener (self->registry, &registry_listener, self);
  wl_display_roundtrip (self->display);
  wl_display_roundtrip (self->display);

  if (!self->layer_shell)
    g_warning ("wlr-layer-shell is unavailable; OSK panel cannot be anchored");
  if (!self->seat)
    g_warning ("wl_seat is unavailable; input method cannot be requested");
  if (!self->input_method_manager)
    g_warning ("input-method-v2 is unavailable; text submission will fail");

  myna_wayland_ensure_input_method (self);

  if (self->virtual_keyboard_manager && self->seat) {
    self->virtual_keyboard = zwp_virtual_keyboard_manager_v1_create_virtual_keyboard (
        self->virtual_keyboard_manager,
        self->seat);
    if (!setup_virtual_keyboard_keymap (self))
      g_warning ("Failed to set virtual-keyboard keymap; Backspace fallback may not work");
  }

  g_message ("Wayland globals: layer_shell=%s seat=%s input_method_manager=%s input_method=%s virtual_keyboard=%s",
             self->layer_shell ? "yes" : "no",
             self->seat ? "yes" : "no",
             self->input_method_manager ? "yes" : "no",
             self->input_method ? "yes" : "no",
             self->virtual_keyboard ? "yes" : "no");

  return self->layer_shell != NULL;
}

bool
myna_wayland_ensure_input_method (MynaWayland *self)
{
  if (self->input_method)
    return true;
  if (!self->input_method_manager || !self->seat)
    return false;

  self->pending_input_active = false;
  self->input_active = false;
  self->serial = 0;
  self->input_method = zwp_input_method_manager_v2_get_input_method (
      self->input_method_manager,
      self->seat);
  zwp_input_method_v2_add_listener (self->input_method, &input_listener, self);
  wl_display_flush (self->display);
  g_debug ("Requested input-method-v2 object");
  return true;
}

void
myna_wayland_destroy (MynaWayland *self)
{
  if (self->virtual_keyboard)
    zwp_virtual_keyboard_v1_destroy (self->virtual_keyboard);
  if (self->virtual_keyboard_manager)
    zwp_virtual_keyboard_manager_v1_destroy (self->virtual_keyboard_manager);
  if (self->input_method)
    zwp_input_method_v2_destroy (self->input_method);
  if (self->input_method_manager)
    zwp_input_method_manager_v2_destroy (self->input_method_manager);
  if (self->layer_shell)
    zwlr_layer_shell_v1_destroy (self->layer_shell);
  if (self->output)
    wl_output_destroy (self->output);
  if (self->seat)
    wl_seat_destroy (self->seat);
  if (self->registry)
    wl_registry_destroy (self->registry);
  g_clear_pointer (&self->surrounding_text, g_free);
}

bool
myna_wayland_commit_string (MynaWayland *self, const char *text)
{
  if (!self->input_method || !self->input_active || !text) {
    g_debug ("input-method-v2 commit rejected has_im=%s active=%s pending=%s serial=%u text=%s",
             self->input_method ? "true" : "false",
             self->input_active ? "true" : "false",
             self->pending_input_active ? "true" : "false",
             self->serial,
             text ? text : "(null)");
    return false;
  }

  g_debug ("input-method-v2 commit_string serial=%u text=%s", self->serial, text);
  zwp_input_method_v2_commit_string (self->input_method, text);
  zwp_input_method_v2_commit (self->input_method, self->serial);
  wl_display_flush (self->display);
  return true;
}

bool
myna_wayland_delete_before (MynaWayland *self, uint32_t bytes)
{
  uint32_t delete_bytes = bytes;

  if (!self->input_method || !self->input_active) {
    g_debug ("input-method-v2 delete rejected has_im=%s active=%s pending=%s serial=%u",
             self->input_method ? "true" : "false",
             self->input_active ? "true" : "false",
             self->pending_input_active ? "true" : "false",
             self->serial);
    return false;
  }
  if (!self->has_surrounding_text || !self->surrounding_text || self->surrounding_cursor == 0)
    return false;

  const char *start = self->surrounding_text;
  const char *cursor = start + MIN (self->surrounding_cursor, strlen (start));
  const char *prev = g_utf8_find_prev_char (start, cursor);

  if (prev)
    delete_bytes = cursor - prev;

  zwp_input_method_v2_delete_surrounding_text (self->input_method, delete_bytes, 0);
  zwp_input_method_v2_commit (self->input_method, self->serial);
  wl_display_flush (self->display);
  return true;
}

bool
myna_wayland_send_backspace (MynaWayland *self)
{
  const uint32_t key_backspace = 14;
  uint32_t time;

  if (!self->virtual_keyboard)
    return false;

  time = (uint32_t) g_get_monotonic_time () / 1000;
  zwp_virtual_keyboard_v1_key (self->virtual_keyboard, time, key_backspace, WL_KEYBOARD_KEY_STATE_PRESSED);
  zwp_virtual_keyboard_v1_key (self->virtual_keyboard, time, key_backspace, WL_KEYBOARD_KEY_STATE_RELEASED);
  wl_display_flush (self->display);
  return true;
}
