#include "gnss-request.h"

typedef struct {
    guint completions;
    guint parses;
    gboolean success;
    GError *error;
} Result;

static gboolean
parse (gpointer output, gpointer data, GError **error)
{
    Result *result = data;
    result->parses++;
    if (!output) {
        g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED, "Rejected indication");
        return FALSE;
    }
    return TRUE;
}

static void
completed (GObject *source, GAsyncResult *async_result, gpointer data)
{
    (void)source;
    Result *result = data;
    result->completions++;
    result->success = g_task_propagate_boolean (G_TASK (async_result), &result->error);
}

static void
wait_result (Result *result)
{
    while (!result->completions)
        g_main_context_iteration (NULL, TRUE);
    g_assert_cmpuint (result->completions, ==, 1);
}

static gboolean
accept (gpointer output, gpointer data, GError **error)
{
    (void)output; (void)error;
    ((Result *)data)->parses++;
    return TRUE;
}

static void
test_order (gconstpointer data)
{
    gboolean indication_first = GPOINTER_TO_INT (data);
    g_autoptr(QmiClientLoc) client = g_object_new (QMI_TYPE_CLIENT_LOC, NULL);
    Result result = {0};
    g_autoptr(GTask) request = gnss_request_new (client, "set-operation-mode", 1000,
        NULL, accept, &result, completed, &result);
    if (indication_first)
        g_signal_emit_by_name (client, "set-operation-mode", NULL);
    else
        gnss_request_response (request, NULL);
    while (g_main_context_iteration (NULL, FALSE));
    g_assert_cmpuint (result.completions, ==, 0);
    if (indication_first)
        gnss_request_response (request, NULL);
    else
        g_signal_emit_by_name (client, "set-operation-mode", NULL);
    wait_result (&result);
    g_assert_true (result.success);
    g_assert_no_error (result.error);
    g_signal_emit_by_name (client, "set-operation-mode", NULL);
    g_assert_cmpuint (result.parses, ==, 1);
}

static void
test_rejected (void)
{
    g_autoptr(QmiClientLoc) client = g_object_new (QMI_TYPE_CLIENT_LOC, NULL);
    Result result = {0};
    g_autoptr(GTask) request = gnss_request_new (client, "set-operation-mode", 1000,
        NULL, parse, &result, completed, &result);
    g_signal_emit_by_name (client, "set-operation-mode", NULL);
    wait_result (&result);
    g_assert_false (result.success);
    g_assert_error (result.error, G_IO_ERROR, G_IO_ERROR_FAILED);
    g_clear_error (&result.error);
    gnss_request_response (request, NULL);
}

static void
test_timeout (void)
{
    g_autoptr(QmiClientLoc) client = g_object_new (QMI_TYPE_CLIENT_LOC, NULL);
    Result result = {0};
    g_autoptr(GTask) request = gnss_request_new (client, "set-operation-mode", 1,
        NULL, accept, &result, completed, &result);
    gnss_request_response (request, NULL);
    wait_result (&result);
    g_assert_error (result.error, G_IO_ERROR, G_IO_ERROR_TIMED_OUT);
    g_clear_error (&result.error);
    g_signal_emit_by_name (client, "set-operation-mode", NULL);
    g_assert_cmpuint (result.parses, ==, 0);
}

static void
test_cancel (void)
{
    g_autoptr(QmiClientLoc) client = g_object_new (QMI_TYPE_CLIENT_LOC, NULL);
    g_autoptr(GCancellable) cancellable = g_cancellable_new ();
    Result result = {0};
    g_autoptr(GTask) request = gnss_request_new (client, "set-operation-mode", 1000,
        cancellable, accept, &result, completed, &result);
    g_cancellable_cancel (cancellable);
    wait_result (&result);
    g_assert_error (result.error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
    g_clear_error (&result.error);
    gnss_request_response (request, g_error_new_literal (G_IO_ERROR, G_IO_ERROR_FAILED, "Late response"));
    g_signal_emit_by_name (client, "set-operation-mode", NULL);
    g_assert_cmpuint (result.parses, ==, 0);
}

static void
test_cancel_after_response (void)
{
    g_autoptr(QmiClientLoc) client = g_object_new (QMI_TYPE_CLIENT_LOC, NULL);
    g_autoptr(GCancellable) cancellable = g_cancellable_new ();
    Result result = {0};
    GTask *request = gnss_request_new (client, "set-operation-mode", 1000,
        cancellable, accept, &result, completed, &result);
    gnss_request_response (request, NULL);
    g_object_unref (request);
    g_cancellable_cancel (cancellable);
    wait_result (&result);
    g_assert_error (result.error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
    g_clear_error (&result.error);
    g_signal_emit_by_name (client, "set-operation-mode", NULL);
    g_assert_cmpuint (result.parses, ==, 0);
}

int
main (int argc, char **argv)
{
    g_test_init (&argc, &argv, NULL);
    g_test_add_data_func ("/request/indication-first", GINT_TO_POINTER (TRUE), test_order);
    g_test_add_data_func ("/request/response-first", GINT_TO_POINTER (FALSE), test_order);
    g_test_add_func ("/request/rejected", test_rejected);
    g_test_add_func ("/request/timeout", test_timeout);
    g_test_add_func ("/request/cancel", test_cancel);
    g_test_add_func ("/request/cancel-after-response", test_cancel_after_response);
    return g_test_run ();
}
