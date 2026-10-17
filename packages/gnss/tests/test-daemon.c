#define main gnss_daemon_main
#include "../src/main.c"
#undef main
#include <fcntl.h>

struct _GnssLoc {
    guint starts;
    guint stops;
    guint resumes;
    GTask *pause;
};

GnssLoc *gnss_loc_new (void) { return g_new0 (GnssLoc, 1); }
void gnss_loc_free (GnssLoc *loc) { g_free (loc); }
void gnss_loc_set_callbacks (GnssLoc *loc, GnssLocNmeaCb n, GnssLocReadyCb r,
                             GnssLocSessionCb s, GnssLocErrorCb e, gpointer d)
{ (void)loc; (void)n; (void)r; (void)s; (void)e; (void)d; }
void gnss_loc_start_session (GnssLoc *loc) { loc->starts++; }
void gnss_loc_stop_session (GnssLoc *loc) { loc->stops++; }
void gnss_loc_resume (GnssLoc *loc) { loc->resumes++; }
void gnss_loc_pause_async (GnssLoc *loc, GAsyncReadyCallback callback, gpointer data)
{
    g_assert_null (loc->pause);
    loc->pause = g_task_new (NULL, NULL, callback, data);
    on_loc_ready (loc, FALSE, data);
}

static void
prepare (GnssDaemon *daemon, gboolean sleep)
{
    g_autoptr(GVariant) parameters = g_variant_ref_sink (g_variant_new ("(b)", sleep));
    on_prepare_for_sleep (NULL, NULL, NULL, NULL, NULL, parameters, daemon);
}

static void
complete_close (GnssDaemon *daemon)
{
    GTask *task = g_steal_pointer (&daemon->loc->pause);
    g_assert_nonnull (task);
    g_task_return_boolean (task, TRUE);
    g_object_unref (task);
    while (daemon->pausing)
        g_main_context_iteration (NULL, TRUE);
}

static void
test_sleep_order (gconstpointer data)
{
    gboolean starting_at_sleep = GPOINTER_TO_INT (data);
    GnssLoc loc = {0};
    GnssDaemon daemon = { .loc = &loc, .sleep_delay_ms = 1000 };
    daemon.loop = g_main_loop_new (NULL, FALSE);
    gnss_state_init (&daemon.sm);
    on_loc_ready (&loc, TRUE, &daemon);
    on_server_client_count (1, &daemon);
    if (!starting_at_sleep)
        on_loc_session (&loc, TRUE, TRUE, &daemon);
    int fds[2];
    g_assert_cmpint (pipe (fds), ==, 0);
    daemon.inhibitor_fd = fds[0];
    prepare (&daemon, TRUE);
    g_assert_cmpint (fcntl (fds[0], F_GETFD), >=, 0);
    g_assert_cmpuint (loc.stops, ==, !starting_at_sleep);
    prepare (&daemon, FALSE);
    g_assert_cmpuint (loc.starts, ==, 1);
    if (starting_at_sleep)
        on_loc_session (&loc, TRUE, TRUE, &daemon);
    g_assert_cmpuint (loc.stops, ==, 1);
    g_assert_null (loc.pause);
    on_loc_session (&loc, FALSE, TRUE, &daemon);
    g_assert_nonnull (loc.pause);
    g_assert_cmpint (fcntl (fds[0], F_GETFD), >=, 0);
    complete_close (&daemon);
    g_assert_cmpint (fcntl (fds[0], F_GETFD), ==, -1);
    g_assert_cmpuint (loc.resumes, ==, 1);
    g_assert_cmpuint (loc.starts, ==, 1);
    on_loc_ready (&loc, TRUE, &daemon);
    g_assert_cmpuint (loc.starts, ==, 2);
    g_assert_cmpint (daemon.sm.state, ==, GNSS_STATE_STARTING);
    g_assert_cmpuint (daemon.deadline_id, ==, 0);
    close (fds[1]);
    g_main_loop_unref (daemon.loop);
}

static void
test_failed_stop (void)
{
    GnssLoc loc = {0};
    GnssDaemon daemon = { .loc = &loc, .inhibitor_fd = -1 };
    daemon.loop = g_main_loop_new (NULL, FALSE);
    gnss_state_init (&daemon.sm);
    on_loc_ready (&loc, TRUE, &daemon);
    on_server_client_count (1, &daemon);
    on_loc_session (&loc, TRUE, TRUE, &daemon);
    begin_shutdown (&daemon, FALSE);
    g_assert_cmpuint (loc.stops, ==, 1);
    g_assert_null (loc.pause);
    on_loc_session (&loc, FALSE, FALSE, &daemon);
    g_assert_cmpint (daemon.sm.state, ==, GNSS_STATE_FAILED);
    g_assert_cmpint (daemon.exit_status, ==, 1);
    complete_close (&daemon);
    g_assert_cmpuint (loc.resumes, ==, 0);
    g_main_loop_unref (daemon.loop);
}

static void
test_continuous_tracking (void)
{
    GnssLoc loc = {0};
    GnssDaemon daemon = { .loc = &loc, .always_track = TRUE, .inhibitor_fd = -1,
                          .sleep_delay_ms = 1000 };
    daemon.loop = g_main_loop_new (NULL, FALSE);
    gnss_state_init (&daemon.sm);
    on_server_client_count (0, &daemon);
    on_loc_ready (&loc, TRUE, &daemon);
    g_assert_cmpuint (loc.starts, ==, 1);
    on_loc_session (&loc, TRUE, TRUE, &daemon);
    on_server_client_count (2, &daemon);
    on_server_client_count (0, &daemon);
    g_assert_cmpuint (loc.stops, ==, 0);
    g_assert_cmpint (daemon.sm.state, ==, GNSS_STATE_TRACKING);

    prepare (&daemon, TRUE);
    g_assert_cmpuint (loc.stops, ==, 1);
    on_loc_session (&loc, FALSE, TRUE, &daemon);
    prepare (&daemon, FALSE);
    complete_close (&daemon);
    g_assert_cmpuint (loc.resumes, ==, 1);
    on_loc_ready (&loc, TRUE, &daemon);
    g_assert_cmpuint (loc.starts, ==, 2);
    on_loc_session (&loc, TRUE, TRUE, &daemon);

    begin_shutdown (&daemon, FALSE);
    g_assert_cmpuint (loc.stops, ==, 2);
    on_loc_session (&loc, FALSE, TRUE, &daemon);
    complete_close (&daemon);
    g_assert_cmpint (daemon.exit_status, ==, 0);
    g_main_loop_unref (daemon.loop);
}

int
main (int argc, char **argv)
{
    g_test_init (&argc, &argv, NULL);
    g_test_add_data_func ("/daemon/sleep-during-start", GINT_TO_POINTER (TRUE), test_sleep_order);
    g_test_add_data_func ("/daemon/resume-during-stop", GINT_TO_POINTER (FALSE), test_sleep_order);
    g_test_add_func ("/daemon/failed-stop", test_failed_stop);
    g_test_add_func ("/daemon/continuous-tracking", test_continuous_tracking);
    return g_test_run ();
}
