/* SPDX-License-Identifier: GPL-3.0-or-later */
#define G_LOG_DOMAIN "libssc"
#include <math.h>
#include "libssc-sensor-proximity.c"

static guint count;
static gboolean latest;

static void measurement(SSCSensorProximity *sensor, gboolean near, gpointer data)
{
	count++;
	latest = near;
}

static void drain(void)
{
	while (g_main_context_iteration(NULL, FALSE))
		;
}

static void deliver(SSCSensorProximity *sensor, ProtobufCMessage *message, guint id)
{
	GArray *bytes = g_array_new(FALSE, FALSE, 1);
	g_array_set_size(bytes, protobuf_c_message_get_packed_size(message));
	protobuf_c_message_pack(message, (uint8_t *)bytes->data);
	report_received(NULL, id, 1, 1, bytes, sensor);
	g_array_unref(bytes);
}

static SSCSensorProximity *make_sensor(void)
{
	SSCSensorProximity *sensor = g_object_new(SSC_TYPE_SENSOR_PROXIMITY,
		SSC_SENSOR_UID_HIGH, (guint64)1, SSC_SENSOR_UID_LOW, (guint64)1, NULL);
	g_signal_connect(sensor, "measurement", G_CALLBACK(measurement), NULL);
	return sensor;
}

static void test_states(void)
{
	g_autoptr(SSCSensorProximity) sensor = make_sensor();
	SscProximityResponse msg = SSC_PROXIMITY_RESPONSE__INIT;
	const int states[] = {1, 0, 3, 2, 2};
	const gboolean expected[] = {TRUE, FALSE, TRUE, FALSE, FALSE};
	count = 0;
	for (guint i = 0; i < G_N_ELEMENTS(states); i++) {
		msg.near = states[i];
		deliver(sensor, &msg.base, SSC_MSG_REPORT_MEASUREMENT_PROXIMITY);
		drain();
		g_assert_cmpint(latest, ==, expected[i]);
	}
	g_assert_cmpuint(count, ==, 4);
	msg.near = 9;
	g_test_expect_message("libssc", G_LOG_LEVEL_WARNING, "Unknown proximity state: 9");
	deliver(sensor, &msg.base, SSC_MSG_REPORT_MEASUREMENT_PROXIMITY);
	g_test_assert_expected_messages();
	drain();
	g_assert_cmpuint(count, ==, 4);
}

static void test_davinci(void)
{
	g_autoptr(SSCSensorProximity) sensor = make_sensor();
	SscProximityResponseDavinci msg = SSC_PROXIMITY_RESPONSE_DAVINCI__INIT;
	ProximityDataDavinci data = {0};
	ProtobufCBinaryData bytes = {sizeof(data), (uint8_t *)&data};
	count = 0;
	/* An empty repeated field is a valid protobuf message. */
	deliver(sensor, &msg.base, SSC_MSG_REPORT_MEASUREMENT);
	g_assert_cmpuint(count, ==, 0);
	msg.n_data = 1;
	msg.data = &bytes;
	data.near = NAN;
	g_test_expect_message("libssc", G_LOG_LEVEL_WARNING, "Unknown Xiaomi Davinci proximity state:*");
	deliver(sensor, &msg.base, SSC_MSG_REPORT_MEASUREMENT);
	g_test_assert_expected_messages();
	data.near = 3;
	deliver(sensor, &msg.base, SSC_MSG_REPORT_MEASUREMENT);
	drain();
	g_assert_cmpuint(count, ==, 1);
	g_assert_true(latest);
}

static void finalized(gpointer data, GObject *object)
{
	*(gboolean *)data = TRUE;
}

static void test_pending_signal_lifetime(void)
{
	SSCSensorProximity *sensor = make_sensor();
	SscProximityResponse msg = SSC_PROXIMITY_RESPONSE__INIT;
	gboolean gone = FALSE;
	g_object_weak_ref(G_OBJECT(sensor), finalized, &gone);
	deliver(sensor, &msg.base, SSC_MSG_REPORT_MEASUREMENT_PROXIMITY);
	g_object_unref(sensor);
	g_assert_false(gone);
	drain();
	g_assert_true(gone);
}

int main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	g_test_add_func("/proximity/states", test_states);
	g_test_add_func("/proximity/davinci", test_davinci);
	g_test_add_func("/proximity/pending-signal", test_pending_signal_lifetime);
	return g_test_run();
}
