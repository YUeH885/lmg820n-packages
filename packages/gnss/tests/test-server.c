#include "../src/gnss-server.c"
#include <glib.h>
#include <glib/gstdio.h>
#include <gio/gio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/socket.h>

static void
test_server_basic (void)
{
    g_autofree gchar *tmp_dir = g_dir_make_tmp ("gnss-test-server-XXXXXX", NULL);
    g_autofree gchar *sock_path = g_build_filename (tmp_dir, "nmea.sock", NULL);
    g_autoptr(GError) error = NULL;
    GnssServer *server = gnss_server_new (sock_path, &error);
    struct stat st;

    g_assert_no_error (error);
    g_assert_nonnull (server);
    g_assert_true (g_file_test (sock_path, G_FILE_TEST_EXISTS));

    g_assert_cmpint (stat (sock_path, &st), ==, 0);
    g_assert_true (S_ISSOCK (st.st_mode));
    g_assert_cmpint (st.st_mode & 0777, ==, 0660);

    /* Connect client 1 */
    g_autoptr(GSocketClient) sclient1 = g_socket_client_new ();
    g_autoptr(GSocketAddress) addr1 = g_unix_socket_address_new_with_type (sock_path, -1, G_UNIX_SOCKET_ADDRESS_PATH);
    g_autoptr(GSocketConnection) conn1 = g_socket_client_connect (sclient1, G_SOCKET_CONNECTABLE (addr1), NULL, &error);
    g_assert_no_error (error);
    g_assert_nonnull (conn1);

    while (gnss_server_get_client_count (server) < 1)
        g_main_context_iteration (NULL, TRUE);

    g_assert_cmpuint (gnss_server_get_client_count (server), ==, 1);

    /* Connect client 2 */
    g_autoptr(GSocketClient) sclient2 = g_socket_client_new ();
    g_autoptr(GSocketAddress) addr2 = g_unix_socket_address_new_with_type (sock_path, -1, G_UNIX_SOCKET_ADDRESS_PATH);
    g_autoptr(GSocketConnection) conn2 = g_socket_client_connect (sclient2, G_SOCKET_CONNECTABLE (addr2), NULL, &error);
    g_assert_no_error (error);
    g_assert_nonnull (conn2);

    while (gnss_server_get_client_count (server) < 2)
        g_main_context_iteration (NULL, TRUE);

    g_assert_cmpuint (gnss_server_get_client_count (server), ==, 2);

    /* Broadcast message */
    const gchar *msg = "$GPGGA,123519,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,*47\r\n";
    gnss_server_broadcast_sentence (server, msg);

    /* Read from client 1 */
    GInputStream *in1 = g_io_stream_get_input_stream (G_IO_STREAM (conn1));
    gchar buf1[128] = {0};
    gssize n1 = g_input_stream_read (in1, buf1, sizeof (buf1) - 1, NULL, &error);
    g_assert_no_error (error);
    g_assert_cmpint (n1, ==, strlen (msg));
    g_assert_cmpstr (buf1, ==, msg);

    /* Read from client 2 */
    GInputStream *in2 = g_io_stream_get_input_stream (G_IO_STREAM (conn2));
    gchar buf2[128] = {0};
    gssize n2 = g_input_stream_read (in2, buf2, sizeof (buf2) - 1, NULL, &error);
    g_assert_no_error (error);
    g_assert_cmpint (n2, ==, strlen (msg));
    g_assert_cmpstr (buf2, ==, msg);

    /* Close client 1 */
    g_io_stream_close (G_IO_STREAM (conn1), NULL, NULL);

    while (gnss_server_get_client_count (server) > 1)
        g_main_context_iteration (NULL, TRUE);

    g_assert_cmpuint (gnss_server_get_client_count (server), ==, 1);

    /* Close client 2 */
    g_io_stream_close (G_IO_STREAM (conn2), NULL, NULL);

    while (gnss_server_get_client_count (server) > 0)
        g_main_context_iteration (NULL, TRUE);

    g_assert_cmpuint (gnss_server_get_client_count (server), ==, 0);

    gnss_server_free (server);
    g_rmdir (tmp_dir);
}

static void
test_server_disconnect_during_stream (void)
{
    g_autofree gchar *tmp_dir = g_dir_make_tmp ("gnss-test-stream-XXXXXX", NULL);
    g_autofree gchar *sock_path = g_build_filename (tmp_dir, "nmea.sock", NULL);
    g_autoptr(GError) error = NULL;
    GnssServer *server = gnss_server_new (sock_path, &error);

    g_assert_no_error (error);
    g_assert_nonnull (server);

    g_autoptr(GSocketClient) sclient = g_socket_client_new ();
    g_autoptr(GSocketAddress) addr = g_unix_socket_address_new_with_type (sock_path, -1, G_UNIX_SOCKET_ADDRESS_PATH);
    g_autoptr(GSocketConnection) conn = g_socket_client_connect (sclient, G_SOCKET_CONNECTABLE (addr), NULL, &error);
    g_assert_no_error (error);
    g_assert_nonnull (conn);

    while (gnss_server_get_client_count (server) < 1)
        g_main_context_iteration (NULL, TRUE);

    const gchar *msg = "$GPGGA,123519,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,*47\r\n";
    for (int i = 0; i < 20; i++) {
        gnss_server_broadcast_sentence (server, msg);
    }

    /* Abruptly close while server has pending writes or just wrote */
    g_io_stream_close (G_IO_STREAM (conn), NULL, NULL);

    for (int i = 0; i < 20; i++) {
        gnss_server_broadcast_sentence (server, msg);
    }

    while (gnss_server_get_client_count (server) > 0)
        g_main_context_iteration (NULL, TRUE);

    g_assert_cmpuint (gnss_server_get_client_count (server), ==, 0);

    /* Spin iterations to ensure all pending callbacks resolve cleanly */
    for (int i = 0; i < 10; i++)
        g_main_context_iteration (NULL, FALSE);

    gnss_server_free (server);
    g_rmdir (tmp_dir);
}

static void
test_slow_writer_integrity (void)
{
    g_autofree gchar *dir = g_dir_make_tmp ("gnss-slow-writer-XXXXXX", NULL);
    g_autofree gchar *path = g_build_filename (dir, "nmea.sock", NULL);
    g_autoptr(GError) error = NULL;
    GnssServer *server = gnss_server_new (path, &error);
    g_assert_no_error (error);
    g_autoptr(GSocketClient) socket_client = g_socket_client_new ();
    g_autoptr(GSocketAddress) address = g_unix_socket_address_new (path);
    g_autoptr(GSocketConnection) connection = g_socket_client_connect (
        socket_client, G_SOCKET_CONNECTABLE (address), NULL, &error);
    g_assert_no_error (error);
    while (!gnss_server_get_client_count (server))
        g_main_context_iteration (NULL, TRUE);
    GnssClientContext *client = server->clients->data;
    GSocket *out = g_socket_connection_get_socket (client->connection);
    g_assert_true (g_socket_set_option (out, SOL_SOCKET, SO_SNDBUF, 1024, &error));
    g_assert_no_error (error);
    gchar expected[40000];
    for (guint i = 0; i < sizeof expected; i++)
        expected[i] = 'A' + i % 23;
    gnss_server_broadcast (server, expected, 1024);
    gnss_server_broadcast (server, expected + 1024, sizeof expected - 1024);
    GSocket *in = g_socket_connection_get_socket (connection);
    g_socket_set_blocking (in, FALSE);
    gchar received[sizeof expected];
    gsize count = 0;
    gint64 deadline = g_get_monotonic_time () + 5 * G_USEC_PER_SEC;
    while (count < sizeof received && g_get_monotonic_time () < deadline) {
        while (g_main_context_iteration (NULL, FALSE));
        gssize bytes = g_socket_receive (in, received + count, sizeof received - count, NULL, &error);
        if (bytes > 0)
            count += bytes;
        else {
            g_assert_error (error, G_IO_ERROR, G_IO_ERROR_WOULD_BLOCK);
            g_clear_error (&error);
            g_usleep (1000);
        }
    }
    g_assert_cmpmem (received, count, expected, sizeof expected);
    gnss_server_free (server);
    for (guint i = 0; i < 10; i++)
        g_main_context_iteration (NULL, FALSE);
    g_rmdir (dir);
}

int
main (int argc, char **argv)
{
    g_test_init (&argc, &argv, NULL);
    g_test_add_func ("/server/basic", test_server_basic);
    g_test_add_func ("/server/disconnect_during_stream", test_server_disconnect_during_stream);
    g_test_add_func ("/server/slow-writer-integrity", test_slow_writer_integrity);
    return g_test_run ();
}
