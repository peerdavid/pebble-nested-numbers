#include <pebble.h>

static Window *s_main_window;
static Layer *s_display_layer;

// Animation state
static AppTimer *s_animation_timer;
static int s_animation_frame = 0;
static bool s_is_animating = false;
static bool s_grow_only = false;  // If true, skip shrink phase

#define ANIMATION_FRAMES_SHRINK 16
#define ANIMATION_FRAMES_GROW 16
#define TOTAL_ANIMATION_FRAMES (ANIMATION_FRAMES_SHRINK + ANIMATION_FRAMES_GROW)
#define ANIMATION_FRAME_DURATION_MS 40

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
static void draw_distorted_segment(GContext *ctx, GPoint center, int segment_type, int digit_width, int digit_height, int segment_thickness, float scale) {
  // Apply scale to dimensions
  digit_width *= scale;
  digit_height *= scale;
  segment_thickness *= scale;
  
  // The "center" point is at 15% from top of the digit
  // This means the middle segment is only 15% down from top, creating a compressed top and huge bottom
  
  int segment_width = digit_width;
  
  // Calculate the actual center position (15% from top)
  // If digit spans from (center.y - h/2) to (center.y + h/2)
  // The middle should be at: top + 0.15 * height = (center.y - h/2) + 0.15*h
  // Which is: center.y - h/2 + 0.15*h = center.y + h*(-0.5 + 0.15) = center.y - 0.35*h
  int distorted_center_y = center.y - digit_height * 0.35;
  
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
        int mid_y = distorted_center_y;
        int x = center.x + segment_width / 2;
        
        points[0] = GPoint(x - segment_thickness, top_y);
        points[1] = GPoint(x, top_y);
        points[2] = GPoint(x, mid_y);
        points[3] = GPoint(x - segment_thickness, mid_y);
      }
      break;
      
    case 2: // Bottom-right vertical (from middle to bottom)
      {
        int mid_y = distorted_center_y;
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
        int mid_y = distorted_center_y;
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
        int mid_y = distorted_center_y;
        int x = center.x - segment_width / 2;
        
        points[0] = GPoint(x, top_y);
        points[1] = GPoint(x + segment_thickness, top_y);
        points[2] = GPoint(x + segment_thickness, mid_y);
        points[3] = GPoint(x, mid_y);
      }
      break;
      
    case 6: // Middle horizontal (at the distorted center - 15% from top!)
      {
        int y = distorted_center_y;
        points[0] = GPoint(center.x - segment_width / 2, y - segment_thickness / 2);
        points[1] = GPoint(center.x + segment_width / 2, y - segment_thickness / 2);
        points[2] = GPoint(center.x + segment_width / 2, y + segment_thickness / 2);
        points[3] = GPoint(center.x - segment_width / 2, y + segment_thickness / 2);
      }
      break;
  }
  
  segment_path_info.num_points = 4;
  segment_path_info.points = points;
  
  GPath *segment_path = gpath_create(&segment_path_info);
  gpath_draw_filled(ctx, segment_path);
  gpath_destroy(segment_path);
}

// Draw a normal (non-distorted) segment
static void draw_normal_segment(GContext *ctx, GPoint center, int segment_type, int digit_width, int digit_height, int segment_thickness, float scale) {
  // Apply scale to dimensions
  digit_width *= scale;
  digit_height *= scale;
  segment_thickness *= scale;
  
  int segment_width = digit_width;
  
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
      
    case 1: // Top-right vertical
      {
        int top_y = center.y - digit_height / 2;
        int mid_y = center.y;
        int x = center.x + segment_width / 2;
        
        points[0] = GPoint(x - segment_thickness, top_y);
        points[1] = GPoint(x, top_y);
        points[2] = GPoint(x, mid_y);
        points[3] = GPoint(x - segment_thickness, mid_y);
      }
      break;
      
    case 2: // Bottom-right vertical
      {
        int mid_y = center.y;
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
      
    case 4: // Bottom-left vertical
      {
        int mid_y = center.y;
        int bottom_y = center.y + digit_height / 2;
        int x = center.x - segment_width / 2;
        
        points[0] = GPoint(x, mid_y);
        points[1] = GPoint(x + segment_thickness, mid_y);
        points[2] = GPoint(x + segment_thickness, bottom_y);
        points[3] = GPoint(x, bottom_y);
      }
      break;
      
    case 5: // Top-left vertical
      {
        int top_y = center.y - digit_height / 2;
        int mid_y = center.y;
        int x = center.x - segment_width / 2;
        
        points[0] = GPoint(x, top_y);
        points[1] = GPoint(x + segment_thickness, top_y);
        points[2] = GPoint(x + segment_thickness, mid_y);
        points[3] = GPoint(x, mid_y);
      }
      break;
      
    case 6: // Middle horizontal
      {
        int y = center.y;
        points[0] = GPoint(center.x - segment_width / 2, y - segment_thickness / 2);
        points[1] = GPoint(center.x + segment_width / 2, y - segment_thickness / 2);
        points[2] = GPoint(center.x + segment_width / 2, y + segment_thickness / 2);
        points[3] = GPoint(center.x - segment_width / 2, y + segment_thickness / 2);
      }
      break;
  }
  
  segment_path_info.num_points = 4;
  segment_path_info.points = points;
  
  GPath *segment_path = gpath_create(&segment_path_info);
  gpath_draw_filled(ctx, segment_path);
  gpath_destroy(segment_path);
}

// Draw a single digit with distortion
static void draw_distorted_digit(GContext *ctx, int digit, GPoint center, int width, int height, int thickness, GColor color, float scale) {
  if (digit < 0 || digit > 9) return;
  if (scale <= 0.0f) return;  // Don't draw if scale is 0
  
  graphics_context_set_fill_color(ctx, color);
  
  for (int i = 0; i < 7; i++) {
    if (DIGIT_SEGMENTS[digit][i]) {
      draw_distorted_segment(ctx, center, i, width, height, thickness, scale);
    }
  }
}

// Draw a single normal (non-distorted) digit
static void draw_normal_digit(GContext *ctx, int digit, GPoint center, int width, int height, int thickness, GColor color, float scale) {
  if (digit < 0 || digit > 9) return;
  if (scale <= 0.0f) return;  // Don't draw if scale is 0
  
  graphics_context_set_fill_color(ctx, color);
  
  for (int i = 0; i < 7; i++) {
    if (DIGIT_SEGMENTS[digit][i]) {
      draw_normal_segment(ctx, center, i, width, height, thickness, scale);
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
  
  int hours = tick_time->tm_hour;
  int minutes = tick_time->tm_min;
  
  // Extract digits
  int hour_tens = hours / 10;
  int hour_ones = hours % 10;
  int min_tens = minutes / 10;
  int min_ones = minutes % 10;
  
  // hour_tens = 2;
  // hour_ones = 3;
  // min_tens = 3;
  // min_ones = 8;

  // Size calculations - each level is scaled to fit in the body (85% bottom portion) of previous
  int level1_width = 144-6;
  int level1_height = 168-10;
  GPoint level1_center = GPoint(bounds.size.w / 2, bounds.size.h / 2);
  
  // Level 2: positioned in the body of level 1 (the large bottom 85%)
  // The body center is at: level1_center.y + (level1_height * 0.35 / 2)
  // Body height is: level1_height * 0.85
  int level2_width = level1_width * 0.85;
  int level2_height = level1_height * 0.72;
  GPoint level2_center = GPoint(level1_center.x, 
                                level1_center.y + level1_height * 0.07);
  
  // Level 3: positioned in the body of level 2
  int level3_width = level2_width * 0.83;
  int level3_height = level2_height * 0.68;
  GPoint level3_center = GPoint(level2_center.x, 
                                level2_center.y + level2_height * 0.07);
  
  // Level 4: positioned in the body of level 3
  int level4_width = level3_width * 0.81;
  int level4_height = level3_height * 0.63;
  GPoint level4_center = GPoint(level3_center.x, 
                                level3_center.y + level3_height * 0.06);
  
  // Calculate scale factors for animation
  float scale_level0 = s_is_animating ? get_digit_scale(0, s_animation_frame) : 1.0f;
  float scale_level1 = s_is_animating ? get_digit_scale(1, s_animation_frame) : 1.0f;
  float scale_level2 = s_is_animating ? get_digit_scale(2, s_animation_frame) : 1.0f;
  float scale_level3 = s_is_animating ? get_digit_scale(3, s_animation_frame) : 1.0f;
  
  // Draw from largest to smallest
  draw_distorted_digit(ctx, min_ones, level1_center, level1_width, level1_height, 6, GColorWhite, scale_level3);
  draw_distorted_digit(ctx, min_tens, level2_center, level2_width, level2_height, 5, GColorLightGray, scale_level2);
  draw_distorted_digit(ctx, hour_ones, level3_center, level3_width, level3_height, 4, GColorWhite, scale_level1);
  draw_normal_digit(ctx, hour_tens, level4_center, level4_width, level4_height, 4, GColorLightGray, scale_level0);
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

static void tick_handler(struct tm *tick_time, TimeUnits units_changed) {
  // Start animation at the beginning of each minute
  if (tick_time->tm_sec == 0) {
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
}

static void deinit(void) {
  // Cancel animation timer if running
  if (s_animation_timer) {
    app_timer_cancel(s_animation_timer);
    s_animation_timer = NULL;
  }
  
  window_destroy(s_main_window);
}

int main(void) {
  init();
  app_event_loop();
  deinit();
}
