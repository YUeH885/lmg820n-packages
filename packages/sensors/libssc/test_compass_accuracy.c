/* SPDX-License-Identifier: GPL-3.0-or-later */
#define G_LOG_DOMAIN "libssc"
#include "libssc-sensor-compass.c"

static void measurement(SSCSensorCompass *sensor, gfloat azimuth, gpointer data)
{
	guint *count = data;

	g_assert_cmpfloat(azimuth, ==, 0.0f);
	(*count)++;
}

static void test_accuracy(void)
{
	SSCSensorCompass *sensor = g_object_new(SSC_TYPE_SENSOR_COMPASS,
		SSC_SENSOR_UID_HIGH, (guint64)1, SSC_SENSOR_UID_LOW, (guint64)1, NULL);
	SscRotationvectorResponse msg = SSC_ROTATIONVECTOR_RESPONSE__INIT;
	float rotation[] = {0.0f, 0.0f, 0.0f, 1.0f};
	GArray *bytes = g_array_new(FALSE, FALSE, 1);
	guint emitted = 0;

	msg.n_rotation = G_N_ELEMENTS(rotation);
	msg.rotation = rotation;
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
	g_test_add_func("/compass/accuracy", test_accuracy);
	return g_test_run();
}
