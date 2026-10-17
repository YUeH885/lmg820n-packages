#pragma once

#include <gio/gio.h>

#define DEFAULT_GNSS_SOCKET_PATH "/run/lmg820n-gnss/nmea.sock"

typedef struct _GnssServer GnssServer;

typedef void (*GnssServerClientCountCb) (guint client_count, gpointer user_data);

GnssServer *gnss_server_new (const gchar *socket_path, GError **error);

void gnss_server_set_client_count_callback (GnssServer *server,
                                            GnssServerClientCountCb callback,
                                            gpointer user_data);

guint gnss_server_get_client_count (GnssServer *server);

void gnss_server_broadcast (GnssServer *server, const gchar *data, gsize len);

void gnss_server_broadcast_sentence (GnssServer *server, const gchar *sentence);

void gnss_server_close (GnssServer *server);

void gnss_server_free (GnssServer *server);
