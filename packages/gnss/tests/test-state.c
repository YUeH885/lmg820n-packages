#include "gnss-state.h"
#include <glib.h>

static GnssStateMachine
starting (void)
{
    GnssStateMachine sm;
    gnss_state_init (&sm);
    g_assert_cmpint (gnss_state_on_client_added (&sm), ==, GNSS_ACTION_NONE);
    g_assert_cmpint (gnss_state_set_node_available (&sm, TRUE), ==, GNSS_ACTION_START);
    return sm;
}

static void
test_lifecycle (void)
{
    GnssStateMachine sm = starting ();
    g_assert_cmpint (gnss_state_on_start_finished (&sm, TRUE), ==, GNSS_ACTION_NONE);
    g_assert_cmpint (gnss_state_on_client_added (&sm), ==, GNSS_ACTION_NONE);
    g_assert_cmpint (gnss_state_on_client_removed (&sm), ==, GNSS_ACTION_NONE);
    g_assert_cmpint (gnss_state_on_client_removed (&sm), ==, GNSS_ACTION_STOP);
    g_assert_cmpint (gnss_state_on_stop_finished (&sm, TRUE), ==, GNSS_ACTION_NONE);
    g_assert_cmpint (sm.state, ==, GNSS_STATE_IDLE);
}

static void
test_disconnect_during_start (void)
{
    GnssStateMachine sm = starting ();
    g_assert_cmpint (gnss_state_on_client_removed (&sm), ==, GNSS_ACTION_NONE);
    g_assert_cmpint (sm.state, ==, GNSS_STATE_STARTING);
    g_assert_cmpint (gnss_state_on_start_finished (&sm, TRUE), ==, GNSS_ACTION_STOP);
    g_assert_cmpint (gnss_state_on_stop_finished (&sm, TRUE), ==, GNSS_ACTION_NONE);
    g_assert_cmpint (sm.state, ==, GNSS_STATE_IDLE);
}

static void
test_suspend_during_start (void)
{
    GnssStateMachine sm = starting ();
    g_assert_cmpint (gnss_state_on_suspend (&sm), ==, GNSS_ACTION_NONE);
    g_assert_cmpint (sm.state, ==, GNSS_STATE_STARTING);
    g_assert_cmpint (gnss_state_on_start_finished (&sm, TRUE), ==, GNSS_ACTION_STOP);
    g_assert_cmpint (sm.state, ==, GNSS_STATE_STOPPING);
    g_assert_cmpint (gnss_state_on_stop_finished (&sm, TRUE), ==, GNSS_ACTION_NONE);
    g_assert_cmpint (sm.state, ==, GNSS_STATE_SUSPENDED);
    g_assert_cmpint (gnss_state_on_resume (&sm), ==, GNSS_ACTION_START);
}

static void
test_resume_during_stop (void)
{
    GnssStateMachine sm = starting ();
    gnss_state_on_start_finished (&sm, TRUE);
    g_assert_cmpint (gnss_state_on_suspend (&sm), ==, GNSS_ACTION_STOP);
    g_assert_cmpint (gnss_state_on_resume (&sm), ==, GNSS_ACTION_NONE);
    g_assert_cmpint (sm.state, ==, GNSS_STATE_STOPPING);
    g_assert_cmpint (gnss_state_on_stop_finished (&sm, TRUE), ==, GNSS_ACTION_START);
    g_assert_cmpint (sm.state, ==, GNSS_STATE_STARTING);
}

static void
test_failed_stop (void)
{
    GnssStateMachine sm = starting ();
    gnss_state_on_start_finished (&sm, TRUE);
    gnss_state_on_suspend (&sm);
    g_assert_cmpint (gnss_state_on_stop_finished (&sm, FALSE), ==, GNSS_ACTION_NONE);
    g_assert_cmpint (sm.state, ==, GNSS_STATE_FAILED);
    g_assert_cmpint (gnss_state_on_resume (&sm), ==, GNSS_ACTION_NONE);
    gnss_state_set_node_available (&sm, FALSE);
    gnss_state_set_node_available (&sm, TRUE);
    g_assert_cmpint (sm.state, ==, GNSS_STATE_FAILED);
}

static void
test_node_replaced (void)
{
    GnssStateMachine sm = starting ();
    gnss_state_set_node_available (&sm, FALSE);
    g_assert_cmpint (gnss_state_on_start_finished (&sm, TRUE), ==, GNSS_ACTION_NONE);
    g_assert_cmpint (sm.state, ==, GNSS_STATE_IDLE);
    g_assert_cmpint (gnss_state_set_node_available (&sm, TRUE), ==, GNSS_ACTION_START);
}

int
main (int argc, char **argv)
{
    g_test_init (&argc, &argv, NULL);
    g_test_add_func ("/state/lifecycle", test_lifecycle);
    g_test_add_func ("/state/disconnect-during-start", test_disconnect_during_start);
    g_test_add_func ("/state/suspend-during-start", test_suspend_during_start);
    g_test_add_func ("/state/resume-during-stop", test_resume_during_stop);
    g_test_add_func ("/state/failed-stop", test_failed_stop);
    g_test_add_func ("/state/node-replaced", test_node_replaced);
    return g_test_run ();
}
