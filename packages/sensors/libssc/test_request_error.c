/* SPDX-License-Identifier: GPL-3.0-or-later */
#define G_LOG_DOMAIN "libssc"
#include "libssc-sensor.c"

static void task_finalized(gpointer data, GObject *object)
{
	*(gboolean *)data = TRUE;
}

static void test_request_error(gconstpointer callback)
{
	GTask *result = g_task_new(NULL, NULL, NULL, NULL);
	GTask *task = g_task_new(NULL, NULL, NULL, NULL);
	gboolean finalized = FALSE;
	GAsyncReadyCallback ready = (GAsyncReadyCallback)callback;

	g_object_weak_ref(G_OBJECT(task), task_finalized, &finalized);
	g_task_return_new_error(result, G_IO_ERROR, G_IO_ERROR_BROKEN_PIPE,
		"SSC transport disconnected");
	ready(NULL, G_ASYNC_RESULT(result), task);
	while (g_main_context_iteration(NULL, FALSE))
		;
	g_assert_true(finalized);
	g_object_unref(result);
}

int main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	g_log_set_debug_enabled(TRUE);
	g_test_add_data_func("/sensor/enable-error", sensor_open_ready, test_request_error);
	g_test_add_data_func("/sensor/disable-error", sensor_close_ready, test_request_error);
	return g_test_run();
}
