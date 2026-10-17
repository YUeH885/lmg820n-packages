#include <gio/gio.h>
#include <gio/gunixsocketaddress.h>
#include <glib/gstdio.h>
#include <signal.h>

typedef struct {
    gchar *dir;
    gchar *path;
    gchar *notify_path;
    GSocket *listener;
    GSocket *notify;
    GSocket *peer;
    GSubprocess *process;
} TimingFixture;

static GSocket *
bind_socket (const gchar *path, GSocketType type)
{
    g_autoptr(GError) error = NULL;
    GSocket *socket = g_socket_new (G_SOCKET_FAMILY_UNIX, type, G_SOCKET_PROTOCOL_DEFAULT, &error);
    g_assert_no_error (error);
    g_socket_set_timeout (socket, 3);
    g_autoptr(GSocketAddress) address = g_unix_socket_address_new (path);
    g_assert_true (g_socket_bind (socket, address, FALSE, &error));
    g_assert_no_error (error);
    return socket;
}

static void
fixture_start (TimingFixture *fixture, gboolean missing)
{
    g_autoptr(GError) error = NULL;
    fixture->dir = g_dir_make_tmp ("gnss-timing-test-XXXXXX", &error);
    g_assert_no_error (error);
    fixture->path = g_build_filename (fixture->dir, "nmea.sock", NULL);
    fixture->notify_path = g_build_filename (fixture->dir, "notify.sock", NULL);
    fixture->notify = bind_socket (fixture->notify_path, G_SOCKET_TYPE_DATAGRAM);
    if (!missing) {
        fixture->listener = bind_socket (fixture->path, G_SOCKET_TYPE_STREAM);
        g_assert_true (g_socket_listen (fixture->listener, &error));
        g_assert_no_error (error);
    }
    g_autoptr(GSubprocessLauncher) launcher = g_subprocess_launcher_new (G_SUBPROCESS_FLAGS_STDERR_PIPE);
    g_subprocess_launcher_setenv (launcher, "NOTIFY_SOCKET", fixture->notify_path, TRUE);
    g_subprocess_launcher_unsetenv (launcher, "G_DEBUG");
    const gchar *binary = g_getenv ("GNSS_TIMING_TEST_BINARY");
    g_assert_nonnull (binary);
    g_auto(GStrv) wrapper_argv = NULL;
    gint wrapper_argc = 0;
    const gchar *wrapper = g_getenv ("MESON_EXE_WRAPPER");
    if (wrapper) {
        g_assert_true (g_shell_parse_argv (wrapper, &wrapper_argc, &wrapper_argv, &error));
        g_assert_no_error (error);
    }
    g_autofree const gchar **child_argv = g_new0 (const gchar *, wrapper_argc + 4);
    for (gint i = 0; i < wrapper_argc; i++)
        child_argv[i] = wrapper_argv[i];
    child_argv[wrapper_argc] = binary;
    child_argv[wrapper_argc + 1] = "--socket";
    child_argv[wrapper_argc + 2] = fixture->path;
    fixture->process = g_subprocess_launcher_spawnv (launcher, child_argv, &error);
    g_assert_no_error (error);
    if (missing)
        return;
    g_assert_true (g_socket_condition_timed_wait (fixture->listener, G_IO_IN,
                                                 3 * G_USEC_PER_SEC, NULL, &error));
    g_assert_no_error (error);
    fixture->peer = g_socket_accept (fixture->listener, NULL, &error);
    g_assert_no_error (error);
    g_socket_set_timeout (fixture->peer, 3);
    gchar notification[128];
    gssize received = g_socket_receive (fixture->notify, notification,
                                       sizeof notification, NULL, &error);
    g_assert_no_error (error);
    g_assert_cmpmem (notification, received, "READY=1", 7);
}

static void
fixture_wait (TimingFixture *fixture, gint status)
{
    g_autoptr(GError) error = NULL;
    g_assert_true (g_subprocess_wait (fixture->process, NULL, &error));
    g_assert_no_error (error);
    g_assert_true (g_subprocess_get_if_exited (fixture->process));
    g_assert_cmpint (g_subprocess_get_exit_status (fixture->process), ==, status);
}

static void
fixture_clear (TimingFixture *fixture)
{
    g_clear_object (&fixture->peer);
    g_clear_object (&fixture->listener);
    g_clear_object (&fixture->notify);
    g_clear_object (&fixture->process);
    g_unlink (fixture->path);
    g_unlink (fixture->notify_path);
    g_rmdir (fixture->dir);
    g_free (fixture->path);
    g_free (fixture->notify_path);
    g_free (fixture->dir);
}

static void
test_release (gconstpointer data)
{
    TimingFixture fixture = {0};
    fixture_start (&fixture, FALSE);
    g_autoptr(GError) error = NULL;
    gchar buffer[4096] = {0};
    /* Exceed the daemon's queue limit to exercise continuous draining. */
    for (guint i = 0; i < 64; i++) {
        gsize offset = 0;
        while (offset < sizeof buffer) {
            gssize sent = g_socket_send (fixture.peer, buffer + offset,
                                         sizeof buffer - offset, NULL, &error);
            g_assert_no_error (error);
            g_assert_cmpint (sent, >, 0);
            offset += sent;
        }
    }
    g_subprocess_send_signal (fixture.process, GPOINTER_TO_INT (data));
    fixture_wait (&fixture, 0);
    gssize received = g_socket_receive (fixture.peer, buffer, sizeof buffer, NULL, &error);
    /* Closing with unread stream data can reset the peer connection. */
    if (received < 0)
        g_assert_error (error, G_IO_ERROR, G_IO_ERROR_CONNECTION_CLOSED);
    else
        g_assert_cmpint (received, ==, 0);
    fixture_clear (&fixture);
}

static void
test_disconnect (void)
{
    TimingFixture fixture = {0};
    fixture_start (&fixture, FALSE);
    g_assert_true (g_socket_close (fixture.peer, NULL));
    fixture_wait (&fixture, 1);
    fixture_clear (&fixture);
}

static void
test_missing_socket (void)
{
    TimingFixture fixture = {0};
    fixture_start (&fixture, TRUE);
    fixture_wait (&fixture, 1);
    fixture_clear (&fixture);
}

int
main (int argc, char **argv)
{
    g_test_init (&argc, &argv, NULL);
    g_test_add_data_func ("/timing/stop", GINT_TO_POINTER (SIGTERM), test_release);
    g_test_add_data_func ("/timing/interrupt", GINT_TO_POINTER (SIGINT), test_release);
    g_test_add_func ("/timing/disconnect", test_disconnect);
    g_test_add_func ("/timing/missing-socket", test_missing_socket);
    return g_test_run ();
}
