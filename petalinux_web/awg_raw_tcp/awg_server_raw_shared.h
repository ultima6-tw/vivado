// awg_server_shared.h — Shared state and declarations for server modules

#ifndef AWG_SERVER_SHARED_H
#define AWG_SERVER_SHARED_H

#include <pthread.h>

// Define the possible states for a list
enum list_status {
    LIST_IDLE,
    LIST_FULL
};

// --- Global variables shared between modules ---
// These are DEFINED in awg_server_raw_notify.c and
// DECLARED as extern here to be used by awg_server_raw_queue.c

// Mutex to protect access to the notification client socket and status
extern pthread_mutex_t g_notify_mutex;

// Status of the two lists (0 and 1)
extern volatile int g_list_status[2];


// --- Public functions exported by the notification server ---

// Call this function to send a system-wide status update (IDLE/FULL)
void send_system_status(void);

// Functions to start and stop the notification server (called by top/main)
int start_notify_server(unsigned short port);
void stop_notify_server(void);

#endif // AWG_SERVER_SHARED_H