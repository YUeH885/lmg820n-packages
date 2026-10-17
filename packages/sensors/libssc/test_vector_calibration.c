/* SPDX-License-Identifier: GPL-3.0-or-later */
#define G_LOG_DOMAIN "libssc"
#include "libssc-calibration.c"

static gboolean update(SSCVectorCalibration *cal, SscCalibrationEvent *event)
{
	GArray *bytes = g_array_new(FALSE, FALSE, 1);
	gboolean success;

	g_array_set_size(bytes, protobuf_c_message_get_packed_size(&event->base));
	protobuf_c_message_pack(&event->base, (uint8_t *)bytes->data);
	success = ssc_calibration_update(cal, bytes);
	g_array_unref(bytes);
	return success;
}

static void test_bias(void)
{
	SSCVectorCalibration cal = {0};
	SscCalibrationEvent event = SSC_CALIBRATION_EVENT__INIT;
	gfloat bias[] = {227, -203, 78};
	gfloat x = 257, y = -183, z = 98;

	ssc_calibration_init(&cal);
	g_assert_false(ssc_calibration_apply(&cal, &x, &y, &z));
	event.n_bias = 3;
	event.bias = bias;
	event.status = 1;
	g_assert_true(update(&cal, &event));
	g_assert_true(ssc_calibration_apply(&cal, &x, &y, &z));
	g_assert_cmpfloat(x, ==, 30);
	g_assert_cmpfloat(y, ==, 20);
	g_assert_cmpfloat(z, ==, 20);
	event.status = 0;
	g_assert_true(update(&cal, &event));
	g_assert_false(ssc_calibration_apply(&cal, &x, &y, &z));
	ssc_calibration_clear(&cal);
}

static void test_matrix(void)
{
	SSCVectorCalibration cal = {0};
	SscCalibrationEvent event = SSC_CALIBRATION_EVENT__INIT;
	gfloat bias[] = {1, 2, 3}, scale[] = {2, 3, 4};
	gfloat matrix[] = {0, 1, 0, 1, 0, 0, 0, 0, 2};
	gfloat x = 2, y = 4, z = 6;

	ssc_calibration_init(&cal);
	event.n_bias = event.n_scale_factor = 3;
	event.bias = bias;
	event.scale_factor = scale;
	event.n_comp_matrix = 9;
	event.comp_matrix = matrix;
	event.status = 3;
	g_assert_true(update(&cal, &event));
	g_assert_true(ssc_calibration_apply(&cal, &x, &y, &z));
	g_assert_cmpfloat(x, ==, 10);
	g_assert_cmpfloat(y, ==, 3);
	g_assert_cmpfloat(z, ==, 42);
	ssc_calibration_clear(&cal);
}

static void test_invalid(void)
{
	SSCVectorCalibration cal = {0};
	SscCalibrationEvent event = SSC_CALIBRATION_EVENT__INIT;
	gfloat bias[] = {1, 2, 3};
	gfloat x = 2, y = 4, z = 6;

	ssc_calibration_init(&cal);
	event.status = 3;
	g_assert_true(update(&cal, &event));
	event.n_bias = 2;
	event.bias = bias;
	g_test_expect_message("libssc", G_LOG_LEVEL_WARNING, "*Invalid vector calibration*");
	g_assert_false(update(&cal, &event));
	g_test_assert_expected_messages();
	g_assert_false(ssc_calibration_apply(&cal, &x, &y, &z));
	event.n_bias = 3;
	bias[1] = NAN;
	g_test_expect_message("libssc", G_LOG_LEVEL_WARNING, "*Invalid vector calibration*");
	g_assert_false(update(&cal, &event));
	g_test_assert_expected_messages();
	ssc_calibration_clear(&cal);
}

int main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	g_test_add_func("/calibration/bias", test_bias);
	g_test_add_func("/calibration/scale-matrix", test_matrix);
	g_test_add_func("/calibration/invalid", test_invalid);
	return g_test_run();
}
