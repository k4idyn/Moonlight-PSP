#pragma once

#include "Limelight.h"
#include "Platform.h"

typedef void(*ThreadEntry)(void* context);

#if defined(LC_WINDOWS)
typedef SRWLOCK PLT_MUTEX;
typedef CONDITION_VARIABLE PLT_COND;
typedef struct _PLT_THREAD {
    HANDLE handle;
    bool cancelled;
} PLT_THREAD;
#elif defined(__WIIU__)
typedef OSFastMutex PLT_MUTEX;
typedef OSFastCondition PLT_COND;
typedef struct _PLT_THREAD {
    OSThread thread;
    int cancelled;
} PLT_THREAD;
#elif defined(__3DS__)
typedef LightLock PLT_MUTEX;
typedef CondVar PLT_COND;
typedef struct _PLT_THREAD {
    Thread thread;
    bool cancelled;
} PLT_THREAD;
#elif defined(_PSP)
typedef SceUID PLT_MUTEX;
typedef SceUID PLT_COND;
typedef struct _PLT_THREAD {
    SceUID thread;
    bool cancelled;
} PLT_THREAD;
#elif defined (LC_POSIX)
typedef pthread_mutex_t PLT_MUTEX;
typedef pthread_cond_t PLT_COND;
typedef struct _PLT_THREAD {
    pthread_t thread;
    bool cancelled;
} PLT_THREAD;
#else
#error Unsupported platform
#endif

#ifdef LC_WINDOWS
typedef HANDLE PLT_EVENT;
#else
typedef struct _PLT_EVENT {
    PLT_MUTEX mutex;
    PLT_COND cond;
    bool signalled;
} PLT_EVENT;
#endif

/**
 * @brief Creates a platform-specific mutex.
 * @param mutex Pointer to the mutex structure to initialize.
 * @return 0 on success, negative error code on failure.
 */
int PltCreateMutex(PLT_MUTEX* mutex);

/**
 * @brief Deletes a platform-specific mutex.
 * @param mutex Pointer to the mutex structure to delete.
 */
void PltDeleteMutex(PLT_MUTEX* mutex);

/**
 * @brief Locks a platform-specific mutex.
 * @param mutex Pointer to the mutex structure to lock.
 */
void PltLockMutex(PLT_MUTEX* mutex);

/**
 * @brief Unlocks a platform-specific mutex.
 * @param mutex Pointer to the mutex structure to unlock.
 */
void PltUnlockMutex(PLT_MUTEX* mutex);

/**
 * @brief Creates and starts a new platform thread.
 * @param name Name of the thread (for debugging).
 * @param entry The function to execute in the new thread.
 * @param context The argument to pass to the entry function.
 * @param thread Pointer to the thread structure to initialize.
 * @return 0 on success, negative error code on failure.
 */
int PltCreateThread(const char* name, ThreadEntry entry, void* context, PLT_THREAD* thread);

/**
 * @brief Flags a thread for interruption.
 * @param thread Pointer to the thread to interrupt.
 */
void PltInterruptThread(PLT_THREAD* thread);

/**
 * @brief Checks if a thread has been interrupted.
 * @param thread Pointer to the thread to check.
 * @return true if interrupted, false otherwise.
 */
bool PltIsThreadInterrupted(PLT_THREAD* thread);

/**
 * @brief Waits for a thread to complete and cleans up its resources.
 * @param thread Pointer to the thread to join.
 */
void PltJoinThread(PLT_THREAD* thread);

/**
 * @brief Detaches a thread, allowing it to clean up itself when finished.
 * @param thread Pointer to the thread to detach.
 */
void PltDetachThread(PLT_THREAD* thread);

/**
 * @brief Creates a platform-specific event object.
 * @param event Pointer to the event structure to initialize.
 * @return 0 on success, negative error code on failure.
 */
int PltCreateEvent(PLT_EVENT* event);

/**
 * @brief Closes and cleans up an event object.
 * @param event Pointer to the event structure to close.
 */
void PltCloseEvent(PLT_EVENT* event);

/**
 * @brief Signals an event object.
 * @param event Pointer to the event structure to set.
 */
void PltSetEvent(PLT_EVENT* event);

/**
 * @brief Clears (resets) an event object.
 * @param event Pointer to the event structure to clear.
 */
void PltClearEvent(PLT_EVENT* event);

/**
 * @brief Waits until an event object is signalled.
 * @param event Pointer to the event structure to wait on.
 */
void PltWaitForEvent(PLT_EVENT* event);

/**
 * @brief Creates a platform-specific condition variable.
 * @param cond Pointer to the condition variable structure to initialize.
 * @param mutex Associated mutex (used on some platforms).
 * @return 0 on success, negative error code on failure.
 */
int PltCreateConditionVariable(PLT_COND* cond, PLT_MUTEX* mutex);

/**
 * @brief Deletes a condition variable.
 * @param cond Pointer to the condition variable structure to delete.
 */
void PltDeleteConditionVariable(PLT_COND* cond);

/**
 * @brief Signals one thread waiting on a condition variable.
 * @param cond Pointer to the condition variable structure to signal.
 */
void PltSignalConditionVariable(PLT_COND* cond);

/**
 * @brief Waits on a condition variable, atomically releasing and re-acquiring the mutex.
 * @param cond Pointer to the condition variable structure to wait on.
 * @param mutex Pointer to the locked mutex.
 */
void PltWaitForConditionVariable(PLT_COND* cond, PLT_MUTEX* mutex);

/**
 * @brief Sleeps for a specified number of milliseconds.
 * @param ms Milliseconds to sleep.
 */
void PltSleepMs(int ms);

/**
 * @brief Sleeps for a specified time unless interrupted.
 * @param thread Pointer to the current thread.
 * @param ms Max milliseconds to sleep.
 */
void PltSleepMsInterruptible(PLT_THREAD* thread, int ms);
