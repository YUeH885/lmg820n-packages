#include "gnss-state.h"

const gchar *
gnss_state_to_string (GnssState state)
{
    switch (state) {
    case GNSS_STATE_IDLE: return "idle";
    case GNSS_STATE_STARTING: return "starting";
    case GNSS_STATE_TRACKING: return "tracking";
    case GNSS_STATE_STOPPING: return "stopping";
    case GNSS_STATE_SUSPENDED: return "suspended";
    case GNSS_STATE_FAILED: return "failed";
    default: return "unknown";
    }
}

static GnssAction
reconcile (GnssStateMachine *sm)
{
    if (sm->state == GNSS_STATE_FAILED || sm->state == GNSS_STATE_STARTING ||
        sm->state == GNSS_STATE_STOPPING)
        return GNSS_ACTION_NONE;
    if (sm->state == GNSS_STATE_TRACKING) {
        if (sm->suspend_requested || !sm->active_clients) {
            sm->state = GNSS_STATE_STOPPING;
            return GNSS_ACTION_STOP;
        }
        return GNSS_ACTION_NONE;
    }
    if (sm->suspend_requested) {
        sm->state = GNSS_STATE_SUSPENDED;
        return GNSS_ACTION_NONE;
    }
    sm->state = GNSS_STATE_IDLE;
    if (sm->node_available && sm->active_clients) {
        sm->state = GNSS_STATE_STARTING;
        return GNSS_ACTION_START;
    }
    return GNSS_ACTION_NONE;
}

void
gnss_state_init (GnssStateMachine *sm)
{
    *sm = (GnssStateMachine){ .state = GNSS_STATE_IDLE };
}

GnssAction
gnss_state_on_client_added (GnssStateMachine *sm)
{
    sm->active_clients++;
    return reconcile (sm);
}

GnssAction
gnss_state_on_client_removed (GnssStateMachine *sm)
{
    g_return_val_if_fail (sm->active_clients > 0, GNSS_ACTION_NONE);
    sm->active_clients--;
    return reconcile (sm);
}

GnssAction
gnss_state_on_start_finished (GnssStateMachine *sm, gboolean success)
{
    if (sm->state != GNSS_STATE_STARTING)
        return GNSS_ACTION_NONE;
    sm->state = success ? GNSS_STATE_TRACKING : GNSS_STATE_FAILED;
    return reconcile (sm);
}

GnssAction
gnss_state_on_stop_finished (GnssStateMachine *sm, gboolean success)
{
    if (sm->state != GNSS_STATE_STOPPING)
        return GNSS_ACTION_NONE;
    sm->state = success ? GNSS_STATE_IDLE : GNSS_STATE_FAILED;
    return reconcile (sm);
}

GnssAction
gnss_state_on_suspend (GnssStateMachine *sm)
{
    sm->suspend_requested = TRUE;
    return reconcile (sm);
}

GnssAction
gnss_state_on_resume (GnssStateMachine *sm)
{
    sm->suspend_requested = FALSE;
    return reconcile (sm);
}

GnssAction
gnss_state_set_node_available (GnssStateMachine *sm, gboolean available)
{
    sm->node_available = available;
    if (!available && sm->state != GNSS_STATE_FAILED)
        sm->state = GNSS_STATE_IDLE;
    return reconcile (sm);
}
