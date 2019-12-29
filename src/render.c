#include <gdk/gdk.h>
#include <gtk/gtk.h>

#include "swappy.h"

#ifndef M_PI
#define M_PI (3.14159265358979323846)
#endif

static cairo_format_t get_cairo_format(enum wl_shm_format wl_fmt) {
  switch (wl_fmt) {
    case WL_SHM_FORMAT_ARGB8888:
      return CAIRO_FORMAT_ARGB32;
    case WL_SHM_FORMAT_XRGB8888:
      return CAIRO_FORMAT_RGB24;
    default:
      return CAIRO_FORMAT_INVALID;
  }
}

static int get_output_flipped(enum wl_output_transform transform) {
  return transform & WL_OUTPUT_TRANSFORM_FLIPPED ? -1 : 1;
}

static void apply_output_transform(enum wl_output_transform transform,
                                   int32_t *width, int32_t *height) {
  if (transform & WL_OUTPUT_TRANSFORM_90) {
    int32_t tmp = *width;
    *width = *height;
    *height = tmp;
  }
}

static void render_shape_arrow(cairo_t *cr, struct swappy_paint_shape shape) {
  cairo_set_source_rgba(cr, shape.r, shape.g, shape.b, shape.a);
  cairo_set_line_width(cr, shape.w);

  double ftx = shape.to.x - shape.from.x;
  double fty = shape.to.y - shape.from.y;
  double ftn = sqrt(ftx * ftx + fty * fty);

  double r = 20;
  double scaling_factor = shape.w / 4;

  double alpha = M_PI / 6;
  double ta = 5 * alpha;
  double tb = 7 * alpha;
  double xa = r * cos(ta);
  double ya = r * sin(ta);
  double xb = r * cos(tb);
  double yb = r * sin(tb);
  double xc = ftn - fabs(xa) * scaling_factor;

  if (xc < DBL_EPSILON) {
    xc = 0;
  }

  if (ftn < DBL_EPSILON) {
    return;
  }

  double theta = atan(fty / ftx);

  if (ftx < DBL_EPSILON) {
    theta = M_PI + theta;
  }

  // Draw line
  cairo_save(cr);
  cairo_translate(cr, shape.from.x, shape.from.y);
  cairo_rotate(cr, theta);
  cairo_move_to(cr, 0, 0);
  cairo_line_to(cr, xc, 0);
  cairo_stroke(cr);
  cairo_restore(cr);

  // Draw arrow
  cairo_save(cr);
  cairo_translate(cr, shape.to.x, shape.to.y);
  cairo_rotate(cr, theta);
  cairo_scale(cr, scaling_factor, scaling_factor);
  cairo_move_to(cr, 0, 0);
  cairo_line_to(cr, xa, ya);
  cairo_line_to(cr, xb, yb);
  cairo_line_to(cr, 0, 0);
  cairo_fill(cr);
  cairo_restore(cr);
}

static void render_shape_ellipse(cairo_t *cr, struct swappy_paint_shape shape) {
  double x = fabs(shape.from.x - shape.to.x);
  double y = fabs(shape.from.y - shape.to.y);
  double xc = shape.from.x + ((shape.to.x - shape.from.x) / 2);
  double yc = shape.from.y + ((shape.to.y - shape.from.y) / 2);

  double n = sqrt(x * x + y * y);
  double r = n / 2;

  cairo_set_source_rgba(cr, shape.r, shape.g, shape.b, shape.a);
  cairo_set_line_width(cr, shape.w);

  cairo_matrix_t save_matrix;
  cairo_get_matrix(cr, &save_matrix);
  cairo_translate(cr, xc, yc);
  cairo_scale(cr, x / n, y / n);
  cairo_arc(cr, 0, 0, r, 0, 2 * M_PI);
  cairo_set_matrix(cr, &save_matrix);
  cairo_stroke(cr);
  cairo_close_path(cr);
}

static void render_shape_rectangle(cairo_t *cr,
                                   struct swappy_paint_shape shape) {
  double x = fmin(shape.from.x, shape.to.x);
  double y = fmin(shape.from.y, shape.to.y);
  double w = fabs(shape.from.x - shape.to.x);
  double h = fabs(shape.from.y - shape.to.y);

  cairo_set_source_rgba(cr, shape.r, shape.g, shape.b, shape.a);
  cairo_set_line_width(cr, shape.w);

  cairo_rectangle(cr, x, y, w, h);
  cairo_close_path(cr);
  cairo_stroke(cr);
}

static void render_shape(cairo_t *cr, struct swappy_paint_shape shape) {
  cairo_save(cr);
  switch (shape.type) {
    case SWAPPY_PAINT_MODE_RECTANGLE:
      render_shape_rectangle(cr, shape);
      break;
    case SWAPPY_PAINT_MODE_ELLIPSE:
      render_shape_ellipse(cr, shape);
      break;
    case SWAPPY_PAINT_MODE_ARROW:
      render_shape_arrow(cr, shape);
      break;
    default:
      break;
  }
  cairo_restore(cr);
}

static void render_buffer(cairo_t *cr, struct swappy_state *state) {
  // FIXME This is wrong, the geometry here is not quite valid
  // It must be based on output, but will work fine on single screen
  struct swappy_box *geometry = state->geometry;
  struct swappy_output *output;

  wl_list_for_each(output, &state->outputs, link) {
    struct swappy_buffer *buffer = output->buffer;

    if (output->buffer == NULL) {
      g_warning(
          "screencopy buffer is empty, cannot draw it onto the paint area");
      continue;
    }

    cairo_format_t format = get_cairo_format(buffer->format);

    g_assert(format != CAIRO_FORMAT_INVALID);

    int32_t output_x = output->logical_geometry.x - geometry->x;
    int32_t output_y = output->logical_geometry.y - geometry->y;
    int32_t output_width = output->logical_geometry.width;
    int32_t output_height = output->logical_geometry.height;
    int32_t scale = output->scale;

    int32_t raw_output_width = output->geometry.width;
    int32_t raw_output_height = output->geometry.height;
    apply_output_transform(output->transform, &raw_output_width,
                           &raw_output_height);

    int output_flipped_x = get_output_flipped(output->transform);
    int output_flipped_y =
        output->screencopy_frame_flags & ZWLR_SCREENCOPY_FRAME_V1_FLAGS_Y_INVERT
            ? -1
            : 1;

    cairo_surface_t *output_surface = cairo_image_surface_create_for_data(
        buffer->data, format, buffer->width, buffer->height, buffer->stride);
    cairo_pattern_t *output_pattern =
        cairo_pattern_create_for_surface(output_surface);

    // All transformations are in pattern-local coordinates
    cairo_matrix_t matrix;
    cairo_matrix_init_identity(&matrix);
    cairo_matrix_translate(&matrix, (double)output->geometry.width / 2,
                           (double)output->geometry.height / 2);
    //    cairo_matrix_rotate(&matrix, -get_output_rotation(output->transform));
    cairo_matrix_scale(
        &matrix, (double)raw_output_width / output_width * output_flipped_x,
        (double)raw_output_height / output_height * output_flipped_y);
    cairo_matrix_translate(&matrix, -(double)output_width / 2,
                           -(double)output_height / 2);
    cairo_matrix_translate(&matrix, -output_x, -output_y);
    cairo_matrix_scale(&matrix, 1 / scale, 1 / scale);
    cairo_pattern_set_matrix(output_pattern, &matrix);

    cairo_pattern_set_filter(output_pattern, CAIRO_FILTER_BEST);

    cairo_set_source(cr, output_pattern);
    cairo_pattern_destroy(output_pattern);

    cairo_paint(cr);

    cairo_surface_destroy(output_surface);
  }
}

static void render_brush(cairo_t *cr, struct swappy_paint_brush brush) {
  cairo_set_source_rgba(cr, brush.r, brush.g, brush.b, brush.a);
  cairo_set_line_width(cr, brush.w);
  cairo_set_line_join(cr, CAIRO_LINE_JOIN_BEVEL);

  guint l = g_list_length(brush.points);

  if (l == 1) {
    struct swappy_point *point = g_list_nth_data(brush.points, 0);
    cairo_rectangle(cr, point->x, point->y, brush.w, brush.w);
    cairo_fill(cr);
  } else {
    for (GList *elem = brush.points; elem; elem = elem->next) {
      struct swappy_point *point = elem->data;
      cairo_line_to(cr, point->x, point->y);
    }
    cairo_stroke(cr);
  }
}

static void render_paint(cairo_t *cr, struct swappy_paint *paint) {
  if (!paint->can_draw) {
    return;
  }

  switch (paint->type) {
    case SWAPPY_PAINT_MODE_BRUSH:
      render_brush(cr, paint->content.brush);
      break;
    case SWAPPY_PAINT_MODE_RECTANGLE:
    case SWAPPY_PAINT_MODE_ELLIPSE:
    case SWAPPY_PAINT_MODE_ARROW:
      render_shape(cr, paint->content.shape);
      break;
    default:
      g_info("unable to draw paint with type: %d", paint->type);
      break;
  }
}

static void render_paints(cairo_t *cr, struct swappy_state *state) {
  for (GList *elem = g_list_last(state->paints); elem; elem = elem->prev) {
    struct swappy_paint *paint = elem->data;
    render_paint(cr, paint);
  }

  if (state->temp_paint) {
    render_paint(cr, state->temp_paint);
  }
}

void render_state(struct swappy_state *state) {
  cairo_t *cr = cairo_create(state->cairo_surface);

  cairo_set_source_rgb(cr, 1, 1, 1);

  cairo_set_source_rgb(cr, 0.0, 0.0, 0.0);
  cairo_paint(cr);

  render_buffer(cr, state);
  render_paints(cr, state);

  // Drawing is finished, notify the GtkDrawingArea it needs to be redrawn.
  gtk_widget_queue_draw(state->ui->area);

  cairo_destroy(cr);
}
