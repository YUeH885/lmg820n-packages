#include "gnss-common.h"
#include "gnss-state.h"
#include "gnss-nmea.h"
#include "gnss-server.h"
#include "gnss-loc.h"
#include "gnss-utc.h"

#include <gio/gunixfdlist.h>
#include <glib-unix.h>
#include <systemd/sd-daemon.h>
#include <unistd.h>

#define CLEANUP_RESERVE_MS (2 * GNSS_LOC_CLEANUP_TIMEOUT_SECONDS * 1000 + 250)

typedef struct {
    GMainLoop *loop;
    GnssStateMachine sm;
    GnssServer *server;
    GnssLoc *loc;
    GnssUtc *utc;
    GDBusConnection *dbus;
    guint sleep_sub_id;
    guint deadline_id;
    guint sleep_delay_ms;
    gint inhibitor_fd;
    guint last_client_count;
    gboolean always_track;
    gboolean inhibitor_pending;
    gboolean sleeping;
    gboolean cleanup_required;
    gboolean pausing;
    gboolean paused;
    gboolean shutdown;
    gint exit_status;
} GnssDaemon;

static void settle (GnssDaemon *daemon);
static void take_sleep_inhibitor (GnssDaemon *daemon);
static void begin_shutdown (GnssDaemon *daemon, gboolean failed);

static void
release_inhibitor (GnssDaemon *daemon)
{
    if (daemon->inhibitor_fd >= 0) {
        close (daemon->inhibitor_fd);
        daemon->inhibitor_fd = -1;
    }
}

static void
clear_deadline (GnssDaemon *daemon)
{
    if (daemon->deadline_id) {
        g_source_remove (daemon->deadline_id);
        daemon->deadline_id = 0;
    }
}

static void
apply_action (GnssDaemon *daemon, GnssAction action)
{
    if (action == GNSS_ACTION_START)
        gnss_loc_start_session (daemon->loc);
    else if (action == GNSS_ACTION_STOP)
        gnss_loc_stop_session (daemon->loc);
}

static void
resume_after_cleanup (GnssDaemon *daemon)
{
    daemon->cleanup_required = FALSE;
    daemon->paused = FALSE;
    apply_action (daemon, gnss_state_on_resume (&daemon->sm));
    gnss_loc_resume (daemon->loc);
    take_sleep_inhibitor (daemon);
}

static void
on_loc_paused (GObject *source, GAsyncResult *result, gpointer data)
{
    (void)source;
    GnssDaemon *daemon = data;
    g_autoptr(GError) error = NULL;
    daemon->pausing = FALSE;
    daemon->paused = TRUE;
    if (!g_task_propagate_boolean (G_TASK (result), &error)) {
        g_warning ("Could not close LOC connection: %s", error->message);
        daemon->shutdown = TRUE;
        daemon->exit_status = 1;
    }
    clear_deadline (daemon);
    release_inhibitor (daemon);
    g_message ("LOC cleanup completed; sleep delay inhibitor released");
    if (!daemon->shutdown && !daemon->sleeping)
        resume_after_cleanup (daemon);
    settle (daemon);
}

static void
settle (GnssDaemon *daemon)
{
    if (!daemon->cleanup_required)
        return;
    if (daemon->paused) {
        if (daemon->shutdown && !daemon->inhibitor_pending)
            g_main_loop_quit (daemon->loop);
        return;
    }
    if (!daemon->pausing && (daemon->sm.state == GNSS_STATE_SUSPENDED ||
                             daemon->sm.state == GNSS_STATE_FAILED)) {
        daemon->pausing = TRUE;
        gnss_loc_pause_async (daemon->loc, on_loc_paused, daemon);
    }
}

static gboolean
cleanup_deadline (gpointer data)
{
    GnssDaemon *daemon = data;
    daemon->deadline_id = 0;
    g_warning ("LOC stop deadline exceeded; session stop is unconfirmed");
    daemon->sm.state = GNSS_STATE_FAILED;
    begin_shutdown (daemon, TRUE);
    return G_SOURCE_REMOVE;
}

static void
begin_shutdown (GnssDaemon *daemon, gboolean failed)
{
    daemon->exit_status |= failed;
    if (!daemon->shutdown) {
        daemon->shutdown = TRUE;
        daemon->cleanup_required = TRUE;
        apply_action (daemon, gnss_state_on_suspend (&daemon->sm));
        if (!daemon->deadline_id && !daemon->paused)
            daemon->deadline_id = g_timeout_add (12000, cleanup_deadline, daemon);
    }
    settle (daemon);
}

static void
on_inhibitor_acquired (GObject *source, GAsyncResult *result, gpointer data)
{
    GnssDaemon *daemon = data;
    g_autoptr(GError) error = NULL;
    g_autoptr(GUnixFDList) fd_list = NULL;
    g_autoptr(GVariant) reply = g_dbus_connection_call_with_unix_fd_list_finish (
        G_DBUS_CONNECTION (source), &fd_list, result, &error);
    daemon->inhibitor_pending = FALSE;
    if (reply && fd_list) {
        gint32 index;
        g_variant_get (reply, "(h)", &index);
        daemon->inhibitor_fd = g_unix_fd_list_get (fd_list, index, &error);
    }
    if (error)
        g_warning ("Could not acquire sleep delay inhibitor: %s", error->message);
    if (daemon->paused)
        release_inhibitor (daemon);
    settle (daemon);
}

static void
take_sleep_inhibitor (GnssDaemon *daemon)
{
    if (!daemon->dbus || daemon->inhibitor_fd >= 0 || daemon->inhibitor_pending ||
        daemon->shutdown || daemon->sleeping)
        return;
    daemon->inhibitor_pending = TRUE;
    g_dbus_connection_call_with_unix_fd_list (
        daemon->dbus, "org.freedesktop.login1", "/org/freedesktop/login1",
        "org.freedesktop.login1.Manager", "Inhibit",
        g_variant_new ("(ssss)", "sleep", "lmg820n-gnssd", "Stop LOC before sleep", "delay"),
        G_VARIANT_TYPE ("(h)"), G_DBUS_CALL_FLAGS_NONE, 2000, NULL, NULL,
        on_inhibitor_acquired, daemon);
}

static void
on_prepare_for_sleep (GDBusConnection *connection, const gchar *sender,
                      const gchar *path, const gchar *interface,
                      const gchar *signal, GVariant *parameters, gpointer data)
{
    (void)connection; (void)sender; (void)path; (void)interface; (void)signal;
    GnssDaemon *daemon = data;
    gboolean sleeping;
    g_variant_get (parameters, "(b)", &sleeping);
    if (daemon->shutdown)
        return;
    daemon->sleeping = sleeping;
    if (sleeping) {
        g_message ("Preparing for sleep; waiting for LOC stop and connection close");
        daemon->cleanup_required = TRUE;
        if (!daemon->deadline_id && !daemon->paused)
            daemon->deadline_id = g_timeout_add (daemon->sleep_delay_ms, cleanup_deadline, daemon);
        apply_action (daemon, gnss_state_on_suspend (&daemon->sm));
        settle (daemon);
    } else if (daemon->paused) {
        resume_after_cleanup (daemon);
    }
    /* An early resume keeps the stop/close sequence intact. */
}

static void
on_loc_nmea (const gchar *sentence, gpointer data)
{
    GnssDaemon *daemon = data;
    struct timespec received;
    if (clock_gettime (CLOCK_REALTIME, &received) == 0)
        gnss_utc_process_nmea (daemon->utc, sentence, &received);
    g_auto(GStrv) sentences = gnss_nmea_split_and_normalize (sentence);
    if (sentences)
        for (guint i = 0; sentences[i]; i++)
            gnss_server_broadcast_sentence (daemon->server, sentences[i]);
}

static void
on_loc_ready (GnssLoc *loc, gboolean ready, gpointer data)
{
    (void)loc;
    GnssDaemon *daemon = data;
    apply_action (daemon, gnss_state_set_node_available (&daemon->sm, ready));
    g_message ("GNSS node %s; state: %s", ready ? "available" : "unavailable",
               gnss_state_to_string (daemon->sm.state));
    settle (daemon);
}

static void
on_loc_session (GnssLoc *loc, gboolean start, gboolean success, gpointer data)
{
    (void)loc;
    GnssDaemon *daemon = data;
    GnssAction action = start ? gnss_state_on_start_finished (&daemon->sm, success)
                             : gnss_state_on_stop_finished (&daemon->sm, success);
    g_message ("GNSS %s %s; state: %s", start ? "start" : "stop",
               success ? "succeeded" : "failed", gnss_state_to_string (daemon->sm.state));
    if (!success)
        begin_shutdown (daemon, TRUE);
    else {
        apply_action (daemon, action);
        settle (daemon);
    }
}

static void
on_loc_error (GnssLoc *loc, const gchar *message, gpointer data)
{
    (void)loc;
    GnssDaemon *daemon = data;
    g_warning ("LOC initialization failed: %s", message);
    daemon->sm.state = GNSS_STATE_FAILED;
    begin_shutdown (daemon, TRUE);
}

static void
on_server_client_count (guint count, gpointer data)
{
    GnssDaemon *daemon = data;
    guint effective = count + (daemon->always_track ? 1 : 0);
    while (daemon->last_client_count < effective) {
        daemon->last_client_count++;
        apply_action (daemon, gnss_state_on_client_added (&daemon->sm));
    }
    while (daemon->last_client_count > effective) {
        daemon->last_client_count--;
        apply_action (daemon, gnss_state_on_client_removed (&daemon->sm));
    }
    g_message ("GNSS socket clients: %u (effective: %u)", count, effective);
    settle (daemon);
}

static gboolean
on_unix_signal (gpointer data)
{
    begin_shutdown (data, FALSE);
    return G_SOURCE_CONTINUE;
}

int
main (int argc, char **argv)
{
    g_autoptr(GError) error = NULL;
    g_autofree gchar *socket_path = NULL;
    g_autofree gchar *utc_socket = NULL;
    gboolean show_version = FALSE;
    gboolean debug = FALSE;
    gboolean no_utc = FALSE;
    gboolean always_track = FALSE;
    GnssDaemon daemon = { .inhibitor_fd = -1, .sleep_delay_ms = 5000 - CLEANUP_RESERVE_MS };
    GOptionEntry entries[] = {
        {"socket", 's', 0, G_OPTION_ARG_STRING, &socket_path, "Path to NMEA Unix socket", "PATH"},
        {"utc-socket", 'c', 0, G_OPTION_ARG_STRING, &utc_socket, "Path to the UTC second-label socket", "PATH"},
        {"no-utc", 0, 0, G_OPTION_ARG_NONE, &no_utc, "Disable UTC second labels", NULL},
        {"always-track", 0, 0, G_OPTION_ARG_NONE, &always_track, "Keep GNSS tracking active continuously", NULL},
        {"debug", 'd', 0, G_OPTION_ARG_NONE, &debug, "Enable verbose debug logging", NULL},
        {"version", 'v', 0, G_OPTION_ARG_NONE, &show_version, "Show daemon version and exit", NULL},
        {0}
    };
    g_autoptr(GOptionContext) options = g_option_context_new ("- LM-G820N GNSS daemon");
    g_option_context_add_main_entries (options, entries, NULL);
    if (!g_option_context_parse (options, &argc, &argv, &error)) {
        g_printerr ("Error parsing options: %s\n", error->message);
        return 1;
    }
    if (show_version) {
        g_print ("lmg820n-gnssd %s\n", DAEMON_VERSION);
        return 0;
    }
    if (debug)
        g_setenv ("G_MESSAGES_DEBUG", "all", TRUE);
    if (!socket_path)
        socket_path = g_strdup (DEFAULT_GNSS_SOCKET_PATH);
    if (!no_utc) {
        if (!utc_socket)
            utc_socket = g_strdup (DEFAULT_UTC_SOCKET_PATH);
        daemon.utc = gnss_utc_new (utc_socket, &error);
        if (!daemon.utc) {
            g_printerr ("Failed to configure UTC second labels: %s\n", error->message);
            return 1;
        }
    }
    g_message ("Starting lmg820n-gnssd %s", DAEMON_VERSION);
    daemon.loop = g_main_loop_new (NULL, FALSE);
    gnss_state_init (&daemon.sm);
    daemon.always_track = always_track;
    if (daemon.always_track) {
        daemon.last_client_count = 1;
        apply_action (&daemon, gnss_state_on_client_added (&daemon.sm));
    }
    daemon.server = gnss_server_new (socket_path, &error);
    if (!daemon.server) {
        g_printerr ("Failed to create NMEA server: %s\n", error->message);
        gnss_utc_free (daemon.utc);
        g_main_loop_unref (daemon.loop);
        return 1;
    }
    gnss_server_set_client_count_callback (daemon.server, on_server_client_count, &daemon);
    daemon.loc = gnss_loc_new ();
    gnss_loc_set_callbacks (daemon.loc, on_loc_nmea, on_loc_ready, on_loc_session,
                            on_loc_error, &daemon);
    daemon.dbus = g_bus_get_sync (G_BUS_TYPE_SYSTEM, NULL, &error);
    if (daemon.dbus) {
        g_autoptr(GDBusProxy) login = g_dbus_proxy_new_sync (
            daemon.dbus, G_DBUS_PROXY_FLAGS_DO_NOT_AUTO_START, NULL, "org.freedesktop.login1",
            "/org/freedesktop/login1", "org.freedesktop.login1.Manager", NULL, &error);
        g_autoptr(GVariant) delay = login ? g_dbus_proxy_get_cached_property (login, "InhibitDelayMaxUSec") : NULL;
        if (delay) {
            guint64 delay_ms = g_variant_get_uint64 (delay) / 1000;
            daemon.sleep_delay_ms = delay_ms > CLEANUP_RESERVE_MS ?
                (guint)MIN (delay_ms - CLEANUP_RESERVE_MS, G_MAXUINT) : 1;
        }
        if (error) {
            g_warning ("Could not read logind sleep delay: %s", error->message);
            g_clear_error (&error);
        }
        daemon.sleep_sub_id = g_dbus_connection_signal_subscribe (
            daemon.dbus, "org.freedesktop.login1", "org.freedesktop.login1.Manager",
            "PrepareForSleep", "/org/freedesktop/login1", NULL, G_DBUS_SIGNAL_FLAGS_NONE,
            on_prepare_for_sleep, &daemon, NULL);
        take_sleep_inhibitor (&daemon);
    } else {
        g_warning ("Could not connect to system D-Bus: %s", error->message);
        g_clear_error (&error);
    }
    guint sigint = g_unix_signal_add (SIGINT, on_unix_signal, &daemon);
    guint sigterm = g_unix_signal_add (SIGTERM, on_unix_signal, &daemon);
    int notified = sd_notify (0, "READY=1");
    if (notified < 0) {
        g_warning ("Could not notify service readiness: %s", g_strerror (-notified));
        begin_shutdown (&daemon, TRUE);
    }
    g_main_loop_run (daemon.loop);
    g_source_remove (sigint);
    g_source_remove (sigterm);
    clear_deadline (&daemon);
    if (daemon.sleep_sub_id)
        g_dbus_connection_signal_unsubscribe (daemon.dbus, daemon.sleep_sub_id);
    release_inhibitor (&daemon);
    g_clear_object (&daemon.dbus);
    gnss_server_set_client_count_callback (daemon.server, NULL, NULL);
    gnss_server_free (daemon.server);
    gnss_loc_free (daemon.loc);
    gnss_utc_free (daemon.utc);
    g_main_loop_unref (daemon.loop);
    return daemon.exit_status;
}
