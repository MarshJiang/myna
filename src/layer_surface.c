#include "layer_surface.h"

#include <gdk/gdkwayland.h>
#include <wayland-client.h>

struct _MynaLayerSurface {
  GtkWindow parent_instance;

  struct zwlr_layer_shell_v1 *layer_shell;
  struct zwlr_layer_surface_v1 *layer_surface;
  struct wl_surface *wl_surface;
  struct wl_output *output;
  int height;
};

G_DEFINE_TYPE (MynaLayerSurface, myna_layer_surface, GTK_TYPE_WINDOW)

static void
layer_configure (void *data,
                 struct zwlr_layer_surface_v1 *surface,
                 uint32_t serial,
                 uint32_t width,
                 uint32_t height)
{
  MynaLayerSurface *self = MYNA_LAYER_SURFACE (data);

  zwlr_layer_surface_v1_ack_configure (surface, serial);
  gtk_window_resize (GTK_WINDOW (self), width, height);
}

static void
layer_closed (void *data, struct zwlr_layer_surface_v1 *surface)
{
  MynaLayerSurface *self = MYNA_LAYER_SURFACE (data);

  if (self->layer_surface == surface) {
    zwlr_layer_surface_v1_destroy (self->layer_surface);
    self->layer_surface = NULL;
  }

  gtk_widget_hide (GTK_WIDGET (self));
}

static const struct zwlr_layer_surface_v1_listener layer_listener = {
  .configure = layer_configure,
  .closed = layer_closed,
};

static void
myna_layer_surface_realize (GtkWidget *widget)
{
  MynaLayerSurface *self = MYNA_LAYER_SURFACE (widget);
  GdkWindow *gdk_window;

  GTK_WIDGET_CLASS (myna_layer_surface_parent_class)->realize (widget);

  gdk_window = gtk_widget_get_window (widget);
  gdk_wayland_window_set_use_custom_surface (gdk_window);
  self->wl_surface = gdk_wayland_window_get_wl_surface (gdk_window);
}

static void
myna_layer_surface_map (GtkWidget *widget)
{
  MynaLayerSurface *self = MYNA_LAYER_SURFACE (widget);

  GTK_WIDGET_CLASS (myna_layer_surface_parent_class)->map (widget);

  if (!self->wl_surface) {
    GdkWindow *gdk_window = gtk_widget_get_window (widget);
    gdk_wayland_window_set_use_custom_surface (gdk_window);
    self->wl_surface = gdk_wayland_window_get_wl_surface (gdk_window);
  }

  if (self->layer_surface)
    return;

  self->layer_surface = zwlr_layer_shell_v1_get_layer_surface (
      self->layer_shell,
      self->wl_surface,
      self->output,
      ZWLR_LAYER_SHELL_V1_LAYER_TOP,
      "osk");

  zwlr_layer_surface_v1_set_anchor (
      self->layer_surface,
      ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM |
      ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT |
      ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT);
  zwlr_layer_surface_v1_set_size (self->layer_surface, 0, self->height);
  zwlr_layer_surface_v1_set_exclusive_zone (self->layer_surface, self->height);
  zwlr_layer_surface_v1_set_keyboard_interactivity (self->layer_surface, 0);
  zwlr_layer_surface_v1_add_listener (self->layer_surface, &layer_listener, self);
  wl_surface_commit (self->wl_surface);
  wl_display_roundtrip (gdk_wayland_display_get_wl_display (gdk_display_get_default ()));
  g_message ("Created layer-shell surface for Myna OSK at height %d", self->height);
}

static void
myna_layer_surface_unmap (GtkWidget *widget)
{
  MynaLayerSurface *self = MYNA_LAYER_SURFACE (widget);

  if (self->layer_surface) {
    zwlr_layer_surface_v1_destroy (self->layer_surface);
    self->layer_surface = NULL;
  }
  self->wl_surface = NULL;

  GTK_WIDGET_CLASS (myna_layer_surface_parent_class)->unmap (widget);
}

static void
myna_layer_surface_dispose (GObject *object)
{
  MynaLayerSurface *self = MYNA_LAYER_SURFACE (object);

  if (self->layer_surface) {
    zwlr_layer_surface_v1_destroy (self->layer_surface);
    self->layer_surface = NULL;
  }

  G_OBJECT_CLASS (myna_layer_surface_parent_class)->dispose (object);
}

static void
myna_layer_surface_class_init (MynaLayerSurfaceClass *klass)
{
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  widget_class->realize = myna_layer_surface_realize;
  widget_class->map = myna_layer_surface_map;
  widget_class->unmap = myna_layer_surface_unmap;
  object_class->dispose = myna_layer_surface_dispose;
}

static void
myna_layer_surface_init (MynaLayerSurface *self)
{
  gtk_window_set_decorated (GTK_WINDOW (self), FALSE);
  gtk_window_set_resizable (GTK_WINDOW (self), FALSE);
  gtk_window_set_skip_taskbar_hint (GTK_WINDOW (self), TRUE);
  gtk_window_set_skip_pager_hint (GTK_WINDOW (self), TRUE);
  gtk_widget_set_can_focus (GTK_WIDGET (self), FALSE);
  g_object_set (G_OBJECT (self), "accept-focus", FALSE, NULL);
}

GtkWidget *
myna_layer_surface_new (struct zwlr_layer_shell_v1 *layer_shell,
                        struct wl_output *output,
                        int height)
{
  MynaLayerSurface *self = g_object_new (MYNA_TYPE_LAYER_SURFACE, NULL);

  self->layer_shell = layer_shell;
  self->output = output;
  self->height = height;

  gtk_window_set_title (GTK_WINDOW (self), "Myna OSK");
  gtk_window_set_default_size (GTK_WINDOW (self), 360, height);

  return GTK_WIDGET (self);
}

void
myna_layer_surface_set_panel_height (MynaLayerSurface *self, int height)
{
  self->height = height;
  gtk_window_resize (GTK_WINDOW (self), 1, height);

  if (self->layer_surface) {
    zwlr_layer_surface_v1_set_size (self->layer_surface, 0, height);
    zwlr_layer_surface_v1_set_exclusive_zone (self->layer_surface, height);
    myna_layer_surface_wl_surface_commit (self);
  }
}

void
myna_layer_surface_wl_surface_commit (MynaLayerSurface *self)
{
  if (self->wl_surface)
    wl_surface_commit (self->wl_surface);
}
