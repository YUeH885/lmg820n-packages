#include "gnss-nmea.h"
#include <ctype.h>
#include <stdio.h>
#include <string.h>

guint8
gnss_nmea_calc_checksum (const gchar *sentence)
{
    const gchar *p;
    guint8 csum = 0;

    if (!sentence)
        return 0;

    p = sentence;
    if (*p == '$' || *p == '!')
        p++;

    while (*p && *p != '*' && *p != '\r' && *p != '\n') {
        csum ^= (guint8)(*p);
        p++;
    }

    return csum;
}

gboolean
gnss_nmea_verify_checksum (const gchar *sentence)
{
    const gchar *star;
    guint8 expected;
    guint parsed;

    if (!sentence)
        return FALSE;

    star = strchr (sentence, '*');
    if (!star)
        return TRUE;

    if (!g_ascii_isxdigit (star[1]) || !g_ascii_isxdigit (star[2]))
        return FALSE;

    if (sscanf (star + 1, "%2x", &parsed) != 1)
        return FALSE;

    expected = gnss_nmea_calc_checksum (sentence);
    return ((guint8)parsed == expected);
}

gboolean
gnss_nmea_is_valid_sentence (const gchar *sentence)
{
    const gchar *p;
    gsize len;

    if (!sentence)
        return FALSE;

    while (*sentence == ' ' || *sentence == '\t')
        sentence++;

    if (*sentence != '$' && *sentence != '!')
        return FALSE;

    len = 0;
    p = sentence + 1;
    while (*p && *p != '\r' && *p != '\n') {
        if ((guchar)*p < 0x20 || (guchar)*p > 0x7e)
            return FALSE;
        p++;
        len++;
    }

    if (len < 5)
        return FALSE;

    return gnss_nmea_verify_checksum (sentence);
}

gchar *
gnss_nmea_normalize_sentence (const gchar *sentence)
{
    gchar *trimmed;
    gchar *star;
    gchar *result;

    if (!gnss_nmea_is_valid_sentence (sentence))
        return NULL;

    while (*sentence == ' ' || *sentence == '\t')
        sentence++;

    trimmed = g_strdup (sentence);
    g_strchomp (trimmed);

    star = strchr (trimmed, '*');
    if (star) {
        /* Already has checksum */
        result = g_strdup_printf ("%s\r\n", trimmed);
    } else {
        guint8 csum = gnss_nmea_calc_checksum (trimmed);
        result = g_strdup_printf ("%s*%02X\r\n", trimmed, csum);
    }

    g_free (trimmed);
    return result;
}

gchar **
gnss_nmea_split_and_normalize (const gchar *raw_data)
{
    GPtrArray *array;
    gchar **lines;
    guint i;

    if (!raw_data)
        return NULL;

    array = g_ptr_array_new_with_free_func (g_free);
    lines = g_strsplit_set (raw_data, "\r\n", -1);

    for (i = 0; lines && lines[i]; i++) {
        gchar *line = lines[i];
        g_strstrip (line);
        if (*line == '\0')
            continue;

        if (gnss_nmea_is_valid_sentence (line)) {
            gchar *normalized = gnss_nmea_normalize_sentence (line);
            if (normalized)
                g_ptr_array_add (array, normalized);
        }
    }

    g_strfreev (lines);
    g_ptr_array_add (array, NULL);
    return (gchar **) g_ptr_array_free (array, FALSE);
}
