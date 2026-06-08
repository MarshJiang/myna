#include "bridge.h"

#include <gdk/gdkwayland.h>
#include <glib.h>
#include <glib/gstdio.h>
#include <sys/mman.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <xkbcommon/xkbcommon.h>
#include <xkbcommon/xkbcommon-keysyms.h>

static void
notify_visibility (MynaWayland *self, bool visible)
{
  if (self->visibility_callback)
    self->visibility_callback (self->visibility_user_data, visible);
}

static bool
notify_physical_key (MynaWayland *self, uint32_t keysym, const char *text)
{
  if (self->physical_key_callback)
    return self->physical_key_callback (self->physical_key_user_data, keysym, text);

  return false;
}

static bool
is_modifier_keysym (xkb_keysym_t keysym)
{
  return keysym == XKB_KEY_Shift_L ||
         keysym == XKB_KEY_Shift_R ||
         keysym == XKB_KEY_Control_L ||
         keysym == XKB_KEY_Control_R ||
         keysym == XKB_KEY_Alt_L ||
         keysym == XKB_KEY_Alt_R ||
         keysym == XKB_KEY_Meta_L ||
         keysym == XKB_KEY_Meta_R ||
         keysym == XKB_KEY_Super_L ||
         keysym == XKB_KEY_Super_R ||
         keysym == XKB_KEY_Hyper_L ||
         keysym == XKB_KEY_Hyper_R ||
         keysym == XKB_KEY_Caps_Lock ||
         keysym == XKB_KEY_Num_Lock;
}

static bool
has_command_modifier (MynaWayland *self)
{
  if (!self->physical_state)
    return false;

  return xkb_state_mod_name_is_active (self->physical_state, "Control", XKB_STATE_MODS_EFFECTIVE) > 0 ||
         xkb_state_mod_name_is_active (self->physical_state, "Mod1", XKB_STATE_MODS_EFFECTIVE) > 0 ||
         xkb_state_mod_name_is_active (self->physical_state, "Mod4", XKB_STATE_MODS_EFFECTIVE) > 0;
}

static bool
is_text_input_keysym (xkb_keysym_t keysym)
{
  return keysym == XKB_KEY_BackSpace ||
         keysym == XKB_KEY_Return ||
         keysym == XKB_KEY_KP_Enter ||
         keysym == XKB_KEY_Escape ||
         keysym == XKB_KEY_Page_Up ||
         keysym == XKB_KEY_Page_Down;
}

static bool
is_ascii_text (const char *text)
{
  if (!text || text[0] == '\0' || text[1] != '\0')
    return false;

  return g_ascii_isgraph (text[0]) || text[0] == ' ';
}

static void
forward_physical_key (MynaWayland *self, uint32_t time, uint32_t key)
{
  if (!self->virtual_keyboard)
    return;

  zwp_virtual_keyboard_v1_key (self->virtual_keyboard, time, key, WL_KEYBOARD_KEY_STATE_PRESSED);
  zwp_virtual_keyboard_v1_key (self->virtual_keyboard, time, key, WL_KEYBOARD_KEY_STATE_RELEASED);
  wl_display_flush (self->display);
}

static void
clear_physical_keyboard_state (MynaWayland *self)
{
  if (self->physical_state) {
    xkb_state_unref (self->physical_state);
    self->physical_state = NULL;
  }
  if (self->physical_keymap) {
    xkb_keymap_unref (self->physical_keymap);
    self->physical_keymap = NULL;
  }
}

static void
keyboard_grab_keymap (void *data,
                      struct zwp_input_method_keyboard_grab_v2 *keyboard_grab,
                      uint32_t format,
                      int32_t fd,
                      uint32_t size)
{
  MynaWayland *self = data;
  char *map_text = NULL;
  struct xkb_keymap *keymap = NULL;
  struct xkb_state *state = NULL;

  (void) keyboard_grab;

  if (format != WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1 || size == 0) {
    close (fd);
    return;
  }

  map_text = mmap (NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);
  if (map_text == MAP_FAILED) {
    close (fd);
    return;
  }

  if (!self->xkb_context)
    self->xkb_context = xkb_context_new (XKB_CONTEXT_NO_FLAGS);
  if (!self->xkb_context)
    goto out;

  keymap = xkb_keymap_new_from_string (
      self->xkb_context,
      map_text,
      XKB_KEYMAP_FORMAT_TEXT_V1,
      XKB_KEYMAP_COMPILE_NO_FLAGS);
  if (!keymap)
    goto out;

  state = xkb_state_new (keymap);
  if (!state) {
    xkb_keymap_unref (keymap);
    goto out;
  }

  clear_physical_keyboard_state (self);
  self->physical_keymap = keymap;
  self->physical_state = state;
  g_debug ("input-method-v2 keyboard grab keymap updated");

out:
  munmap (map_text, size);
  close (fd);
}

static void
keyboard_grab_key (void *data,
                   struct zwp_input_method_keyboard_grab_v2 *keyboard_grab,
                   uint32_t serial,
                   uint32_t time,
                   uint32_t key,
                   uint32_t state)
{
  MynaWayland *self = data;
  xkb_keycode_t keycode = key + 8;
  xkb_keysym_t keysym;
  char text[64] = { 0 };
  int length;

  (void) keyboard_grab;
  (void) serial;
  (void) time;

  if (state != WL_KEYBOARD_KEY_STATE_PRESSED)
    return;
  if (!self->physical_state)
    return;

  keysym = xkb_state_key_get_one_sym (self->physical_state, keycode);
  length = xkb_state_key_get_utf8 (self->physical_state, keycode, text, sizeof (text));
  if (length <= 0 || (size_t) length >= sizeof (text))
    text[0] = '\0';

  if (has_command_modifier (self) && !is_modifier_keysym (keysym)) {
    forward_physical_key (self, time, key);
    return;
  }

  if (!is_text_input_keysym (keysym) && !is_ascii_text (text)) {
    if (!is_modifier_keysym (keysym))
      forward_physical_key (self, time, key);
    return;
  }

  if (!notify_physical_key (self, keysym, text[0] ? text : NULL) &&
      !is_modifier_keysym (keysym))
    forward_physical_key (self, time, key);
}

static void
keyboard_grab_modifiers (void *data,
                         struct zwp_input_method_keyboard_grab_v2 *keyboard_grab,
                         uint32_t serial,
                         uint32_t mods_depressed,
                         uint32_t mods_latched,
                         uint32_t mods_locked,
                         uint32_t group)
{
  MynaWayland *self = data;

  (void) keyboard_grab;
  (void) serial;

  if (self->physical_state) {
    xkb_state_update_mask (
        self->physical_state,
        mods_depressed,
        mods_latched,
        mods_locked,
        0,
        0,
        group);
  }
  if (self->virtual_keyboard) {
    zwp_virtual_keyboard_v1_modifiers (
        self->virtual_keyboard,
        mods_depressed,
        mods_latched,
        mods_locked,
        group);
    wl_display_flush (self->display);
  }
}

static void
keyboard_grab_repeat_info (void *data,
                           struct zwp_input_method_keyboard_grab_v2 *keyboard_grab,
                           int32_t rate,
                           int32_t delay)
{
  (void) data;
  (void) keyboard_grab;
  (void) rate;
  (void) delay;
}

static const struct zwp_input_method_keyboard_grab_v2_listener keyboard_grab_listener = {
  .keymap = keyboard_grab_keymap,
  .key = keyboard_grab_key,
  .modifiers = keyboard_grab_modifiers,
  .repeat_info = keyboard_grab_repeat_info,
};

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
    bool was_active = self->input_active;
    self->input_active = self->pending_input_active;
    self->serial++;
    g_debug ("input-method-v2 done serial=%u active=%s",
             self->serial,
             self->input_active ? "true" : "false");
    if (self->input_active != was_active)
      notify_visibility (self, self->input_active);
  }
}

static void
input_unavailable (void *data, struct zwp_input_method_v2 *input_method)
{
  MynaWayland *self = data;
  if (self->input_method == input_method) {
    if (self->keyboard_grab) {
      zwp_input_method_keyboard_grab_v2_release (self->keyboard_grab);
      self->keyboard_grab = NULL;
    }
    clear_physical_keyboard_state (self);
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
  MynaWaylandVisibilityCallback visibility_callback = self->visibility_callback;
  void *visibility_user_data = self->visibility_user_data;
  MynaWaylandPhysicalKeyCallback physical_key_callback = self->physical_key_callback;
  void *physical_key_user_data = self->physical_key_user_data;

  memset (self, 0, sizeof (*self));
  self->visibility_callback = visibility_callback;
  self->visibility_user_data = visibility_user_data;
  self->physical_key_callback = physical_key_callback;
  self->physical_key_user_data = physical_key_user_data;

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

void
myna_wayland_set_visibility_callback (MynaWayland *self,
                                      MynaWaylandVisibilityCallback callback,
                                      void *user_data)
{
  self->visibility_callback = callback;
  self->visibility_user_data = user_data;
}

void
myna_wayland_set_physical_key_callback (MynaWayland *self,
                                        MynaWaylandPhysicalKeyCallback callback,
                                        void *user_data)
{
  self->physical_key_callback = callback;
  self->physical_key_user_data = user_data;
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
  self->keyboard_grab = zwp_input_method_v2_grab_keyboard (self->input_method);
  zwp_input_method_keyboard_grab_v2_add_listener (
      self->keyboard_grab,
      &keyboard_grab_listener,
      self);
  wl_display_flush (self->display);
  g_debug ("Requested input-method-v2 object");
  return true;
}

void
myna_wayland_destroy (MynaWayland *self)
{
  if (self->keyboard_grab)
    zwp_input_method_keyboard_grab_v2_release (self->keyboard_grab);
  clear_physical_keyboard_state (self);
  if (self->xkb_context)
    xkb_context_unref (self->xkb_context);
  self->xkb_context = NULL;
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
