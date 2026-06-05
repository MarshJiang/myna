#pragma once

#include <stdbool.h>
#include <wayland-client.h>

#include "input-method-unstable-v2-client-protocol.h"
#include "virtual-keyboard-unstable-v1-client-protocol.h"
#include "wlr-layer-shell-unstable-v1-client-protocol.h"

typedef void (*MynaWaylandVisibilityCallback) (void *user_data, bool visible);

typedef struct {
  struct wl_display *display;
  struct wl_registry *registry;
  struct wl_seat *seat;
  struct wl_output *output;
  struct zwlr_layer_shell_v1 *layer_shell;
  struct zwp_input_method_manager_v2 *input_method_manager;
  struct zwp_input_method_v2 *input_method;
  struct zwp_virtual_keyboard_manager_v1 *virtual_keyboard_manager;
  struct zwp_virtual_keyboard_v1 *virtual_keyboard;
  bool pending_input_active;
  bool input_active;
  uint32_t serial;
  char *surrounding_text;
  uint32_t surrounding_cursor;
  bool has_surrounding_text;
  MynaWaylandVisibilityCallback visibility_callback;
  void *visibility_user_data;
} MynaWayland;

bool myna_wayland_init          (MynaWayland *self);
void myna_wayland_destroy       (MynaWayland *self);
void myna_wayland_set_visibility_callback (MynaWayland *self,
                                           MynaWaylandVisibilityCallback callback,
                                           void *user_data);
bool myna_wayland_ensure_input_method (MynaWayland *self);
bool myna_wayland_commit_string (MynaWayland *self, const char *text);
bool myna_wayland_delete_before (MynaWayland *self, uint32_t bytes);
bool myna_wayland_send_backspace (MynaWayland *self);
