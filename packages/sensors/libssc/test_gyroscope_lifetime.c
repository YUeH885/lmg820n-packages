/* SPDX-License-Identifier: GPL-3.0-or-later */
#define G_LOG_DOMAIN "libssc"
#include "libssc-sensor-gyroscope.c"
#include "ssc-calibration.pb-c.h"

static void calibrate(SSCSensorGyroscope *sensor)
{
	SSCSensorGyroscopePrivate *priv = ssc_sensor_gyroscope_get_instance_private(sensor);
	SscCalibrationEvent msg = SSC_CALIBRATION_EVENT__INIT;
	GArray *bytes = g_array_new(FALSE, FALSE, 1);

	msg.status = SSC_ACCURACY_HIGH;
	g_array_set_size(bytes, protobuf_c_message_get_packed_size(&msg.base));
	protobuf_c_message_pack(&msg.base, (uint8_t *)bytes->data);
	g_assert_true(ssc_calibration_update(&priv->calibration, bytes));
	g_array_unref(bytes);
}

static void finalized(gpointer data, GObject *object)
{
	*(gboolean *)data = TRUE;
}

static void test_pending_report(void)
{
	SSCSensorGyroscope *sensor = g_object_new(SSC_TYPE_SENSOR_GYROSCOPE,
		SSC_SENSOR_UID_HIGH, (guint64)1, SSC_SENSOR_UID_LOW, (guint64)1, NULL);
	SscGyroscopeResponse msg = SSC_GYROSCOPE_RESPONSE__INIT;
	float velocity[] = {0.1f, 0.2f, 0.3f};
	GArray *bytes = g_array_new(FALSE, FALSE, 1);
	gboolean gone = FALSE;

	msg.n_velocity = G_N_ELEMENTS(velocity);
	msg.velocity = velocity;
	msg.accuracy = 3;
	calibrate(sensor);
	g_array_set_size(bytes, protobuf_c_message_get_packed_size(&msg.base));
	protobuf_c_message_pack(&msg.base, (uint8_t *)bytes->data);
	g_object_weak_ref(G_OBJECT(sensor), finalized, &gone);
	report_received(NULL, SSC_MSG_REPORT_MEASUREMENT, 1, 1, bytes, sensor);
	g_array_unref(bytes);
	g_object_unref(sensor);
	g_assert_false(gone);
	while (g_main_context_iteration(NULL, FALSE))
		;
	g_assert_true(gone);
}

static void measurement(SSCSensorGyroscope *sensor, gfloat x, gfloat y,
                        gfloat z, gpointer data)
{
	guint *count = data;

	g_assert_cmpfloat(x, ==, 34.906555f);
	(*count)++;
}

static void test_accuracy(void)
{
	SSCSensorGyroscope *sensor = g_object_new(SSC_TYPE_SENSOR_GYROSCOPE,
		SSC_SENSOR_UID_HIGH, (guint64)1, SSC_SENSOR_UID_LOW, (guint64)1, NULL);
	SscGyroscopeResponse msg = SSC_GYROSCOPE_RESPONSE__INIT;
	float velocity[] = {34.906555f, 34.906555f, -34.906555f};
	GArray *bytes = g_array_new(FALSE, FALSE, 1);
	guint emitted = 0;

	msg.n_velocity = G_N_ELEMENTS(velocity);
	msg.velocity = velocity;
	calibrate(sensor);
	g_signal_connect(sensor, "measurement", G_CALLBACK(measurement), &emitted);
	for (gint accuracy = SSC_ACCURACY_UNRELIABLE; accuracy <= SSC_ACCURACY_HIGH; accuracy++) {
		msg.accuracy = accuracy;
		g_array_set_size(bytes, protobuf_c_message_get_packed_size(&msg.base));
		protobuf_c_message_pack(&msg.base, (uint8_t *)bytes->data);
		report_received(NULL, SSC_MSG_REPORT_MEASUREMENT, 1, 1, bytes, sensor);
		while (g_main_context_iteration(NULL, FALSE))
			;
		g_assert_cmpuint(emitted, ==, (guint)accuracy);
	}
	g_array_unref(bytes);
	g_object_unref(sensor);
}

int main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	g_test_add_func("/gyroscope/pending-report", test_pending_report);
	g_test_add_func("/gyroscope/accuracy", test_accuracy);
	return g_test_run();
}
