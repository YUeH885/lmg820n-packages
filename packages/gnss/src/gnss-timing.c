#include "gnss-server.h"

#include <gio/gunixsocketaddress.h>
#include <glib-unix.h>
#include <systemd/sd-daemon.h>

typedef struct {
    GMainLoop *loop;
    gint exit_status;
} GnssTiming;

static gboolean
on_socket_ready (GSocket *socket, GIOCondition condition, gpointer data)
{
    GnssTiming *timing = data;
    g_autoptr(GError) error = NULL;
    gchar buffer[4096];

    if (condition & G_IO_IN) {
        gssize received = g_socket_receive (socket, buffer, sizeof buffer, NULL, &error);
        if (received > 0)
            return G_SOURCE_CONTINUE;
        if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_WOULD_BLOCK))
            return G_SOURCE_CONTINUE;
    }
    g_warning ("GNSS subscription closed: %s", error ? error->message : "connection ended");
    timing->exit_status = 1;
    g_main_loop_quit (timing->loop);
    return G_SOURCE_REMOVE;
}

static gboolean
on_unix_signal (gpointer data)
{
    GnssTiming *timing = data;
    g_main_loop_quit (timing->loop);
    return G_SOURCE_CONTINUE;
}

int
main (int argc, char **argv)
{
    g_autoptr(GError) error = NULL;
    g_autofree gchar *socket_path = NULL;
    gboolean show_version = FALSE;
    GOptionEntry entries[] = {
        {"socket", 's', 0, G_OPTION_ARG_STRING, &socket_path, "Path to NMEA Unix socket", "PATH"},
        {"version", 'v', 0, G_OPTION_ARG_NONE, &show_version, "Show companion version and exit", NULL},
        {0}
    };
    g_autoptr(GOptionContext) options = g_option_context_new ("- LM-G820N GNSS timing companion");
    g_option_context_add_main_entries (options, entries, NULL);
    if (!g_option_context_parse (options, &argc, &argv, &error)) {
        g_printerr ("Error parsing options: %s\n", error->message);
        return 1;
    }
    if (show_version) {
        g_print ("lmg820n-gnss-timing %s\n", DAEMON_VERSION);
        return 0;
    }
    if (!socket_path)
        socket_path = g_strdup (DEFAULT_GNSS_SOCKET_PATH);
    g_autoptr(GSocketClient) client = g_socket_client_new ();
    g_autoptr(GSocketAddress) address = g_unix_socket_address_new (socket_path);
    g_autoptr(GSocketConnection) connection = g_socket_client_connect (
        client, G_SOCKET_CONNECTABLE (address), NULL, &error);
    if (!connection) {
        g_printerr ("Could not subscribe to GNSS: %s\n", error->message);
        return 1;
    }

    GnssTiming timing = { .loop = g_main_loop_new (NULL, FALSE) };
    GSocket *socket = g_socket_connection_get_socket (connection);
    g_socket_set_blocking (socket, FALSE);
    GSource *source = g_socket_create_source (socket, G_IO_IN | G_IO_HUP | G_IO_ERR, NULL);
    g_source_set_callback (source, G_SOURCE_FUNC (on_socket_ready), &timing, NULL);
    g_source_attach (source, NULL);
    guint sigint = g_unix_signal_add (SIGINT, on_unix_signal, &timing);
    guint sigterm = g_unix_signal_add (SIGTERM, on_unix_signal, &timing);

    int notified = sd_notify (0, "READY=1");
    if (notified < 0) {
        g_printerr ("Could not notify service readiness: %s\n", g_strerror (-notified));
        timing.exit_status = 1;
    } else {
        g_message ("GNSS timing subscription active: %s", socket_path);
        g_main_loop_run (timing.loop);
    }

    g_source_remove (sigint);
    g_source_remove (sigterm);
    g_source_destroy (source);
    g_source_unref (source);
    g_main_loop_unref (timing.loop);
    if (!g_io_stream_close (G_IO_STREAM (connection), NULL, &error)) {
        g_warning ("Could not close GNSS subscription: %s", error->message);
        timing.exit_status = 1;
    }
    return timing.exit_status;
}
