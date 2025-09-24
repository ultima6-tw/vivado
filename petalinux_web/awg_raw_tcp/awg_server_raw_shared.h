// awg_server_raw_shared.h — FINAL VERSION
// Shared state and declarations for AWG server modules.

#ifndef AWG_SERVER_SHARED_H
#define AWG_SERVER_SHARED_H

#include <pthread.h>

// Defines the three possible states for a list.
enum list_status {
    LIST_IDLE,
    LIST_LOADING,
    LIST_READY
};

// --- Global variables shared between modules ---
// Defined in awg_server_raw_notify.c and used by awg_server_raw_queue.c.

// Mutex to protect access to the notification state.
extern pthread_mutex_t g_notify_mutex;

// Status of the two lists (0 and 1).
extern volatile int g_list_status[2];

// --- Public functions exported by the notification server ---

// Call this function to send a status update for a specific list.
void send_status_update(int list_id);

// Functions to start and stop the notification server.
int start_notify_server(unsigned short port);
void stop_notify_server(void);

#endif // AWG_SERVER_SHARED_H