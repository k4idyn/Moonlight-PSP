#ifndef MOONLIGHT_APP_STARTUP_H
#define MOONLIGHT_APP_STARTUP_H

/* Load the adjacent kernel helper before the first AVC call. A helper
 * supplied by PSPLink remains owned by PSPLink. */
int moonlight_startup_load_helper(int argc, char *argv[]);
int moonlight_startup_release_helper(void);
const char *moonlight_startup_error_step(void);
const char *moonlight_startup_helper_path(void);

#endif
