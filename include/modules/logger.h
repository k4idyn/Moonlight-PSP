#ifndef LOGGER_H
#define LOGGER_H

// Define an enum for log components
typedef enum {
    COMPONENT_MAIN,
    COMPONENT_VIDEO,
    COMPONENT_AUDIO,
    COMPONENT_NETWORK,
    COMPONENT_LIMELIGHT
} LogComponent;

// Helper function to convert LogComponent enum to a string
const char* LogComponentToString(LogComponent comp);

void logger_init();
void logger_shutdown();

void moonlight_log(const char* level, const char* component_str, const char* format, ...);

// Update macros to use the enum and convert it to string
#define LOG_INFO(comp_enum, ...)  moonlight_log("INFO", LogComponentToString(comp_enum), __VA_ARGS__)
#define LOG_ERROR(comp_enum, ...) moonlight_log("ERROR", LogComponentToString(comp_enum), __VA_ARGS__)
#define LOG_DEBUG(comp_enum, ...) moonlight_log("DEBUG", LogComponentToString(comp_enum), __VA_ARGS__)

#endif
