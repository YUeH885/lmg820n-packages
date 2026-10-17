#pragma once
#include <glib.h>

typedef enum {
    GNSS_STATE_IDLE,
    GNSS_STATE_STARTING,
    GNSS_STATE_TRACKING,
    GNSS_STATE_STOPPING,
    GNSS_STATE_SUSPENDED,
    GNSS_STATE_FAILED,
} GnssState;

typedef enum {
    GNSS_ACTION_NONE,
    GNSS_ACTION_START,
    GNSS_ACTION_STOP,
} GnssAction;

typedef struct {
    GnssState state;
    guint active_clients;
    gboolean node_available;
    gboolean suspend_requested;
} GnssStateMachine;

const gchar *gnss_state_to_string (GnssState state);
void gnss_state_init (GnssStateMachine *sm);
GnssAction gnss_state_on_client_added (GnssStateMachine *sm);
GnssAction gnss_state_on_client_removed (GnssStateMachine *sm);
GnssAction gnss_state_on_start_finished (GnssStateMachine *sm, gboolean success);
GnssAction gnss_state_on_stop_finished (GnssStateMachine *sm, gboolean success);
GnssAction gnss_state_on_suspend (GnssStateMachine *sm);
GnssAction gnss_state_on_resume (GnssStateMachine *sm);
GnssAction gnss_state_set_node_available (GnssStateMachine *sm, gboolean available);
