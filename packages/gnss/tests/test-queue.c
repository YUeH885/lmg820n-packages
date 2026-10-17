#include "../src/gnss-server.h"
#include <glib.h>
#include <glib/gstdio.h>
#include <gio/gio.h>
#include <string.h>

typedef struct {
    GMainLoop *loop;
    gboolean disconnected;
} TestQueueContext;

static void
on_client_count_changed (guint count, gpointer user_data)
{
    TestQueueContext *ctx = (TestQueueContext *) user_data;
    if (count == 0) {
        ctx->disconnected = TRUE;
        if (ctx->loop)
            g_main_loop_quit (ctx->loop);
    }
}

static void
test_server_queue_overflow (void)
{
    g_autofree gchar *tmp_dir = g_dir_make_tmp ("gnss-test-queue-XXXXXX", NULL);
    g_autofree gchar *sock_path = g_build_filename (tmp_dir, "nmea.sock", NULL);
    g_autoptr(GError) error = NULL;
    GnssServer *server = gnss_server_new (sock_path, &error);
    g_assert_no_error (error);
    g_assert_nonnull (server);

    TestQueueContext ctx = {0};
    ctx.loop = g_main_loop_new (NULL, FALSE);
    gnss_server_set_client_count_callback (server, on_client_count_changed, &ctx);

    /* Connect a client socket but do not read from it to simulate a frozen/slow client */
    g_autoptr(GSocketClient) client = g_socket_client_new ();
    g_autoptr(GSocketAddress) addr = g_unix_socket_address_new_with_type (sock_path, -1, G_UNIX_SOCKET_ADDRESS_PATH);
    g_autoptr(GSocketConnection) conn = g_socket_client_connect (client, G_SOCKET_CONNECTABLE (addr), NULL, &error);
    g_assert_no_error (error);
    g_assert_nonnull (conn);

    /* Run main loop briefly to process accept */
    while (gnss_server_get_client_count (server) < 1)
        g_main_context_iteration (NULL, TRUE);

    g_assert_cmpuint (gnss_server_get_client_count (server), ==, 1);

    /* Expect the slow consumer warning */
    g_test_expect_message (G_LOG_DOMAIN, G_LOG_LEVEL_WARNING, "*exceeded bounded queue*");

    /* Send oversized chunks that exceed MAX_CLIENT_QUEUE_BYTES */
    gchar junk_chunk[8192];
    memset (junk_chunk, 'A', sizeof (junk_chunk));

    /* Broadcast chunks until client overflows */
    for (int i = 0; i < 20; i++) {
        gnss_server_broadcast (server, junk_chunk, sizeof (junk_chunk));
    }

    /* Process main loop: client should have been dropped */
    while (gnss_server_get_client_count (server) > 0)
        g_main_context_iteration (NULL, TRUE);

    g_test_assert_expected_messages ();
    g_assert_true (ctx.disconnected);
    g_assert_cmpuint (gnss_server_get_client_count (server), ==, 0);

    g_main_loop_unref (ctx.loop);
    gnss_server_free (server);
    g_rmdir (tmp_dir);
}

int
main (int argc, char **argv)
{
    g_test_init (&argc, &argv, NULL);
    g_test_add_func ("/server/queue_overflow", test_server_queue_overflow);
    return g_test_run ();
}
