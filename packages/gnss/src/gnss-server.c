#include "gnss-server.h"
#include "gnss-common.h"
#include <glib/gstdio.h>
#include <sys/stat.h>
#include <unistd.h>

typedef struct {
    GnssServer *server;
    GSocketConnection *connection;
    GOutputStream *ostream;
    GInputStream *istream;
    GCancellable *cancellable;
    GByteArray *write_queue;
    GBytes *write_buffer;
    gsize write_offset;
    gboolean is_writing;
    gboolean is_reading;
    gboolean removed;
    gint ref_count;
    guint8 read_dummy[64];
} GnssClientContext;

struct _GnssServer {
    GSocketService *service;
    gchar *socket_path;
    GList *clients;
    GnssServerClientCountCb count_cb;
    gpointer count_user_data;
    gboolean active;
};

static GnssClientContext *
gnss_client_context_ref (GnssClientContext *client)
{
    if (client)
        g_atomic_int_inc (&client->ref_count);
    return client;
}

static void
gnss_client_context_unref (GnssClientContext *client)
{
    if (!client)
        return;

    if (g_atomic_int_dec_and_test (&client->ref_count)) {
        if (client->cancellable) {
            g_cancellable_cancel (client->cancellable);
            g_object_unref (client->cancellable);
        }

        if (client->ostream)
            g_object_unref (client->ostream);

        if (client->istream)
            g_object_unref (client->istream);

        if (client->connection) {
            g_io_stream_close (G_IO_STREAM (client->connection), NULL, NULL);
            g_object_unref (client->connection);
        }

        if (client->write_queue)
            g_byte_array_unref (client->write_queue);
        g_clear_pointer (&client->write_buffer, g_bytes_unref);

        g_free (client);
    }
}

static void
gnss_server_remove_client (GnssServer *server, GnssClientContext *client)
{
    if (!server || !client)
        return;

    if (client->removed)
        return;

    client->removed = TRUE;

    if (client->cancellable)
        g_cancellable_cancel (client->cancellable);

    if (g_list_find (server->clients, client)) {
        server->clients = g_list_remove (server->clients, client);
        if (server->count_cb)
            server->count_cb (g_list_length (server->clients), server->count_user_data);
    }

    gnss_client_context_unref (client);
}

static void gnss_server_client_flush_write (GnssClientContext *client);

static void
on_client_read_ready (GObject *source_object,
                      GAsyncResult *res,
                      gpointer user_data)
{
    GnssClientContext *client = (GnssClientContext *) user_data;
    GInputStream *istream = G_INPUT_STREAM (source_object);
    g_autoptr(GError) error = NULL;
    gssize bytes_read;

    client->is_reading = FALSE;
    bytes_read = g_input_stream_read_finish (istream, res, &error);

    if (client->removed || g_cancellable_is_cancelled (client->cancellable)) {
        gnss_client_context_unref (client);
        return;
    }

    if (bytes_read <= 0) {
        /* EOF or disconnect error */
        gnss_server_remove_client (client->server, client);
        gnss_client_context_unref (client);
        return;
    }

    /* Continue reading to detect EOF */
    if (!client->removed && !g_cancellable_is_cancelled (client->cancellable)) {
        client->is_reading = TRUE;
        gnss_client_context_ref (client);
        g_input_stream_read_async (client->istream,
                                   client->read_dummy,
                                   sizeof (client->read_dummy),
                                   G_PRIORITY_DEFAULT,
                                   client->cancellable,
                                   on_client_read_ready,
                                   client);
    }

    gnss_client_context_unref (client);
}

static void
on_client_write_ready (GObject *source_object,
                       GAsyncResult *res,
                       gpointer user_data)
{
    GnssClientContext *client = (GnssClientContext *) user_data;
    GOutputStream *ostream = G_OUTPUT_STREAM (source_object);
    g_autoptr(GError) error = NULL;
    gssize bytes_written;

    client->is_writing = FALSE;
    bytes_written = g_output_stream_write_finish (ostream, res, &error);

    if (client->removed || g_cancellable_is_cancelled (client->cancellable)) {
        gnss_client_context_unref (client);
        return;
    }

    if (bytes_written <= 0) {
        g_warning ("Failed to write to GNSS client: %s", error ? error->message : "unknown error");
        gnss_server_remove_client (client->server, client);
        gnss_client_context_unref (client);
        return;
    }

    client->write_offset += bytes_written;
    if (client->write_offset == g_bytes_get_size (client->write_buffer)) {
        g_clear_pointer (&client->write_buffer, g_bytes_unref);
        client->write_offset = 0;
    }
    gnss_server_client_flush_write (client);

    gnss_client_context_unref (client);
}

static void
gnss_server_client_flush_write (GnssClientContext *client)
{
    gsize size;
    const guint8 *data;

    if (client->removed || client->is_writing)
        return;

    if (g_cancellable_is_cancelled (client->cancellable))
        return;

    if (!client->write_buffer) {
        if (client->write_queue->len == 0)
            return;
        client->write_buffer = g_byte_array_free_to_bytes (client->write_queue);
        client->write_queue = g_byte_array_new ();
    }
    data = g_bytes_get_data (client->write_buffer, &size);

    client->is_writing = TRUE;
    gnss_client_context_ref (client);
    g_output_stream_write_async (client->ostream,
                                 data + client->write_offset,
                                 size - client->write_offset,
                                 G_PRIORITY_DEFAULT,
                                 client->cancellable,
                                 on_client_write_ready,
                                 client);
}

static gboolean
on_incoming_connection (GSocketService *service,
                        GSocketConnection *connection,
                        GObject *source_object,
                        gpointer user_data)
{
    (void)service;
    (void)source_object;
    GnssServer *server = (GnssServer *) user_data;
    GnssClientContext *client;

    if (!server->active)
        return FALSE;

    client = g_new0 (GnssClientContext, 1);
    client->server = server;
    client->connection = g_object_ref (connection);
    client->ostream = g_object_ref (g_io_stream_get_output_stream (G_IO_STREAM (connection)));
    client->istream = g_object_ref (g_io_stream_get_input_stream (G_IO_STREAM (connection)));
    client->cancellable = g_cancellable_new ();
    client->write_queue = g_byte_array_new ();
    client->is_writing = FALSE;
    client->is_reading = TRUE;
    client->removed = FALSE;
    client->ref_count = 1; /* Owned by server->clients list */

    server->clients = g_list_append (server->clients, client);

    /* Listen on input stream to detect EOF immediately */
    gnss_client_context_ref (client);
    g_input_stream_read_async (client->istream,
                               client->read_dummy,
                               sizeof (client->read_dummy),
                               G_PRIORITY_DEFAULT,
                               client->cancellable,
                               on_client_read_ready,
                               client);

    if (server->count_cb)
        server->count_cb (g_list_length (server->clients), server->count_user_data);

    return TRUE;
}

GnssServer *
gnss_server_new (const gchar *socket_path, GError **error)
{
    g_autofree gchar *dir = NULL;
    g_autoptr(GSocketAddress) address = NULL;
    GnssServer *server;

    if (!socket_path) {
        g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "Socket path cannot be NULL");
        return NULL;
    }

    dir = g_path_get_dirname (socket_path);
    if (g_mkdir_with_parents (dir, 0750) != 0) {
        g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                     "Failed to create directory '%s': %s", dir, g_strerror (errno));
        return NULL;
    }

    g_unlink (socket_path);

    server = g_new0 (GnssServer, 1);
    server->socket_path = g_strdup (socket_path);
    server->service = g_socket_service_new ();
    server->active = TRUE;

    address = g_unix_socket_address_new_with_type (socket_path, -1, G_UNIX_SOCKET_ADDRESS_PATH);
    if (!g_socket_listener_add_address (G_SOCKET_LISTENER (server->service),
                                        address,
                                        G_SOCKET_TYPE_STREAM,
                                        G_SOCKET_PROTOCOL_DEFAULT,
                                        NULL,
                                        NULL,
                                        error)) {
        gnss_server_free (server);
        return NULL;
    }

    /* Socket permissions: 0660 */
    chmod (socket_path, 0660);

    g_signal_connect (server->service, "incoming", G_CALLBACK (on_incoming_connection), server);
    g_socket_service_start (server->service);

    return server;
}

void
gnss_server_set_client_count_callback (GnssServer *server,
                                       GnssServerClientCountCb callback,
                                       gpointer user_data)
{
    if (!server)
        return;

    server->count_cb = callback;
    server->count_user_data = user_data;
}

guint
gnss_server_get_client_count (GnssServer *server)
{
    return server ? g_list_length (server->clients) : 0;
}

void
gnss_server_broadcast (GnssServer *server, const gchar *data, gsize len)
{
    GList *l;

    if (!server || !server->active || !data || len == 0)
        return;

    l = server->clients;
    while (l) {
        GList *next = l->next;
        GnssClientContext *client = (GnssClientContext *) l->data;

        if (client->removed) {
            l = next;
            continue;
        }

        gsize queued = client->write_queue->len;
        if (client->write_buffer)
            queued += g_bytes_get_size (client->write_buffer) - client->write_offset;
        if (queued + len > MAX_CLIENT_QUEUE_BYTES) {
            g_warning ("Slow GNSS consumer %p exceeded bounded queue (%u bytes > %d), disconnecting",
                       (void *)client, (guint)(queued + len), MAX_CLIENT_QUEUE_BYTES);
            gnss_server_remove_client (server, client);
        } else {
            g_byte_array_append (client->write_queue, (const guint8 *)data, len);
            gnss_server_client_flush_write (client);
        }

        l = next;
    }
}

void
gnss_server_broadcast_sentence (GnssServer *server, const gchar *sentence)
{
    if (sentence)
        gnss_server_broadcast (server, sentence, strlen (sentence));
}

void
gnss_server_close (GnssServer *server)
{
    if (!server || !server->active)
        return;

    server->active = FALSE;

    if (server->service) {
        g_signal_handlers_disconnect_by_data (server->service, server);
        g_socket_service_stop (server->service);
        g_socket_listener_close (G_SOCKET_LISTENER (server->service));
    }

    while (server->clients) {
        GnssClientContext *client = (GnssClientContext *) server->clients->data;
        gnss_server_remove_client (server, client);
    }

    if (server->socket_path)
        g_unlink (server->socket_path);
}

void
gnss_server_free (GnssServer *server)
{
    if (!server)
        return;

    gnss_server_close (server);

    if (server->service)
        g_object_unref (server->service);

    g_free (server->socket_path);
    g_free (server);
}
