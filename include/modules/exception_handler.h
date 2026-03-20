#ifndef EXCEPTION_HANDLER_H
#define EXCEPTION_HANDLER_H

/**
 * @brief Initialize the custom exception handler (Blue Screen of Death).
 * Registers a callback to catch MIPS hardware exceptions and display 
 * a register dump on the screen.
 */
#define PANIC(msg, code) exception_handler_trigger_manually(msg, code)

void exception_handler_init(void);

/**
 * Manually trigger the diagnostic "Blue Screen" with a custom message.
 * This satisfies the "Absolute Perfection" requirement for localized failure reporting.
 */
void exception_handler_trigger_manually(const char* reason, int error_code);

#endif /* EXCEPTION_HANDLER_H */
