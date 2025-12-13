#include <pebble.h>

static Window *s_main_window;
static Layer *s_display_layer;

// Animation state
static AppTimer *s_animation_timer;
static int s_animation_frame = 0;
static bool s_is_animating = false;
static bool s_grow_only = false;  // If true, skip shrink phase

// Display modes
typedef enum {
  DISPLAY_TIME,
  DISPLAY_DATE,
  DISPLAY_BATTERY_STEPS
} DisplayMode;

static DisplayMode s_display_mode = DISPLAY_TIME;
static AppTimer *s_return_to_time_timer = NULL;

// Store the time to display during animation
static int s_stored_hour_tens = 0;
static int s_stored_hour_ones = 0;
static int s_stored_min_tens = 0;
static int s_stored_min_ones = 0;

#define ANIMATION_FRAMES_SHRINK 16
#define ANIMATION_FRAMES_GROW 16
#define TOTAL_ANIMATION_FRAMES (ANIMATION_FRAMES_SHRINK + ANIMATION_FRAMES_GROW)
#define ANIMATION_FRAME_DURATION_MS 40

// Distortion constants - middle segment position as fraction from top
#define DISTORTION 0.18f
#define THICKNESS 8
#define MARGIN_H 14
#define MARGIN_W 12

// Segment indices: 0=top, 1=top-right, 2=bottom-right, 3=bottom, 4=bottom-left, 5=top-left, 6=middle
static const bool DIGIT_SEGMENTS[10][7] = {
  {1, 1, 1, 1, 1, 1, 0}, // 0
  {0, 1, 1, 0, 0, 0, 0}, // 1
  {1, 1, 0, 1, 1, 0, 1}, // 2
  {1, 1, 1, 1, 0, 0, 1}, // 3
  {0, 1, 1, 0, 0, 1, 1}, // 4
  {1, 0, 1, 1, 0, 1, 1}, // 5
  {1, 0, 1, 1, 1, 1, 1}, // 6
  {1, 1, 1, 0, 0, 0, 0}, // 7
  {1, 1, 1, 1, 1, 1, 1}, // 8
  {1, 1, 1, 0, 0, 1, 1}  // 9
};

// Get scale factor for a digit level during animation
// level: 0=innermost (hour_tens), 1=hour_ones, 2=min_tens, 3=min_ones (outermost)
// Returns 0.0 to 1.0
static float get_digit_scale(int level, int animation_frame) {
  if (s_grow_only) {
    // Skip shrink phase, go straight to growing
    animation_frame += ANIMATION_FRAMES_SHRINK;
  }

  if (animation_frame < ANIMATION_FRAMES_SHRINK) {
    // Shrinking phase: innermost (0) shrinks first, outermost (3) shrinks last
    int start_frame = level * 2;  // Stagger the shrink
    int end_frame = start_frame + ANIMATION_FRAMES_SHRINK / 2;

    if (animation_frame < start_frame) {
      return 1.0f;  // Not started shrinking yet
    } else if (animation_frame >= end_frame) {
      return 0.0f;  // Fully shrunk
    } else {
      // Shrinking
      float progress = (float)(animation_frame - start_frame) / (end_frame - start_frame);
      return 1.0f - progress;
    }
  } else {
    // Growing phase: outermost (3) grows first, innermost (0) grows last
    int grow_frame = animation_frame - ANIMATION_FRAMES_SHRINK;
    int reversed_level = 3 - level;  // Reverse the order for growing
    int start_frame = reversed_level * 2;
    int end_frame = start_frame + ANIMATION_FRAMES_GROW / 2;

    if (grow_frame < start_frame) {
      return 0.0f;  // Not started growing yet
    } else if (grow_frame >= end_frame) {
      return 1.0f;  // Fully grown
    } else {
      // Growing
      float progress = (float)(grow_frame - start_frame) / (end_frame - start_frame);
      return progress;
    }
  }
}

// Draw a distorted segment - center is at 15% from top, so middle segment is much higher
static void draw_distorted_segment(GContext *ctx, GPoint center, int segment_type, int digit_width, int digit_height, int segment_thickness, float scale, float distortion) {
  // Apply scale to dimensions
  digit_width *= scale;
  digit_height *= scale;
  segment_thickness *= scale;

  // The "center" point is at 15% from top of the digit
  // This means the middle segment is only 15% down from top, creating a compressed top and huge bottom

  int segment_width = digit_width;
  int distorted_y = center.y - digit_height / 2 + digit_height * distortion;

  GPathInfo segment_path_info;
  GPoint points[4];

  switch(segment_type) {
    case 0: // Top horizontal
      {
        int y = center.y - digit_height / 2;
        points[0] = GPoint(center.x - segment_width / 2, y);
        points[1] = GPoint(center.x + segment_width / 2, y);
        points[2] = GPoint(center.x + segment_width / 2, y + segment_thickness);
        points[3] = GPoint(center.x - segment_width / 2, y + segment_thickness);
      }
      break;

    case 1: // Top-right vertical (from top to middle)
      {
        int top_y = center.y - digit_height / 2;
        int mid_y = distorted_y;
        int x = center.x + segment_width / 2;

        points[0] = GPoint(x - segment_thickness, top_y);
        points[1] = GPoint(x, top_y);
        points[2] = GPoint(x, mid_y);
        points[3] = GPoint(x - segment_thickness, mid_y);
      }
      break;

    case 2: // Bottom-right vertical (from middle to bottom)
      {
        int mid_y = distorted_y;
        int bottom_y = center.y + digit_height / 2;
        int x = center.x + segment_width / 2;

        points[0] = GPoint(x - segment_thickness, mid_y);
        points[1] = GPoint(x, mid_y);
        points[2] = GPoint(x, bottom_y);
        points[3] = GPoint(x - segment_thickness, bottom_y);
      }
      break;

    case 3: // Bottom horizontal
      {
        int y = center.y + digit_height / 2;
        points[0] = GPoint(center.x - segment_width / 2, y - segment_thickness);
        points[1] = GPoint(center.x + segment_width / 2, y - segment_thickness);
        points[2] = GPoint(center.x + segment_width / 2, y);
        points[3] = GPoint(center.x - segment_width / 2, y);
      }
      break;

    case 4: // Bottom-left vertical (from middle to bottom)
      {
        int mid_y = distorted_y;
        int bottom_y = center.y + digit_height / 2;
        int x = center.x - segment_width / 2;

        points[0] = GPoint(x, mid_y);
        points[1] = GPoint(x + segment_thickness, mid_y);
        points[2] = GPoint(x + segment_thickness, bottom_y);
        points[3] = GPoint(x, bottom_y);
      }
      break;

    case 5: // Top-left vertical (from top to middle)
      {
        int top_y = center.y - digit_height / 2;
        int mid_y = distorted_y;
        int x = center.x - segment_width / 2;

        points[0] = GPoint(x , top_y);
        points[1] = GPoint(x + segment_thickness, top_y);
        points[2] = GPoint(x + segment_thickness, mid_y);
        points[3] = GPoint(x, mid_y);
      }
      break;

    case 6: // Middle horizontal (at the distorted center - 15% from top!)
      {
        int y = distorted_y;
        points[0] = GPoint(center.x - segment_width / 2, y - segment_thickness);
        points[1] = GPoint(center.x + segment_width / 2, y - segment_thickness);
        points[2] = GPoint(center.x + segment_width / 2, y);
        points[3] = GPoint(center.x - segment_width / 2, y);
      }
      break;
  }

  segment_path_info.num_points = 4;
  segment_path_info.points = points;

  GPath *segment_path = gpath_create(&segment_path_info);
  gpath_draw_filled(ctx, segment_path);
  gpath_destroy(segment_path);
}


// Structure to hold digit dimensions and position
typedef struct {
  GPoint center;
  int width;
  int height;
  int thickness;
} DigitLayout;

// Calculate nested digit layouts based on screen bounds and distortion
static void calculate_digit_layouts(GRect bounds, DigitLayout layouts[4], int digits[4]) {
  // Level 0 (outermost): Fill screen without margins
  layouts[0].width = bounds.size.w;
  layouts[0].height = bounds.size.h;
  layouts[0].center = GPoint(bounds.size.w / 2-1, bounds.size.h / 2);
  layouts[0].thickness = THICKNESS;

  int num_middle_segments = !(digits[0] == 0 || digits[0] == 1 || digits[0] == 7) ? 1 : 0;

  // For each subsequent level, nest within parent's body region
  for (int level = 1; level < 4; level++) {
    DigitLayout *parent = &layouts[level - 1];
    DigitLayout *current = &layouts[level];
    int parent_digit = digits[level - 1];
    int current_digit = digits[level];
    int parent_body_height = parent->height * (1.0 - DISTORTION);

    // Position current digit in center of parent's body region
    current->center.x = parent->center.x;
    current->center.y = parent->center.y;
    current->height = parent->height;
    current->width = parent->width;
    current->thickness = parent->thickness;

    // First adapt the height correctly
    if(parent_digit == 2 || parent_digit == 3 || parent_digit == 5 || parent_digit == 6 || parent_digit == 8){
      current->center.y += (parent->height - parent_body_height) / 2; //(parent->height * DISTORTION) / 2;
      current->height = parent_body_height;

      current->height -= parent->thickness;
      current->center.y -= parent->thickness / 2;
      current->height -= MARGIN_H;
      
      current->thickness -= 1;
    } else if(parent_digit == 9 || parent_digit == 4){
      current->center.y += (parent->height - parent_body_height) / 2;
      current->height = parent_body_height;
      current->height -= MARGIN_H / 2;
      current->center.y += MARGIN_H / 4;
      current->thickness -= 1;
    } else if(parent_digit == 0){
      current->height -= parent->thickness*2;
      current->height -= MARGIN_H;
    } else if(parent_digit == 1){
      // NOP as of full height
    } else if (parent_digit == 7){
      current->height -= parent->thickness;
      current->center.y += parent->thickness/2;
      current->height -= MARGIN_H / 2;
      current->center.y += MARGIN_H / 4;
    }

    // In case we have to draw a bold digit, ensure smaller thickness
    if(current_digit == 2 || current_digit == 3 || current_digit == 5 || current_digit == 6 || current_digit == 8){
        current->thickness -= 1;  
      }

    int min_thickness = level < 3 ? 4 : 6;
    current->thickness = current->thickness < min_thickness ? min_thickness : current->thickness;

    // Now adapt the width correctly
    if(parent_digit == 0 || parent_digit == 6 || parent_digit == 8){
      current->width -= parent->thickness*2;
      current->width -= MARGIN_W;
    } else if (parent_digit == 2) {
      current->width -= parent->thickness;
      current->center.x += parent->thickness/2;
      current->width -= MARGIN_W / 2;
      current->center.x += MARGIN_W / 4;
    } else if (parent_digit == 1 || parent_digit == 3 || parent_digit == 4 || parent_digit == 5 || parent_digit == 7 || parent_digit == 9){
      current->width -= parent->thickness;
      current->center.x -= parent->thickness/2;
      current->width -= MARGIN_W / 2;
      current->center.x -= MARGIN_W / 4;
    }
  }
}

// Draw a single digit with distortion
static void draw_distorted_digit(GContext *ctx, int digit, GPoint center, int width, int height, int thickness, GColor color, float scale, float distortion) {
  if (digit < 0 || digit > 9) return;
  if (scale <= 0.0f) return;  // Don't draw if scale is 0

  graphics_context_set_fill_color(ctx, color);

  for (int i = 0; i < 7; i++) {
    if (DIGIT_SEGMENTS[digit][i]) {
      draw_distorted_segment(ctx, center, i, width, height, thickness, scale, distortion);
    }
  }
}


// Update procedure for the display layer
static void display_layer_update_proc(Layer *layer, GContext *ctx) {
  GRect bounds = layer_get_bounds(layer);

  // Clear background
  graphics_context_set_fill_color(ctx, GColorBlack);
  graphics_fill_rect(ctx, bounds, 0, GCornerNone);

  // Get current time
  time_t temp = time(NULL);
  struct tm *tick_time = localtime(&temp);

  int digit_0, digit_1, digit_2, digit_3;

  if (s_display_mode == DISPLAY_DATE) {
    // Show date: month and day (MM-DD)
    int month = tick_time->tm_mon + 1;  // tm_mon is 0-11
    int day = tick_time->tm_mday;

    digit_0 = month / 10;
    digit_1 = month % 10;
    digit_2 = day / 10;
    digit_3 = day % 10;
  } else if (s_display_mode == DISPLAY_BATTERY_STEPS) {
    // Show battery (inner, max 99) and steps in thousands (outer)
    BatteryChargeState battery = battery_state_service_peek();
    int battery_percent = battery.charge_percent;
    if (battery_percent > 99) battery_percent = 99;

    HealthValue steps = health_service_sum_today(HealthMetricStepCount);
    // steps = 12000; // Testing
    int steps_thousands = steps / 1000;
    if (steps_thousands > 99) steps_thousands = 99;

    digit_0 = steps_thousands / 10;
    digit_1 = steps_thousands % 10;
    digit_2 = battery_percent / 10;
    digit_3 = battery_percent % 10;
  } else {
    // Show time: hours and minutes (HH:MM)
    int hours = tick_time->tm_hour;
    int minutes = tick_time->tm_min;

    digit_0 = hours / 10;
    digit_1 = hours % 10;
    digit_2 = minutes / 10;
    digit_3 = minutes % 10;
  }

  // Extract digits
  int hour_tens = digit_0;
  int hour_ones = digit_1;
  int min_tens = digit_2;
  int min_ones = digit_3;

  bool is_shrinking = (s_animation_frame < ANIMATION_FRAMES_SHRINK);

  // During animation, use stored old time during shrink, new time during grow
  if (is_shrinking && s_is_animating && !s_grow_only) {
    hour_tens = s_stored_hour_tens;
    hour_ones = s_stored_hour_ones;
    min_tens = s_stored_min_tens;
    min_ones = s_stored_min_ones;
  }

  // Screenshot
  // hour_tens = 0;
  // hour_ones = 7;
  // min_tens = 6;
  // min_ones = 3;

  // Extrema 1
  // hour_tens = 2;
  // hour_ones = 7;
  // min_tens = 1;
  // min_ones = 1;

  // Extrema 2
  // hour_tens = 0;
  // hour_ones = 0;
  // min_tens = 0;
  // min_ones = 0;

  // Extrema 3
  // hour_tens = 1;
  // hour_ones = 1;
  // min_tens = 1;
  // min_ones = 1;

  // Extrema 4
  // hour_tens = 2;
  // hour_ones = 2;
  // min_tens = 2;
  // min_ones = 2;

  // hour_tens = 2;
  // hour_ones = 2;
  // min_tens = 1;
  // min_ones = 2;

  // Calculate proper dimensions and positions for all nested digits
  DigitLayout layouts[4];
  int digits[4] = {min_ones, min_tens, hour_ones, hour_tens};
  calculate_digit_layouts(bounds, layouts, digits);

  // Calculate scale factors for animation
  float scale_level0 = s_is_animating ? get_digit_scale(0, s_animation_frame) : 1.0f;
  float scale_level1 = s_is_animating ? get_digit_scale(1, s_animation_frame) : 1.0f;
  float scale_level2 = s_is_animating ? get_digit_scale(2, s_animation_frame) : 1.0f;
  float scale_level3 = s_is_animating ? get_digit_scale(3, s_animation_frame) : 1.0f;

  // Draw from largest to smallest (level 0 = outermost, level 3 = innermost)
  // Digit mapping: level 0 = min_ones, level 1 = min_tens, level 2 = hour_ones, level 3 = hour_tens
  // Colors: time (white gray white gray), date (white white gray gray), battery/steps (gray gray white white)
  GColor colors[4];
  if (s_display_mode == DISPLAY_DATE) {
    colors[0] = GColorWhite;      // day ones
    colors[1] = GColorWhite;      // day tens
    colors[2] = GColorLightGray;  // month ones
    colors[3] = GColorLightGray;  // month tens
  } else if (s_display_mode == DISPLAY_BATTERY_STEPS) {
    colors[0] = GColorLightGray;  // steps thousands ones
    colors[1] = GColorLightGray;  // steps thousands tens
    colors[2] = GColorWhite;      // battery ones
    colors[3] = GColorWhite;      // battery tens
  } else {
    colors[0] = GColorWhite;      // min_ones
    colors[1] = GColorLightGray;  // min_tens
    colors[2] = GColorWhite;      // hour_ones
    colors[3] = GColorLightGray;  // hour_tens
  }

  float scales[4] = {scale_level3, scale_level2, scale_level1, scale_level0};

  // Draw digits from outermost to innermost
  for (int i = 0; i < 4; i++) {

    float distortion = (i < 3) ? DISTORTION : 0.5f;
    // First three levels use distortion
    draw_distorted_digit(ctx, digits[i], layouts[i].center, layouts[i].width,
                        layouts[i].height, layouts[i].thickness, colors[i],
                        scales[i], distortion);
  }
}

static void animation_timer_callback(void *data) {
  s_animation_frame++;

  int max_frames = s_grow_only ? ANIMATION_FRAMES_GROW : TOTAL_ANIMATION_FRAMES;

  if (s_animation_frame >= max_frames) {
    // Animation complete
    s_animation_frame = 0;
    s_is_animating = false;
    s_grow_only = false;
    s_animation_timer = NULL;
  } else {
    // Schedule next frame
    s_animation_timer = app_timer_register(ANIMATION_FRAME_DURATION_MS, animation_timer_callback, NULL);
  }

  // Redraw
  layer_mark_dirty(s_display_layer);
}

static void start_animation(bool grow_only) {
  if (s_is_animating) return;  // Already animating

  s_is_animating = true;
  s_grow_only = grow_only;
  s_animation_frame = 0;

  // Start the animation timer
  s_animation_timer = app_timer_register(ANIMATION_FRAME_DURATION_MS, animation_timer_callback, NULL);

  // Trigger immediate redraw
  layer_mark_dirty(s_display_layer);
}

static void return_to_time_callback(void *data) {
  // Switch back to time display from date or battery/steps
  s_return_to_time_timer = NULL;

  // Store current display values for animation
  time_t temp = time(NULL);
  struct tm *tick_time = localtime(&temp);

  if (s_display_mode == DISPLAY_DATE) {
    int month = tick_time->tm_mon + 1;
    int day = tick_time->tm_mday;

    s_stored_hour_tens = month / 10;
    s_stored_hour_ones = month % 10;
    s_stored_min_tens = day / 10;
    s_stored_min_ones = day % 10;
  } else if (s_display_mode == DISPLAY_BATTERY_STEPS) {
    BatteryChargeState battery = battery_state_service_peek();
    int battery_percent = battery.charge_percent;
    if (battery_percent > 99) battery_percent = 99;

    HealthValue steps = health_service_sum_today(HealthMetricStepCount);
    int steps_thousands = steps / 1000;
    if (steps_thousands > 99) steps_thousands = 99;

    s_stored_hour_tens = steps_thousands / 10;
    s_stored_hour_ones = steps_thousands % 10;
    s_stored_min_tens = battery_percent / 10;
    s_stored_min_ones = battery_percent % 10;
  }

  s_display_mode = DISPLAY_TIME;
  start_animation(false);  // Animate back to time
}

static void accel_tap_handler(AccelAxisType axis, int32_t direction) {
  // When we already animate, ignore new flicks and finish this first
  if(s_is_animating){
    return;
  }

  // Handle flick gesture - cycle through display modes
  // Cancel existing return-to-time timer
  if (s_return_to_time_timer) {
    app_timer_cancel(s_return_to_time_timer);
    s_return_to_time_timer = NULL;
  }

  time_t temp = time(NULL);
  struct tm *tick_time = localtime(&temp);

  // Store current display values for animation
  if (s_display_mode == DISPLAY_TIME) {
    // Store time values
    int hours = tick_time->tm_hour;
    int minutes = tick_time->tm_min;

    s_stored_hour_tens = hours / 10;
    s_stored_hour_ones = hours % 10;
    s_stored_min_tens = minutes / 10;
    s_stored_min_ones = minutes % 10;

    // Switch to date
    s_display_mode = DISPLAY_DATE;
  } else if (s_display_mode == DISPLAY_DATE) {
    // Store date values
    int month = tick_time->tm_mon + 1;
    int day = tick_time->tm_mday;

    s_stored_hour_tens = month / 10;
    s_stored_hour_ones = month % 10;
    s_stored_min_tens = day / 10;
    s_stored_min_ones = day % 10;

    // Switch to battery/steps
    s_display_mode = DISPLAY_BATTERY_STEPS;
  } else {
    // Store battery/steps values
    BatteryChargeState battery = battery_state_service_peek();
    int battery_percent = battery.charge_percent;
    if (battery_percent > 99) battery_percent = 99;

    HealthValue steps = health_service_sum_today(HealthMetricStepCount);
    int steps_thousands = steps / 1000;
    if (steps_thousands > 99) steps_thousands = 99;

    s_stored_hour_tens = steps_thousands / 10;
    s_stored_hour_ones = steps_thousands % 10;
    s_stored_min_tens = battery_percent / 10;
    s_stored_min_ones = battery_percent % 10;

    // Switch back to time (no timer needed since we're at time)
    s_display_mode = DISPLAY_TIME;
  }

  start_animation(false);

  // Set timer to return to time after 5 seconds (except if already at time)
  if (s_display_mode != DISPLAY_TIME) {
    s_return_to_time_timer = app_timer_register(5000, return_to_time_callback, NULL);
  }
}

static void tick_handler(struct tm *tick_time, TimeUnits units_changed) {
  // Start animation at the beginning of each minute (only when showing time)
  if (tick_time->tm_sec == 0 && s_display_mode == DISPLAY_TIME) {
    // Store the OLD time (which is the previous minute)
    // Since it just turned to a new minute, we need to calculate the previous minute
    int current_minutes = tick_time->tm_min;
    int current_hours = tick_time->tm_hour;

    // Go back one minute to get the old time
    int old_minutes = current_minutes - 1;
    int old_hours = current_hours;

    if (old_minutes < 0) {
      old_minutes = 59;
      old_hours = current_hours - 1;
      if (old_hours < 0) {
        old_hours = 23;
      }
    }

    s_stored_hour_tens = old_hours / 10;
    s_stored_hour_ones = old_hours % 10;
    s_stored_min_tens = old_minutes / 10;
    s_stored_min_ones = old_minutes % 10;

    start_animation(false);  // Full animation (shrink + grow)
  } else {
    layer_mark_dirty(s_display_layer);
  }
}

static void main_window_load(Window *window) {
  Layer *window_layer = window_get_root_layer(window);
  GRect bounds = layer_get_bounds(window_layer);

  // Create display layer
  s_display_layer = layer_create(bounds);
  layer_set_update_proc(s_display_layer, display_layer_update_proc);
  layer_add_child(window_layer, s_display_layer);

  // Trigger grow-only animation on startup
  start_animation(true);
}

static void main_window_unload(Window *window) {
  layer_destroy(s_display_layer);
}

static void init(void) {
  // Create main window
  s_main_window = window_create();
  window_set_window_handlers(s_main_window, (WindowHandlers) {
    .load = main_window_load,
    .unload = main_window_unload
  });
  window_stack_push(s_main_window, true);

  // Register with TickTimerService - use SECOND_UNIT to detect minute changes
  tick_timer_service_subscribe(SECOND_UNIT, tick_handler);

  // Register with AccelTapService for flick gestures
  accel_tap_service_subscribe(accel_tap_handler);
}

static void deinit(void) {
  // Cancel animation timer if running
  if (s_animation_timer) {
    app_timer_cancel(s_animation_timer);
    s_animation_timer = NULL;
  }

  // Cancel return-to-time timer if running
  if (s_return_to_time_timer) {
    app_timer_cancel(s_return_to_time_timer);
    s_return_to_time_timer = NULL;
  }

  // Unsubscribe from services
  accel_tap_service_unsubscribe();

  window_destroy(s_main_window);
}

int main(void) {
  init();
  app_event_loop();
  deinit();
}
