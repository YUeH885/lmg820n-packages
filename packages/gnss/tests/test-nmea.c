#include "../src/gnss-nmea.h"
#include <glib.h>

static void
test_nmea_checksum (void)
{
    /* Known standard NMEA sentence */
    const gchar *sentence = "$GPGGA,123519,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,*47";
    guint8 csum = gnss_nmea_calc_checksum (sentence);
    g_assert_cmphex (csum, ==, 0x47);

    g_assert_true (gnss_nmea_verify_checksum (sentence));

    /* Tampered checksum */
    const gchar *tampered = "$GPGGA,123519,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,*48";
    g_assert_false (gnss_nmea_verify_checksum (tampered));

    /* Sentence without checksum */
    const gchar *no_csum = "$GPRMC,123519,A,4807.038,N,01131.000,E,022.4,084.4,230394,003.1,W";
    g_assert_true (gnss_nmea_verify_checksum (no_csum));
}

static void
test_nmea_validation (void)
{
    g_assert_true (gnss_nmea_is_valid_sentence ("$GPGGA,123519,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,*47"));
    g_assert_true (gnss_nmea_is_valid_sentence ("  $GPGGA,123519,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,*47\r\n"));
    g_assert_true (gnss_nmea_is_valid_sentence ("!AIVDM,1,1,,B,177KQJ001G5017K7HGj:tOA000S:,0*31"));

    /* Invalid */
    g_assert_false (gnss_nmea_is_valid_sentence (NULL));
    g_assert_false (gnss_nmea_is_valid_sentence (""));
    g_assert_false (gnss_nmea_is_valid_sentence ("junk data"));
    g_assert_false (gnss_nmea_is_valid_sentence ("$GP"));
    g_assert_false (gnss_nmea_is_valid_sentence ("$GPGGA*ZZ"));
}

static void
test_nmea_normalization (void)
{
    const gchar *raw = "$GPGGA,123519,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,";
    g_autofree gchar *norm = gnss_nmea_normalize_sentence (raw);
    g_assert_nonnull (norm);
    g_assert_cmpstr (norm, ==, "$GPGGA,123519,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,*47\r\n");

    const gchar *already_complete = "$GPGGA,123519,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,*47";
    g_autofree gchar *norm2 = gnss_nmea_normalize_sentence (already_complete);
    g_assert_nonnull (norm2);
    g_assert_cmpstr (norm2, ==, "$GPGGA,123519,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,*47\r\n");
}

static void
test_nmea_split (void)
{
    const gchar *stream =
        "\r\n\r\n"
        "$GPGGA,123519,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,*47\r\n"
        "junk line\r\n"
        "$GPGSA,A,3,04,05,,09,12,,,24,,,,,2.5,1.3,2.1*39\n"
        "\n";

    gchar **sentences = gnss_nmea_split_and_normalize (stream);
    g_assert_nonnull (sentences);
    g_assert_cmpuint (g_strv_length (sentences), ==, 2);
    g_assert_cmpstr (sentences[0], ==, "$GPGGA,123519,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,*47\r\n");
    g_assert_cmpstr (sentences[1], ==, "$GPGSA,A,3,04,05,,09,12,,,24,,,,,2.5,1.3,2.1*39\r\n");
    g_strfreev (sentences);
}

int
main (int argc, char **argv)
{
    g_test_init (&argc, &argv, NULL);
    g_test_add_func ("/nmea/checksum", test_nmea_checksum);
    g_test_add_func ("/nmea/validation", test_nmea_validation);
    g_test_add_func ("/nmea/normalization", test_nmea_normalization);
    g_test_add_func ("/nmea/split", test_nmea_split);
    return g_test_run ();
}
