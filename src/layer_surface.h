#pragma once

#include <gtk/gtk.h>
#include "wlr-layer-shell-unstable-v1-client-protocol.h"

G_BEGIN_DECLS

#define MYNA_TYPE_LAYER_SURFACE (myna_layer_surface_get_type ())
G_DECLARE_FINAL_TYPE (MynaLayerSurface, myna_layer_surface, MYNA, LAYER_SURFACE, GtkWindow)

GtkWidget *myna_layer_surface_new                (struct zwlr_layer_shell_v1 *layer_shell,
                                                  struct wl_output           *output,
                                                  int                         height);
void       myna_layer_surface_set_panel_height   (MynaLayerSurface           *self,
                                                  int                         height);
void       myna_layer_surface_wl_surface_commit  (MynaLayerSurface           *self);

G_END_DECLS
