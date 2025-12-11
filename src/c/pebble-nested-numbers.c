#include <pebble.h>

static Window *s_main_window;
static Layer *s_display_layer;

// Animation state
static AppTimer *s_animation_timer;
static int s_animation_frame = 0;
static bool s_is_animating = false;
static bool s_grow_only = false;  // If true, skip shrink phase

// Date display state
static bool s_showing_date = false;
static AppTimer *s_date_timer = NULL;

// Store the time to display during animation
static int s_stored_hour_tens = 0;
static int s_stored_hour_ones = 0;
static int s_stored_min_tens = 0;
static int s_stored_min_ones = 0;

#define ANIMATION_FRAMES_SHRINK 16
#define ANIMATION_FRAMES_GROW 16
#define TOTAL_ANIMATION_FRAMES (ANIMATION_FRAMES_SHRINK + ANIMATION_FRAMES_GROW)
#define ANIMATION_FRAME_DURATION_MS 40
#define VERT_DISTORTION_FACTOR_R 2
#define VERT_DISTORTION_FACTOR_L 2

// Distortion constants - middle segment position as fraction from top
#define DISTORTION_LEVEL_1 0.35f  // Outermost digit (35% from top)
#define DISTORTION_LEVEL_2 0.30f  // Second level (30% from top)
#define DISTORTION_LEVEL_3 0.25f  // Third level (25% from top)
#define DISTORTION_LEVEL_4 0.50f  // Innermost digit (no distortion)

// Nested digit sizing ratios
#define MARGIN_HORIZONTAL 6   // Horizontal margin from screen edge
#define MARGIN_VERTICAL 10    // Vertical margin from screen edge
#define NESTING_SCALE_H 0.85f   // Each digit fits in 85% (body portion) of parent
#define NESTING_SCALE_W 0.9f   // Each digit fits in 85% (body portion) of parent


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
  
  // Calculate the actual center position (15% from top)
  // If digit spans from (center.y - h/2) to (center.y + h/2)
  // The middle should be at: top + 0.15 * height = (center.y - h/2) + 0.15*h
  // Which is: center.y - h/2 + 0.15*h = center.y + h*(-0.5 + 0.15) = center.y - 0.35*h
  int distorted_center_y = center.y - digit_height * distortion;
  
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
        
        points[0] = GPoint(x - segment_thickness - VERT_DISTORTION_FACTOR_R, top_y);
        points[1] = GPoint(x, top_y);
        points[2] = GPoint(x, mid_y);
        points[3] = GPoint(x - segment_thickness - VERT_DISTORTION_FACTOR_R, mid_y);
      }
      break;
      
    case 2: // Bottom-right vertical (from middle to bottom)
      {
        int mid_y = distorted_center_y;
        int bottom_y = center.y + digit_height / 2;
        int x = center.x + segment_width / 2;
        
        points[0] = GPoint(x - segment_thickness - VERT_DISTORTION_FACTOR_R, mid_y);
        points[1] = GPoint(x, mid_y);
        points[2] = GPoint(x, bottom_y);
        points[3] = GPoint(x - segment_thickness - VERT_DISTORTION_FACTOR_R, bottom_y);
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
        points[1] = GPoint(x + segment_thickness + VERT_DISTORTION_FACTOR_L, mid_y);
        points[2] = GPoint(x + segment_thickness + VERT_DISTORTION_FACTOR_L, bottom_y);
        points[3] = GPoint(x, bottom_y);
      }
      break;
      
    case 5: // Top-left vertical (from top to middle)
      {
        int top_y = center.y - digit_height / 2;
        int mid_y = distorted_center_y;
        int x = center.x - segment_width / 2;
        
        points[0] = GPoint(x, top_y);
        points[1] = GPoint(x + segment_thickness + VERT_DISTORTION_FACTOR_L, top_y);
        points[2] = GPoint(x + segment_thickness + VERT_DISTORTION_FACTOR_L, mid_y);
        points[3] = GPoint(x, mid_y);
      }
      break;
      
    case 6: // Middle horizontal (at the distorted center - 15% from top!)
      {
        int y = distorted_center_y;
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
        points[0] = GPoint(center.x - segment_width / 2, y+2);
        points[1] = GPoint(center.x + segment_width / 2, y+2);
        points[2] = GPoint(center.x + segment_width / 2, y + segment_thickness+2);
        points[3] = GPoint(center.x - segment_width / 2, y + segment_thickness+2);
      }
      break;
      
    case 1: // Top-right vertical
      {
        int top_y = center.y - digit_height / 2 + 2;
        int mid_y = center.y;
        int x = center.x + segment_width / 2;
        
        points[0] = GPoint(x - segment_thickness - VERT_DISTORTION_FACTOR_R, top_y);
        points[1] = GPoint(x, top_y);
        points[2] = GPoint(x, mid_y);
        points[3] = GPoint(x - segment_thickness - VERT_DISTORTION_FACTOR_R, mid_y);
      }
      break;
      
    case 2: // Bottom-right vertical
      {
        int mid_y = center.y;
        int bottom_y = center.y + digit_height / 2;
        int x = center.x + segment_width / 2;
        
        points[0] = GPoint(x - segment_thickness - VERT_DISTORTION_FACTOR_R, mid_y);
        points[1] = GPoint(x, mid_y);
        points[2] = GPoint(x, bottom_y);
        points[3] = GPoint(x - segment_thickness - VERT_DISTORTION_FACTOR_R, bottom_y);
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
        points[1] = GPoint(x + segment_thickness + VERT_DISTORTION_FACTOR_L, mid_y);
        points[2] = GPoint(x + segment_thickness + VERT_DISTORTION_FACTOR_L, bottom_y);
        points[3] = GPoint(x, bottom_y);
      }
      break;
      
    case 5: // Top-left vertical
      {
        int top_y = center.y - digit_height / 2 + 2;
        int mid_y = center.y;
        int x = center.x - segment_width / 2;
        
        points[0] = GPoint(x, top_y);
        points[1] = GPoint(x + segment_thickness + VERT_DISTORTION_FACTOR_L, top_y);
        points[2] = GPoint(x + segment_thickness + VERT_DISTORTION_FACTOR_L, mid_y);
        points[3] = GPoint(x, mid_y);
      }
      break;
      
    case 6: // Middle horizontal
      {
        int y = center.y;
        points[0] = GPoint(center.x - segment_width / 2, y - segment_thickness/2);
        points[1] = GPoint(center.x + segment_width / 2, y - segment_thickness/2);
        points[2] = GPoint(center.x + segment_width / 2, y + segment_thickness/2+1);
        points[3] = GPoint(center.x - segment_width / 2, y + segment_thickness/2+1);
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
  float distortion;
} DigitLayout;

// Calculate nested digit layouts based on screen bounds and distortion
static void calculate_digit_layouts(GRect bounds, DigitLayout layouts[4], int digits[4]) {
  // Level 0 (outermost): Fill screen without margins
  layouts[0].width = bounds.size.w;
  layouts[0].height = bounds.size.h;
  layouts[0].center = GPoint(bounds.size.w / 2, bounds.size.h / 2);
  layouts[0].thickness = 6;
  layouts[0].distortion = DISTORTION_LEVEL_1;
  
  // For each subsequent level, nest within parent's body region
  for (int level = 1; level < 4; level++) {
    DigitLayout *parent = &layouts[level - 1];
    DigitLayout *current = &layouts[level];
    
    // Calculate where parent's middle segment is located
    int parent_middle_y = parent->center.y - parent->height * parent->distortion;

    // Check if parent digit has no middle segment (0, 1, 7)
    int parent_digit = digits[level - 1];
    bool is_0_1_or_7 = (parent_digit == 0 || parent_digit == 1 || parent_digit == 7);
    if(is_0_1_or_7){
      parent_middle_y = parent->center.y - parent->height/2 + parent->thickness; // Move middle_y to just below top segment
    }
    
    // Calculate parent's body region (from middle segment to bottom)
    int parent_top = parent->center.y - parent->height / 2;
    int parent_bottom = parent->center.y + parent->height / 2;
    int body_top = parent_middle_y - parent->thickness;
    int body_bottom = parent_bottom + parent->thickness + 3-level;
    int body_height = body_bottom - body_top;
    
    // Position current digit in center of parent's body region
    current->center.x = parent->center.x;
    current->center.y = body_top + body_height / 2 - (parent->thickness) / 2 - 1;
    
    // Scale current digit to fit within parent's body using NESTING_SCALE
    current->height = (body_height * NESTING_SCALE_H) - level * 2 - 12;
    // Width scales proportionally from parent's width, not from height
    current->width = (parent->width * NESTING_SCALE_W) - level * 2 - 12;
    
    // For now lets fix the thickness
    current->thickness = 6;
    
    // Set distortion based on level
    if (level == 1) current->distortion = DISTORTION_LEVEL_2;
    else if (level == 2) current->distortion = DISTORTION_LEVEL_3;
    else current->distortion = DISTORTION_LEVEL_4; // No distortion for innermost
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
  
  int digit_0, digit_1, digit_2, digit_3;
  
  if (s_showing_date) {
    // Show date: month and day (MM-DD)
    int month = tick_time->tm_mon + 1;  // tm_mon is 0-11
    int day = tick_time->tm_mday;
    
    digit_0 = month / 10;
    digit_1 = month % 10;
    digit_2 = day / 10;
    digit_3 = day % 10;
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
  bool is_pause = (s_animation_frame == 0);
  
  // During animation, use stored old time during shrink, new time during grow
  if (is_shrinking && s_is_animating && !s_grow_only) {
    hour_tens = s_stored_hour_tens;
    hour_ones = s_stored_hour_ones;
    min_tens = s_stored_min_tens;
    min_ones = s_stored_min_ones;
  }
  
  // Screenshot 1
  // hour_tens = 2;
  // hour_ones = 3;
  // min_tens = 3;
  // min_ones = 8;

  // Screenshot 2
  // hour_tens = 1;
  // hour_ones = 9;
  // min_tens = 1;
  // min_ones = 8;

  // Extrema 1
  // hour_tens = 8;
  // hour_ones = 8;
  // min_tens = 8;
  // min_ones = 8;

  // Extrema 2
  // hour_tens = 0;
  // hour_ones = 0;
  // min_tens = 0;
  // min_ones = 0;
  
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
  // Colors are white gray white gray for time and gray gray white white for date
  GColor colors[4];
  colors[0] = s_showing_date && is_pause ? GColorWhite : GColorWhite;      // min_ones
  colors[1] = s_showing_date && is_pause ? GColorWhite : GColorLightGray;  // min_tens
  colors[2] = s_showing_date && is_pause ? GColorLightGray : GColorWhite;  // hour_ones
  colors[3] = s_showing_date && is_pause ? GColorLightGray : GColorLightGray; // hour_tens
  
  float scales[4] = {scale_level3, scale_level2, scale_level1, scale_level0};
  
  // Draw digits from outermost to innermost
  for (int i = 0; i < 4; i++) {
    if (i < 3) {
      // First three levels use distortion
      draw_distorted_digit(ctx, digits[i], layouts[i].center, layouts[i].width, 
                          layouts[i].height, layouts[i].thickness, colors[i], 
                          scales[i], layouts[i].distortion);
    } else {
      // Innermost level (hour_tens) has no distortion
      draw_normal_digit(ctx, digits[i], layouts[i].center, layouts[i].width, 
                       layouts[i].height, layouts[i].thickness, colors[i], scales[i]);
    }
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

static void date_timer_callback(void *data) {
  // Switch back to time display
  s_date_timer = NULL;
  
  // Store current date values for animation
  time_t temp = time(NULL);
  struct tm *tick_time = localtime(&temp);
  int month = tick_time->tm_mon + 1;
  int day = tick_time->tm_mday;
  
  s_stored_hour_tens = month / 10;
  s_stored_hour_ones = month % 10;
  s_stored_min_tens = day / 10;
  s_stored_min_ones = day % 10;
  
  s_showing_date = false;
  start_animation(false);  // Animate from date to time
}

static void accel_tap_handler(AccelAxisType axis, int32_t direction) {
  // Handle flick gesture
  if (s_showing_date) {
    // Already showing date, cancel timer and extend display
    if (s_date_timer) {
      app_timer_cancel(s_date_timer);
    }
    s_date_timer = app_timer_register(5000, date_timer_callback, NULL);
  } else {
    // Store current time values for animation
    time_t temp = time(NULL);
    struct tm *tick_time = localtime(&temp);
    int hours = tick_time->tm_hour;
    int minutes = tick_time->tm_min;
    
    s_stored_hour_tens = hours / 10;
    s_stored_hour_ones = hours % 10;
    s_stored_min_tens = minutes / 10;
    s_stored_min_ones = minutes % 10;
    
    s_showing_date = true;
    start_animation(false);  // Animate from time to date
    
    // Set timer to switch back to time after 5 seconds
    s_date_timer = app_timer_register(5000, date_timer_callback, NULL);
  }
}

static void tick_handler(struct tm *tick_time, TimeUnits units_changed) {
  // Start animation at the beginning of each minute (only when showing time)
  if (tick_time->tm_sec == 0 && !s_showing_date) {
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
  
  // Cancel date timer if running
  if (s_date_timer) {
    app_timer_cancel(s_date_timer);
    s_date_timer = NULL;
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
