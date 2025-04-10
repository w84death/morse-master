#include <furi.h>
#include <gui/gui.h>
#include <gui/elements.h>
#include <input/input.h>
#include <notification/notification_messages.h>
#include <furi_hal.h>
#include <furi_hal_rtc.h>
#include <furi_hal_speaker.h>
#include <string.h>
#include <ctype.h>

// Include icons
#include "p1x_morse_master_icons.h"

// Timing Configuration (in milliseconds)
#define DOT_DURATION_MS 150
#define DASH_DURATION_MS 300
#define ELEMENT_SPACE_MS 100       // Space between dots and dashes
#define CHAR_SPACE_MS 300          // Space between characters
#define WORD_SPACE_MS 1000         // Space between words
#define DECODE_TIMEOUT_MS 2000     // Time after which decoder tries to decode (was 3000ms)
#define MAX_MORSE_LENGTH 6        // Maximum length of morse code input
#define TOP_WORDS_MAX_LENGTH 16    // Maximum length for top words marquee display
#define INITIAL_VOLUME 0.25f      // Initial volume level (0.0 to 1.0)
#define DEFAULT_FREQUENCY 800

// Application states
typedef enum {
    MorseStateTitleScreen,
    MorseStateMenu,        // Main menu with icons
    MorseStateLearn,
    MorseStatePractice,
    MorseStateHelp,
    MorseStateExit
} MorseAppState;

// Sound command types
typedef enum {
    SoundCommandNone,
    SoundCommandDot,
    SoundCommandDash,
    SoundCommandCharacter
} SoundCommand;

// Morse code structure
typedef struct {
    char character;
    const char* code;
} MorseCode;

// Main application structure
typedef struct {
    // UI elements
    Gui* gui;
    ViewPort* view_port;
    FuriMessageQueue* event_queue;
    NotificationApp* notifications;
    
    // Sound processing
    FuriThread* sound_thread;
    FuriMessageQueue* sound_queue;
    bool sound_running;
    SoundCommand current_sound;
    char sound_character;
    float volume;  // Volume level from 0.0 to 1.0
    
    // Application state
    MorseAppState app_state;
    int menu_selection;
    bool is_running;
    
    // Learning
    char current_char;
    char user_input[MAX_MORSE_LENGTH];
    int input_position;
    bool learning_letters_mode;  // True for letters, false for numbers
    
    // Practice
    uint32_t last_input_time;
    char decoded_text[MAX_MORSE_LENGTH];
    char top_words[TOP_WORDS_MAX_LENGTH + 1];  // Buffer for marquee display
    char current_morse[MAX_MORSE_LENGTH];
    int current_morse_position;
    bool auto_add_space;
    char last_decoded_char;  // Store the last decoded character
} MorseApp;

// International Morse Code mappings (simplified subset)
static const MorseCode MORSE_TABLE[] = {
    {'A', ".-"},
    {'B', "-..."},
    {'C', "-.-."},
    {'D', "-.."},
    {'E', "."},
    {'F', "..-."},
    {'G', "--."},
    {'H', "...."},
    {'I', ".."},
    {'J', ".---"},
    {'K', "-.-"},
    {'L', ".-.."},
    {'M', "--"},
    {'N', "-."},
    {'O', "---"},
    {'P', ".--."},
    {'Q', "--.-"},
    {'R', ".-."},
    {'S', "..."},
    {'T', "-"},
    {'U', "..-"},
    {'V', "...-"},
    {'W', ".--"},
    {'X', "-..-"},
    {'Y', "-.--"},
    {'Z', "--.."},
    {'0', "-----"},
    {'1', ".----"},
    {'2', "..---"},
    {'3', "...--"},
    {'4', "....-"},
    {'5', "....."},
    {'6', "-...."},
    {'7', "--..."},
    {'8', "---.."},
    {'9', "----."},
};

// Function prototypes
static void morse_app_draw_callback(Canvas* canvas, void* ctx);
static void morse_app_input_callback(InputEvent* input_event, void* ctx);
static void play_dot(MorseApp* app);
static void play_dash(MorseApp* app);
static void play_character(MorseApp* app, char ch);
static const char* get_morse_for_char(char c);
static int32_t sound_worker_thread(void* context);
static char get_char_for_morse(const char* morse);
static void try_decode_morse(MorseApp* app);
static void draw_help_screen(Canvas* canvas, MorseApp* app);
static void draw_title_screen(Canvas* canvas, MorseApp* app);
static void update_top_words_marquee(MorseApp* app, char new_char);

// Get morse code for a character
static const char* get_morse_for_char(char c) {
    c = toupper(c);
    
    for(size_t i = 0; i < sizeof(MORSE_TABLE) / sizeof(MORSE_TABLE[0]); i++) {
        if(MORSE_TABLE[i].character == c) {
            return MORSE_TABLE[i].code;
        }
    }
    
    return NULL;
}

// Get character for a morse code
static char get_char_for_morse(const char* morse) {
    for(size_t i = 0; i < sizeof(MORSE_TABLE) / sizeof(MORSE_TABLE[0]); i++) {
        if(strcmp(MORSE_TABLE[i].code, morse) == 0) {
            return MORSE_TABLE[i].character;
        }
    }
    
    return '?';  // Unknown morse code
}

// Sound worker thread function - handles all audio output
static int32_t sound_worker_thread(void* context) {
    MorseApp* app = (MorseApp*)context;
    SoundCommand command;
    
    while(app->sound_running) {
        // Wait for a sound command
        if(furi_message_queue_get(app->sound_queue, &command, 100) == FuriStatusOk) {
            // Process sound command
            switch(command) {
                case SoundCommandDot:
                    // Play dot
                    if(furi_hal_speaker_acquire(1000)) {
                        furi_hal_speaker_start(DEFAULT_FREQUENCY, app->volume); // Use volume setting
                        notification_message(app->notifications, &sequence_set_only_red_255);
                        furi_delay_ms(DOT_DURATION_MS);
                        furi_hal_speaker_stop();
                        notification_message(app->notifications, &sequence_reset_red);
                        furi_hal_speaker_release();
                    }
                    furi_delay_ms(ELEMENT_SPACE_MS);
                    break;
                    
                case SoundCommandDash:
                    // Play dash
                    if(furi_hal_speaker_acquire(1000)) {
                        furi_hal_speaker_start(DEFAULT_FREQUENCY, app->volume); // Use volume setting
                        notification_message(app->notifications, &sequence_set_only_blue_255);
                        furi_delay_ms(DASH_DURATION_MS);
                        furi_hal_speaker_stop();
                        notification_message(app->notifications, &sequence_reset_blue);
                        furi_hal_speaker_release();
                    }
                    furi_delay_ms(ELEMENT_SPACE_MS);
                    break;
                    
                case SoundCommandCharacter:
                    // Play a full character
                    {
                        const char* morse = get_morse_for_char(app->sound_character);
                        if(morse) {
                            // Start playing immediately with first element
                            if(morse[0] != '\0') {
                                if(morse[0] == '.') {
                                    // Play dot directly for immediate feedback
                                    if(furi_hal_speaker_acquire(1000)) {
                                        furi_hal_speaker_start(DEFAULT_FREQUENCY, app->volume);
                                        notification_message(app->notifications, &sequence_set_only_red_255);
                                        furi_delay_ms(DOT_DURATION_MS);
                                        furi_hal_speaker_stop();
                                        notification_message(app->notifications, &sequence_reset_red);
                                        furi_hal_speaker_release();
                                    }
                                    // Only add a small pause between elements without full delay
                                    furi_delay_ms(ELEMENT_SPACE_MS / 2);
                                } else if(morse[0] == '-') {
                                    // Play dash directly for immediate feedback
                                    if(furi_hal_speaker_acquire(1000)) {
                                        furi_hal_speaker_start(DEFAULT_FREQUENCY, app->volume);
                                        notification_message(app->notifications, &sequence_set_only_blue_255);
                                        furi_delay_ms(DASH_DURATION_MS);
                                        furi_hal_speaker_stop();
                                        notification_message(app->notifications, &sequence_reset_blue);
                                        furi_hal_speaker_release();
                                    }
                                    // Only add a small pause between elements without full delay
                                    furi_delay_ms(ELEMENT_SPACE_MS / 2);
                                }
                            }
                            
                            // Queue the rest of the elements - directly play them without queuing
                            for(size_t i = 1; morse[i] != '\0'; i++) {
                                if(morse[i] == '.') {
                                    // Play dot directly without queueing
                                    if(furi_hal_speaker_acquire(1000)) {
                                        furi_hal_speaker_start(DEFAULT_FREQUENCY, app->volume);
                                        notification_message(app->notifications, &sequence_set_only_red_255);
                                        furi_delay_ms(DOT_DURATION_MS);
                                        furi_hal_speaker_stop();
                                        notification_message(app->notifications, &sequence_reset_red);
                                        furi_hal_speaker_release();
                                    }
                                    // Normal delay between elements 
                                    furi_delay_ms(ELEMENT_SPACE_MS);
                                } else if(morse[i] == '-') {
                                    // Play dash directly without queueing
                                    if(furi_hal_speaker_acquire(1000)) {
                                        furi_hal_speaker_start(DEFAULT_FREQUENCY, app->volume);
                                        notification_message(app->notifications, &sequence_set_only_blue_255);
                                        furi_delay_ms(DASH_DURATION_MS);
                                        furi_hal_speaker_stop();
                                        notification_message(app->notifications, &sequence_reset_blue);
                                        furi_hal_speaker_release();
                                    }
                                    // Normal delay between elements
                                    furi_delay_ms(ELEMENT_SPACE_MS);
                                }
                            }
                        }
                    }
                    break;
                    
                default:
                    break;
            }
        }
        
        furi_delay_ms(10);
    }
    
    return 0;
}

// Redefined sound functions that queue sound commands instead of playing directly

// Play dot sound and visual
static void play_dot(MorseApp* app) {
    SoundCommand cmd = SoundCommandDot;
    furi_message_queue_put(app->sound_queue, &cmd, 0);
}

// Play dash sound and visual
static void play_dash(MorseApp* app) {
    SoundCommand cmd = SoundCommandDash;
    furi_message_queue_put(app->sound_queue, &cmd, 0);
}

// Play a complete morse character
static void play_character(MorseApp* app, char ch) {
    app->sound_character = ch;
    SoundCommand cmd = SoundCommandCharacter;
    furi_message_queue_put(app->sound_queue, &cmd, 0);
}

// Function to try to decode current morse code if there's been a pause
static void try_decode_morse(MorseApp* app) {
    uint32_t current_time = furi_hal_rtc_get_timestamp();
    
    // Check if enough time has passed since last input (pause detected)
    if(app->last_input_time > 0 && 
       (current_time - app->last_input_time) >= (DECODE_TIMEOUT_MS / 1000) && 
       app->current_morse_position > 0) {
        
        // Null terminate the current morse code
        app->current_morse[app->current_morse_position] = '\0';
        
        // Decode the morse code to a character
        char decoded = get_char_for_morse(app->current_morse);
        
        // Store the last decoded character (regardless of validity)
        app->last_decoded_char = decoded;
        
        // If we got a valid character, add it to the decoded text
        if(decoded != '?') {
            // Add to decoded_text (used for internal tracking, limited by MAX_MORSE_LENGTH)
            size_t len = strlen(app->decoded_text);
            if(len < MAX_MORSE_LENGTH - 1) {
                app->decoded_text[len] = decoded;
                app->decoded_text[len + 1] = '\0';
                
                // Set flag to add space automatically on next input
                app->auto_add_space = true;
            }
            
            // Update the top_words marquee (this handles the marquee effect)
            update_top_words_marquee(app, decoded);
            
            // Copy the decoded character to user_input display
            if (app->input_position < MAX_MORSE_LENGTH - 1) {
                app->user_input[app->input_position++] = decoded;
                app->user_input[app->input_position] = '\0';
            }
        }
        
        // Reset the current morse code for next letter
        app->current_morse_position = 0;
        app->current_morse[0] = '\0';
    }
}

// Draw application UI based on current state
static void morse_app_draw_callback(Canvas* canvas, void* ctx) {
    MorseApp* app = ctx;
    if(!app || !canvas) return;
    
    canvas_clear(canvas);
    // Make background black
    canvas_set_color(canvas, ColorBlack);
    canvas_draw_box(canvas, 0, 0, 128, 64);
    canvas_draw_icon(canvas, 0, 45, &I_menu_bg);

    switch(app->app_state) {
        case MorseStateTitleScreen:
            draw_title_screen(canvas, app);
            break;
            
        case MorseStateMenu: {
            canvas_draw_icon(canvas, 0, 45, &I_menu_bg);

            canvas_set_color(canvas, ColorWhite);
            // Define menu items and their corresponding titles
            const char* menu_titles[] = {"LEARN", "PRACTICE", "HELP"};
            
            // Display title based on current selection at the top
            canvas_set_font(canvas, FontPrimary);
            canvas_draw_str_aligned(canvas, 64, 10, AlignCenter, AlignCenter, menu_titles[app->menu_selection]);
            
            const int16_t y_offset = 24;

            // Draw rectangle around the selected icon (instead of triangles)
            canvas_draw_frame(
                canvas,
                8+app->menu_selection*40,  // x 
                y_offset-4, // y
                32,                 // width
                28                  // height
            );
            canvas_set_color(canvas, ColorBlack);
            
            canvas_draw_icon(canvas, 12, y_offset, &I_learn);
            canvas_draw_icon(canvas, 54, y_offset, &I_practice);
            canvas_draw_icon(canvas, 94, y_offset, &I_help);

            break;
        }
        
        case MorseStateLearn: {
            canvas_draw_icon(canvas, 5, 17, &I_learning_bg);
            
            canvas_set_font(canvas, FontPrimary);

            // Display current character
            char txt[32];
            snprintf(txt, sizeof(txt), "%c", app->current_char);
            canvas_draw_str(canvas, 20, 33, txt);
            
            // Display Morse code
            const char* morse = get_morse_for_char(app->current_char);
            snprintf(txt, sizeof(txt), "%s", morse ? morse : "");
            canvas_draw_str(canvas, 75, 33, txt);
            
            canvas_set_color(canvas, ColorWhite);

            // Display instructions
            canvas_draw_str(canvas, 5, 12, "< Prev");
            canvas_draw_str(canvas, 70, 12, "Next >");
            break;
        }
        
        case MorseStatePractice: {
            // Draw practice screen
            canvas_draw_str(canvas, 2, 10, "PRACTICE");
            canvas_draw_line(canvas, 0, 12, 128, 12);
            
            // Display top words marquee (new method)
            canvas_set_font(canvas, FontPrimary);
            
            // Try to decode if there's a pause
            try_decode_morse(app);
            
            // Display words  (old)
            canvas_draw_str(canvas, 5, 25, app->top_words);
            
            // Display current morse being decoded
            char current_status[64];
            snprintf(current_status, sizeof(current_status), "Current: %s", app->current_morse);
            canvas_draw_str(canvas, 5, 49, current_status);
            
            // Display decoded text with last decoded character
            char decoded_line[MAX_MORSE_LENGTH + 20];
            if (app->last_decoded_char == '?') {
                snprintf(decoded_line, sizeof(decoded_line), "Decoded: [unknown]");
            } else if (app->last_decoded_char == 0) {
                snprintf(decoded_line, sizeof(decoded_line), "Decoded:");
            } else {
                snprintf(decoded_line, sizeof(decoded_line), "Decoded: %c", app->last_decoded_char);
            }
            canvas_draw_str(canvas, 5, 61, decoded_line);
            
            // Display volume level
            char volume_str[20];
            int volume_bars = (int)(app->volume * 10);
            snprintf(volume_str, sizeof(volume_str), "Vol: [");
            for(int i = 0; i < 10; i++) {
                if(i < volume_bars) {
                    snprintf(volume_str + strlen(volume_str), sizeof(volume_str) - strlen(volume_str), "|");
                } else {
                    snprintf(volume_str + strlen(volume_str), sizeof(volume_str) - strlen(volume_str), " ");
                }
            }
            snprintf(volume_str + strlen(volume_str), sizeof(volume_str) - strlen(volume_str), "]");
            canvas_set_font(canvas, FontSecondary);
            canvas_draw_str(canvas, 5, 75, volume_str);
            
            // Display instructions
            canvas_draw_str(canvas, 5, 94, "OK: Short=Dot Long=Dash");
            canvas_draw_str(canvas, 5, 104, "LEFT: Space  RIGHT: Clear");
            break;
        }
        
        case MorseStateHelp: {
            draw_help_screen(canvas, app);
            break;
        }
        
        default:
            break;
    }
}

// Handle user input
static void morse_app_input_callback(InputEvent* input_event, void* ctx) {
    MorseApp* app = ctx;
    if(!app || !input_event) return;
    
    // Update last input time for practice mode
    if(app->app_state == MorseStatePractice && 
       (input_event->key == InputKeyOk || input_event->key == InputKeyRight)) {
        app->last_input_time = furi_hal_rtc_get_timestamp();
    }
    
    switch(app->app_state) {
        case MorseStateTitleScreen:
            // Any button press on title screen advances to main menu
            if(input_event->type == InputTypeShort || input_event->type == InputTypeLong) {
                app->app_state = MorseStateMenu;
            }
            break;
            
        case MorseStateMenu:
            if(input_event->key == InputKeyLeft && input_event->type == InputTypeShort) {
                // Move selection left (with wrap-around)
                app->menu_selection = (app->menu_selection > 0) ? 
                    app->menu_selection - 1 : 2; // Wrap to last item
            }
            else if(input_event->key == InputKeyRight && input_event->type == InputTypeShort) {
                // Move selection right (with wrap-around)
                app->menu_selection = (app->menu_selection < 2) ? 
                    app->menu_selection + 1 : 0; // Wrap to first item
            }
            else if(input_event->key == InputKeyOk && input_event->type == InputTypeShort) {
                // Open the selected option
                switch(app->menu_selection) {
                    case 0: // Learn
                        app->app_state = MorseStateLearn;
                        break;
                        
                    case 1: // Practice
                        app->app_state = MorseStatePractice;
                        memset(app->user_input, 0, sizeof(app->user_input));
                        break;
                        
                    case 2: // Help
                        app->app_state = MorseStateHelp;
                        break;
                }
            }
            else if(input_event->key == InputKeyBack && input_event->type == InputTypeShort) {
                app->is_running = false;
            }
            break;
            
        case MorseStateLearn:
            if(input_event->key == InputKeyOk && input_event->type == InputTypeShort) {
                // Play the character's morse code
                play_character(app, app->current_char);
            }
            else if(input_event->key == InputKeyLeft && input_event->type == InputTypeShort) {
                // Switch to letters mode
                app->learning_letters_mode = true;
                app->current_char = 'A';
            }
            else if(input_event->key == InputKeyRight && input_event->type == InputTypeShort) {
                // Switch to numbers mode
                app->learning_letters_mode = false;
                app->current_char = '0';
            }
            else if(input_event->key == InputKeyDown && input_event->type == InputTypeShort) {
                // Show next character based on current mode
                if (app->learning_letters_mode) {
                    // Letters mode
                    if(app->current_char == 'Z') 
                        app->current_char = 'A';
                    else
                        app->current_char++;
                } else {
                    // Numbers mode
                    if(app->current_char == '9')
                        app->current_char = '0';
                    else 
                        app->current_char++;
                }
            }
            else if(input_event->key == InputKeyUp && input_event->type == InputTypeShort) {
                // Show previous character based on current mode
                if (app->learning_letters_mode) {
                    // Letters mode
                    if(app->current_char == 'A')
                        app->current_char = 'Z';
                    else
                        app->current_char--;
                } else {
                    // Numbers mode
                    if(app->current_char == '0')
                        app->current_char = '9';
                    else
                        app->current_char--;
                }
            }
            else if(input_event->key == InputKeyBack && input_event->type == InputTypeShort) {
                app->app_state = MorseStateMenu;
            }
            break;
            
        case MorseStatePractice:
            if(input_event->key == InputKeyOk || input_event->key == InputKeyLeft) {
                // Check if we need to add auto space from previous decode
                if(app->auto_add_space) {
                    size_t len = strlen(app->decoded_text);
                    if(len < MAX_MORSE_LENGTH - 1) {
                        app->decoded_text[len] = ' ';
                        app->decoded_text[len + 1] = '\0';
                    }
                    app->auto_add_space = false;
                }
                
                // Update last input time
                app->last_input_time = furi_hal_rtc_get_timestamp();
                
                // Check if input has reached MAX_MORSE_LENGTH
                if(app->input_position >= MAX_MORSE_LENGTH - 1) {
                    // Clear all input to prevent buffer overflow
                    memset(app->user_input, 0, sizeof(app->user_input));
                    app->input_position = 0;
                    
                    // Show visual feedback (flash green LED) to indicate input was cleared
                    notification_message(app->notifications, &sequence_set_only_green_255);
                    furi_delay_ms(200);
                    notification_message(app->notifications, &sequence_reset_green);
                }
                
                if(input_event->type == InputTypeShort) {
                    // Short press OK - Add dot to input if there's room
                    if(strlen(app->user_input) < MAX_MORSE_LENGTH - 1) {
                        app->user_input[app->input_position++] = '.';
                        app->user_input[app->input_position] = '\0';
                        
                        // Also add to current morse code being decoded
                        if(app->current_morse_position < MAX_MORSE_LENGTH - 1) {
                            app->current_morse[app->current_morse_position++] = '.';
                            app->current_morse[app->current_morse_position] = '\0';
                        }
                        
                        play_dot(app);
                    }
                } 
                else if(input_event->type == InputTypeLong) {
                    // Long press OK - Add dash to input if there's room
                    if(strlen(app->user_input) < MAX_MORSE_LENGTH - 1) {
                        app->user_input[app->input_position++] = '-';
                        app->user_input[app->input_position] = '\0';
                        
                        // Also add to current morse code being decoded
                        if(app->current_morse_position < MAX_MORSE_LENGTH - 1) {
                            app->current_morse[app->current_morse_position++] = '-';
                            app->current_morse[app->current_morse_position] = '\0';
                        }
                        
                        play_dash(app);
                    }
                }
            }
            else if(input_event->key == InputKeyRight && input_event->type == InputTypeShort) {
                // Clear all input
                memset(app->user_input, 0, sizeof(app->user_input));
                app->input_position = 0;
                
                memset(app->decoded_text, 0, sizeof(app->decoded_text));
                memset(app->current_morse, 0, sizeof(app->current_morse));
                app->current_morse_position = 0;
                app->auto_add_space = false;
                app->last_input_time = 0;
            }
            else if(input_event->key == InputKeyUp && input_event->type == InputTypeShort) {
                // Increase volume (with upper limit)
                app->volume = (app->volume < 1.0f) ? app->volume + 0.1f : 1.0f;
                notification_message(app->notifications, &sequence_set_only_green_255);
                furi_delay_ms(100);
                notification_message(app->notifications, &sequence_reset_green);
            }
            else if(input_event->key == InputKeyDown && input_event->type == InputTypeShort) {
                // Decrease volume (with lower limit)
                app->volume = (app->volume > 0.1f) ? app->volume - 0.1f : 0.1f;
                notification_message(app->notifications, &sequence_set_only_green_255);
                furi_delay_ms(100);
                notification_message(app->notifications, &sequence_reset_green);
            }
            else if(input_event->key == InputKeyBack && input_event->type == InputTypeShort) {
                app->app_state = MorseStateMenu;
            }
            break;
        
        case MorseStateHelp:
            if(input_event->key == InputKeyBack && input_event->type == InputTypeShort) {
                app->app_state = MorseStateMenu;
            }
            break;
            
        default:
            break;
    }
    
    view_port_update(app->view_port);
}

// Entry point for Morse Master application
int32_t p1x_morse_master_app(void* p) {
    UNUSED(p);
    FURI_LOG_I("MorseMaster", "Application starting");
    
    MorseApp* app = malloc(sizeof(MorseApp));
    if(!app) {
        FURI_LOG_E("MorseMaster", "Failed to allocate memory");
        return 255;
    }
    
    // Initialize application structure
    memset(app, 0, sizeof(MorseApp));
    
    // Allocate required resources
    app->gui = furi_record_open(RECORD_GUI);
    app->view_port = view_port_alloc();
    app->event_queue = furi_message_queue_alloc(8, sizeof(InputEvent));
    app->notifications = furi_record_open(RECORD_NOTIFICATION);
    app->sound_queue = furi_message_queue_alloc(8, sizeof(SoundCommand));
    
    // Check if all resources were allocated
    if(!app->view_port || !app->event_queue || !app->sound_queue) {
        FURI_LOG_E("MorseMaster", "Failed to allocate resources");
        if(app->view_port) view_port_free(app->view_port);
        if(app->event_queue) furi_message_queue_free(app->event_queue);
        if(app->sound_queue) furi_message_queue_free(app->sound_queue);
        if(app->gui) furi_record_close(RECORD_GUI);
        if(app->notifications) furi_record_close(RECORD_NOTIFICATION);
        free(app);
        return 255;
    }
    
    // Set up default values
    app->app_state = MorseStateTitleScreen;  // Start with title screen
    app->menu_selection = 0;
    app->is_running = true;
    app->sound_running = true;
    app->current_char = 'A'; // Start with A instead of E
    app->learning_letters_mode = true; // Start in letters mode
    app->input_position = 0;
    app->last_input_time = 0;
    app->current_morse_position = 0;
    app->auto_add_space = false;
    app->volume = INITIAL_VOLUME; // Initialize volume to max
    memset(app->user_input, 0, sizeof(app->user_input));
    memset(app->decoded_text, 0, sizeof(app->decoded_text));
    memset(app->top_words, 0, sizeof(app->top_words));
    memset(app->current_morse, 0, sizeof(app->current_morse));
    
    // Configure viewport
    view_port_draw_callback_set(app->view_port, morse_app_draw_callback, app);
    view_port_input_callback_set(app->view_port, morse_app_input_callback, app);
    gui_add_view_port(app->gui, app->view_port, GuiLayerFullscreen);
    
    // Create and start sound worker thread
    app->sound_thread = furi_thread_alloc_ex("MorseSoundWorker", 1024, sound_worker_thread, app);
    furi_thread_start(app->sound_thread);
    
    // Seed RNG
    srand(furi_hal_rtc_get_timestamp());
    
    // Main event loop
    while(app->is_running) {
        InputEvent event;
        // Wait for input with a timeout
        if(furi_message_queue_get(app->event_queue, &event, 100) == FuriStatusOk) {
            morse_app_input_callback(&event, app);
        } else {
            // No input - check if we need to decode morse after a pause
            if(app->app_state == MorseStatePractice) {
                view_port_update(app->view_port);  // Update to check for decoding
            }
        }
        furi_delay_ms(5); // Small delay to prevent CPU hogging
    }
    
    // Signal sound thread to stop and wait for it to finish
    app->sound_running = false;
    furi_thread_join(app->sound_thread);
    furi_thread_free(app->sound_thread);
    
    // Free resources
    view_port_enabled_set(app->view_port, false);
    gui_remove_view_port(app->gui, app->view_port);
    view_port_free(app->view_port);
    furi_message_queue_free(app->event_queue);
    furi_message_queue_free(app->sound_queue);
    furi_record_close(RECORD_GUI);
    furi_record_close(RECORD_NOTIFICATION);
    free(app);
    
    return 0;
}

static void draw_help_screen(Canvas* canvas, MorseApp* app) {
    UNUSED(app);  // Mark parameter as unused to prevent compiler warning
    
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str(canvas, 2, 10, "MORSE CODE HELP");
    canvas_draw_line(canvas, 0, 12, 128, 12);
    
    canvas_set_font(canvas, FontSecondary);
    
    int16_t y_offset = 22;
    canvas_draw_str(canvas, 2, y_offset, "PRACTICE MODE:");
    y_offset += 10;
    canvas_draw_str(canvas, 2, y_offset, "- OK and LEFT:");
    y_offset += 10;
    canvas_draw_str(canvas, 2, y_offset, "    Short=dot, Long=dash");
    y_offset += 10;
    canvas_draw_str(canvas, 2, y_offset, "- RIGHT: Clear input");
    y_offset += 10;
    canvas_draw_str(canvas, 2, y_offset, "- UP/DOWN: Adjust volume");
}

// Function to update the top words marquee buffer
static void update_top_words_marquee(MorseApp* app, char new_char) {
    // If there's room in the buffer, just append
    size_t len = strlen(app->top_words);
    
    if(len < TOP_WORDS_MAX_LENGTH) {
        // There's still room, just append
        app->top_words[len] = new_char;
        app->top_words[len + 1] = '\0';
    } else {
        // Shift characters left by one position
        for(size_t i = 0; i < TOP_WORDS_MAX_LENGTH - 1; i++) {
            app->top_words[i] = app->top_words[i + 1];
        }
        // Add the new character at the end
        app->top_words[TOP_WORDS_MAX_LENGTH - 1] = new_char;
        app->top_words[TOP_WORDS_MAX_LENGTH] = '\0';
    }
}

// Draw the title screen with the title-screen.png image
static void draw_title_screen(Canvas* canvas, MorseApp* app) {
    UNUSED(app);  // Mark parameter as unused to prevent compiler warning
    
    canvas_draw_icon(canvas, 0, 0, &I_title_screen);
}
