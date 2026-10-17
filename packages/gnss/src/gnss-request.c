#include "gnss-request.h"

typedef struct {
    gulong indication_id;
    gulong cancelled_id;
    guint timeout_id;
    gboolean response_received;
    gboolean indication_received;
    gboolean done;
    GnssIndicationParser parser;
    gpointer parser_data;
} GnssRequest;

static void
finish_request (GTask *task, GError *error)
{
    g_autoptr(GTask) keep_alive = g_object_ref (task);
    GnssRequest *request = g_task_get_task_data (task);
    GCancellable *cancellable = g_task_get_cancellable (task);
    if (request->done) {
        g_clear_error (&error);
        return;
    }
    request->done = TRUE;
    if (request->indication_id) {
        g_signal_handler_disconnect (g_task_get_source_object (task), request->indication_id);
        request->indication_id = 0;
    }
    if (request->cancelled_id) {
        g_signal_handler_disconnect (cancellable, request->cancelled_id);
        request->cancelled_id = 0;
    }
    if (request->timeout_id) {
        g_source_remove (request->timeout_id);
        request->timeout_id = 0;
    }
    if (error)
        g_task_return_error (task, error);
    else
        g_task_return_boolean (task, TRUE);
}

static gboolean
request_timeout (gpointer data)
{
    GTask *task = data;
    GnssRequest *request = g_task_get_task_data (task);
    request->timeout_id = 0;
    finish_request (task, g_error_new_literal (G_IO_ERROR, G_IO_ERROR_TIMED_OUT,
                                             "LOC response/indication deadline exceeded"));
    return G_SOURCE_REMOVE;
}

static void
request_cancelled (GCancellable *cancellable, gpointer data)
{
    (void)cancellable;
    finish_request (data, g_error_new_literal (G_IO_ERROR, G_IO_ERROR_CANCELLED,
                                             "LOC request cancelled"));
}

static void
request_indication (QmiClientLoc *client, gpointer output, gpointer data)
{
    (void)client;
    GTask *task = g_object_ref (data);
    GnssRequest *request = g_task_get_task_data (task);
    g_autoptr(GError) error = NULL;
    if (!request->done && !request->indication_received) {
        if (!request->parser (output, request->parser_data, &error)) {
            if (!error)
                error = g_error_new_literal (G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                                            "Invalid LOC indication");
            finish_request (task, g_steal_pointer (&error));
        } else {
            request->indication_received = TRUE;
            if (request->response_received)
                finish_request (task, NULL);
        }
    }
    g_object_unref (task);
}

GTask *
gnss_request_new (QmiClientLoc *client, const gchar *signal, guint timeout_ms,
                  GCancellable *cancellable, GnssIndicationParser parser,
                  gpointer parser_data, GAsyncReadyCallback callback, gpointer user_data)
{
    GTask *task = g_task_new (client, cancellable, callback, user_data);
    GnssRequest *request = g_new0 (GnssRequest, 1);
    request->parser = parser;
    request->parser_data = parser_data;
    g_task_set_task_data (task, request, g_free);
    request->indication_id = g_signal_connect (client, signal, G_CALLBACK (request_indication), task);
    request->timeout_id = g_timeout_add_full (G_PRIORITY_DEFAULT, timeout_ms, request_timeout,
                                             g_object_ref (task), g_object_unref);
    if (cancellable) {
        request->cancelled_id = g_signal_connect (cancellable, "cancelled",
                                                G_CALLBACK (request_cancelled), task);
        if (g_cancellable_is_cancelled (cancellable))
            request_cancelled (cancellable, task);
    }
    return task;
}

void
gnss_request_response (GTask *task, GError *error)
{
    GnssRequest *request = g_task_get_task_data (task);
    if (request->done) {
        g_clear_error (&error);
        return;
    }
    if (error) {
        finish_request (task, error);
        return;
    }
    request->response_received = TRUE;
    if (request->indication_received)
        finish_request (task, NULL);
}
