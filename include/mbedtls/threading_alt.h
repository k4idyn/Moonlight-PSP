#ifndef THREADING_ALT_H
#define THREADING_ALT_H

#include "platform_alt.h"

/* Define the platform mutex and condition variable types for PSP */
/* These are already defined in platform_alt.h, but we redeclare them here for clarity */
typedef SceUID mbedtls_platform_mutex_t;
typedef SceUID mbedtls_platform_condition_variable_t;

#endif /* THREADING_ALT_H */