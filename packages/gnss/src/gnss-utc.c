#include "gnss-utc.h"
#include "gnss-nmea.h"

#include <errno.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

struct _GnssUtc {
    gint fd;
    struct sockaddr_un address;
    gint64 last_utc_ns;
};

GnssUtc *
gnss_utc_new (const gchar *socket_path, GError **error)
{
    if (!socket_path || !g_path_is_absolute (socket_path) ||
        strlen (socket_path) >= sizeof (((struct sockaddr_un *)0)->sun_path)) {
        g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                             "UTC socket path must be absolute and fit sockaddr_un");
        return NULL;
    }
    gint fd = socket (AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (fd < 0) {
        g_set_error (error, G_IO_ERROR, g_io_error_from_errno (errno),
                     "Could not create UTC socket: %s", g_strerror (errno));
        return NULL;
    }
    GnssUtc *utc = g_new0 (GnssUtc, 1);
    utc->fd = fd;
    utc->address.sun_family = AF_UNIX;
    g_strlcpy (utc->address.sun_path, socket_path, sizeof utc->address.sun_path);
    g_message ("[UTC] RMC second labels configured for %s", socket_path);
    return utc;
}

void
gnss_utc_free (GnssUtc *utc)
{
    if (!utc)
        return;
    close (utc->fd);
    g_free (utc);
}

static gboolean
decimal_digits (const gchar *text, gsize length)
{
    for (gsize i = 0; i < length; i++)
        if (!g_ascii_isdigit (text[i]))
            return FALSE;
    return TRUE;
}

static gint
decimal_pair (const gchar *text)
{
    return (text[0] - '0') * 10 + text[1] - '0';
}

gboolean
gnss_utc_parse_rmc (const gchar *sentence, gint64 *utc_ns)
{
    if (!sentence || !utc_ns || !gnss_nmea_is_valid_sentence (sentence))
        return FALSE;
    const gchar *star = strchr (sentence, '*');
    if (!star || strspn (star + 3, "\r\n") != strlen (star + 3))
        return FALSE;
    g_autofree gchar *body = g_strndup (sentence, star - sentence);
    g_auto(GStrv) fields = g_strsplit (body, ",", -1);
    guint count = g_strv_length (fields);
    if (count < 10 || strlen (fields[0]) != 6 || fields[0][0] != '$' ||
        !g_ascii_isupper (fields[0][1]) || !g_ascii_isupper (fields[0][2]) ||
        strcmp (fields[0] + 3, "RMC") || strcmp (fields[2], "A"))
        return FALSE;
    if (count > 12 && *fields[12] &&
        (strlen (fields[12]) != 1 || !strchr ("ADFRP", fields[12][0])))
        return FALSE;

    gsize length = strlen (fields[1]);
    if (length < 6 || !decimal_digits (fields[1], 6) ||
        (length > 6 && (fields[1][6] != '.' || length < 8 || length > 16 ||
                        !decimal_digits (fields[1] + 7, length - 7))) ||
        strlen (fields[9]) != 6 || !decimal_digits (fields[9], 6))
        return FALSE;
    gint hour = decimal_pair (fields[1]);
    gint minute = decimal_pair (fields[1] + 2);
    gint second = decimal_pair (fields[1] + 4);
    gint day = decimal_pair (fields[9]);
    gint month = decimal_pair (fields[9] + 2);
    gint year = 2000 + decimal_pair (fields[9] + 4);
    if (hour > 23 || minute > 59 || second > 59)
        return FALSE;
    g_autoptr(GDateTime) date = g_date_time_new_utc (year, month, day, hour, minute, second);
    if (!date)
        return FALSE;
    gint64 fraction = 0;
    for (gsize i = 7; i < length; i++)
        fraction = fraction * 10 + fields[1][i] - '0';
    for (gsize i = length > 6 ? length - 7 : 0; i < 9; i++)
        fraction *= 10;
    *utc_ns = g_date_time_to_unix (date) * 1000000000 + fraction;
    return TRUE;
}

gboolean
gnss_utc_process_nmea (GnssUtc *utc, const gchar *sentences,
                       const struct timespec *received)
{
    if (!utc || !sentences || !received || received->tv_sec < 0 ||
        received->tv_nsec < 0 || received->tv_nsec >= 1000000000)
        return FALSE;
    g_auto(GStrv) lines = g_strsplit_set (sentences, "\r\n", -1);
    gboolean delivered = FALSE;
    for (guint i = 0; lines[i]; i++) {
        gint64 utc_ns;
        if (!gnss_utc_parse_rmc (lines[i], &utc_ns) || utc_ns <= utc->last_utc_ns)
            continue;
        struct chrony_sock_sample sample = {
            .tv = { .tv_sec = received->tv_sec, .tv_usec = received->tv_nsec / 1000 },
            .magic = 0x534f434b,
        };
        sample.offset = (gdouble)(utc_ns / 1000000000 - sample.tv.tv_sec) +
            (gdouble)(utc_ns % 1000000000 - sample.tv.tv_usec * 1000) * 1e-9;
        ssize_t size = sendto (utc->fd, &sample, sizeof sample, MSG_DONTWAIT,
                               (struct sockaddr *)&utc->address, sizeof utc->address);
        if (size < 0) {
            g_debug ("[UTC] Second label delivery failed: %s", g_strerror (errno));
            continue;
        }
        utc->last_utc_ns = utc_ns;
        delivered = TRUE;
        g_debug ("[UTC] Second label delivered: UTC=%" G_GINT64_FORMAT " ns", utc_ns);
    }
    return delivered;
}
