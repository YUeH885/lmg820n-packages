#include "gnss-utc.h"
#include "gnss-nmea.h"

#include <errno.h>
#include <glib/gstdio.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

static gchar *
rmc (const gchar *time, const gchar *date, const gchar *status, const gchar *mode)
{
    g_autofree gchar *body = g_strdup_printf (
        "$GPRMC,%s,%s,2449.753604,N,11124.694214,E,0.0,290.8,%s,2.4,W,%s,V",
        time, status, date, mode);
    return g_strdup_printf ("%s*%02X\r\n", body, gnss_nmea_calc_checksum (body));
}

static void
test_parse (void)
{
    g_autofree gchar *sentence = rmc ("151813.123456789", "111026", "A", "A");
    gint64 utc_ns;
    g_assert_true (gnss_utc_parse_rmc (sentence, &utc_ns));
    g_assert_cmpint (utc_ns, ==, 1791731893123456789LL);
}

static void
test_midnight (void)
{
    g_autofree gchar *before = rmc ("235959", "290224", "A", "D");
    g_autofree gchar *after = rmc ("000000.00", "010324", "A", "D");
    gint64 first, second;
    g_assert_true (gnss_utc_parse_rmc (before, &first));
    g_assert_true (gnss_utc_parse_rmc (after, &second));
    g_assert_cmpint (second - first, ==, 1000000000);
}

typedef struct {
    const gchar *time;
    const gchar *date;
    const gchar *status;
    const gchar *mode;
} InvalidRmc;

static void
test_invalid (gconstpointer data)
{
    const InvalidRmc *value = data;
    g_autofree gchar *sentence = rmc (value->time, value->date, value->status, value->mode);
    gint64 utc_ns;
    g_assert_false (gnss_utc_parse_rmc (sentence, &utc_ns));
}

static void
test_checksum (void)
{
    g_autofree gchar *sentence = rmc ("151813.00", "111026", "A", "A");
    gint64 utc_ns;
    gchar *star = strchr (sentence, '*');
    star[1] = star[1] == '0' ? '1' : '0';
    g_assert_false (gnss_utc_parse_rmc (sentence, &utc_ns));
    *star = '\0';
    g_assert_false (gnss_utc_parse_rmc (sentence, &utc_ns));
    g_assert_false (gnss_utc_parse_rmc ("$GPRMC*", &utc_ns));
}

static void
test_delivery (void)
{
    g_autoptr(GError) error = NULL;
    g_autofree gchar *directory = g_dir_make_tmp ("gnss-utc-XXXXXX", &error);
    g_assert_no_error (error);
    g_autofree gchar *path = g_build_filename (directory, "utc.sock", NULL);
    GnssUtc *utc = gnss_utc_new (path, &error);
    g_assert_no_error (error);
    g_assert_nonnull (utc);
    g_autofree gchar *sentence = rmc ("151813.00", "111026", "A", "A");
    struct timespec received = {1791731893, 21012345};
    g_assert_false (gnss_utc_process_nmea (utc, sentence, &received));

    gint fd = socket (AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    g_assert_cmpint (fd, >=, 0);
    struct sockaddr_un address = {.sun_family = AF_UNIX};
    g_strlcpy (address.sun_path, path, sizeof address.sun_path);
    g_assert_cmpint (bind (fd, (struct sockaddr *)&address, sizeof address), ==, 0);
    g_assert_true (gnss_utc_process_nmea (utc, sentence, &received));
    struct chrony_sock_sample sample;
    g_assert_cmpint (recv (fd, &sample, sizeof sample, 0), ==, sizeof sample);
    g_assert_cmpint (sample.tv.tv_sec, ==, 1791731893);
    g_assert_cmpint (sample.tv.tv_usec, ==, 21012);
    g_assert_cmpfloat_with_epsilon (sample.offset, -0.021012, 1e-12);
    g_assert_cmpint (sample.magic, ==, 0x534f434b);
    g_assert_cmpint (sample.pulse, ==, 0);
    g_assert_cmpint (sample.leap, ==, 0);

    g_assert_false (gnss_utc_process_nmea (utc, sentence, &received));
    g_autofree gchar *old = rmc ("151812.00", "111026", "A", "A");
    g_assert_false (gnss_utc_process_nmea (utc, old, &received));
    g_assert_cmpint (recv (fd, &sample, sizeof sample, 0), ==, -1);
    g_assert_cmpint (errno, ==, EAGAIN);

    g_autofree gchar *invalid = rmc ("151814.00", "111026", "V", "A");
    g_assert_false (gnss_utc_process_nmea (utc, invalid, &received));
    g_autofree gchar *next = rmc ("151814.00", "111026", "A", "A");
    g_autofree gchar *batch = g_strconcat (invalid, next, next, NULL);
    received.tv_sec++;
    g_assert_true (gnss_utc_process_nmea (utc, batch, &received));
    g_assert_cmpint (recv (fd, &sample, sizeof sample, 0), ==, sizeof sample);
    g_assert_cmpfloat_with_epsilon (sample.offset, -0.021012, 1e-12);
    g_assert_cmpint (recv (fd, &sample, sizeof sample, 0), ==, -1);
    g_assert_cmpint (errno, ==, EAGAIN);
    gnss_utc_free (utc);
    close (fd);
    g_unlink (path);
    g_rmdir (directory);
}

static void
test_socket_path (void)
{
    g_autoptr(GError) error = NULL;
    g_assert_null (gnss_utc_new ("relative.sock", &error));
    g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
}

int
main (int argc, char **argv)
{
    g_test_init (&argc, &argv, NULL);
    g_test_add_func ("/utc/parse", test_parse);
    g_test_add_func ("/utc/midnight", test_midnight);
    g_test_add_func ("/utc/checksum", test_checksum);
    g_test_add_func ("/utc/delivery", test_delivery);
    g_test_add_func ("/utc/socket-path", test_socket_path);
    static const InvalidRmc invalid[] = {
        {"240000.00", "111026", "A", "A"},
        {"156000.00", "111026", "A", "A"},
        {"151860.00", "111026", "A", "A"},
        {"151813.", "111026", "A", "A"},
        {"151813.0000000001", "111026", "A", "A"},
        {"15x813", "111026", "A", "A"},
        {"151813.0e1", "111026", "A", "A"},
        {"151813.00", "321026", "A", "A"},
        {"151813.00", "290226", "A", "A"},
        {"151813.00", "011326", "A", "A"},
        {"151813.00", "11x026", "A", "A"},
        {"151813.00", "", "A", "A"},
        {"151813.00", "111026", "V", "A"},
        {"151813.00", "111026", "A", "N"},
        {"151813.00", "111026", "A", "E"},
    };
    for (guint i = 0; i < G_N_ELEMENTS (invalid); i++) {
        g_autofree gchar *name = g_strdup_printf ("/utc/invalid/%u", i);
        g_test_add_data_func (name, &invalid[i], test_invalid);
    }
    return g_test_run ();
}
