#pragma once

#include <gio/gio.h>
#include <sys/time.h>
#include <time.h>

#define DEFAULT_UTC_SOCKET_PATH "/run/chrony.gnss.sock"

/* Chrony SOCK protocol uses the host ABI and native byte order. */
struct chrony_sock_sample {
    struct timeval tv;
    double offset;
    int pulse;
    int leap;
    int _pad;
    int magic;
};

typedef struct _GnssUtc GnssUtc;

GnssUtc *gnss_utc_new (const gchar *socket_path, GError **error);
void gnss_utc_free (GnssUtc *utc);
gboolean gnss_utc_parse_rmc (const gchar *sentence, gint64 *utc_ns);
gboolean gnss_utc_process_nmea (GnssUtc *utc, const gchar *sentences,
                               const struct timespec *received);
