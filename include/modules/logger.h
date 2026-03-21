#ifndef MOONLIGHT_LOGGER_H
#define MOONLIGHT_LOGGER_H

/**
 * @file logger.h
 * @brief Logging module for Moonlight PSP.
 */

/**
 * @enum LogComponent
 * @brief Components that can emit log messages.
 */
typedef enum {
    COMPONENT_MAIN,
    COMPONENT_VIDEO,
    COMPONENT_AUDIO,
    COMPONENT_NETWORK,
    COMPONENT_LIMELIGHT
} LogComponent;

/**
 * @brief Converts a LogComponent enum value to its string representation.
 * @param comp The component to convert.
 * @return A constant string naming the component.
 */
const char* LogComponentToString(LogComponent comp);

/**
 * @brief Initializes the logging subsystem, including file and mutex creation.
 */
void logger_init();

/**
 * @brief Shuts down the logging subsystem and releases resources.
 */
void logger_shutdown();

/**
 * @brief Core logging function.
 * @param level The log level string (e.g., "INFO", "ERROR").
 * @param component_str The component name string.
 * @param format The printf-style format string.
 * @param ... Additional arguments for the format string.
 */
void moonlight_log(const char* level, const char* component_str, const char* format, ...);

// Update macros to use the enum and convert it to string
#define LOG_INFO(comp_enum, ...)  moonlight_log("INFO", LogComponentToString(comp_enum), __VA_ARGS__)
#define LOG_ERROR(comp_enum, ...) moonlight_log("ERROR", LogComponentToString(comp_enum), __VA_ARGS__)
#define LOG_DEBUG(comp_enum, ...) moonlight_log("DEBUG", LogComponentToString(comp_enum), __VA_ARGS__)

#endif
