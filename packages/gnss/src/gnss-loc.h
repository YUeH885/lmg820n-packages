#pragma once
#include "gnss-common.h"

typedef struct _GnssLoc GnssLoc;
#define GNSS_LOC_CLEANUP_TIMEOUT_SECONDS 1
typedef void (*GnssLocNmeaCb) (const gchar *sentence, gpointer data);
typedef void (*GnssLocReadyCb) (GnssLoc *loc, gboolean ready, gpointer data);
typedef void (*GnssLocSessionCb) (GnssLoc *loc, gboolean start, gboolean success, gpointer data);
typedef void (*GnssLocErrorCb) (GnssLoc *loc, const gchar *message, gpointer data);

GnssLoc *gnss_loc_new (void);
void gnss_loc_set_callbacks (GnssLoc *loc, GnssLocNmeaCb nmea_cb,
                             GnssLocReadyCb ready_cb, GnssLocSessionCb session_cb,
                             GnssLocErrorCb error_cb, gpointer data);
void gnss_loc_start_session (GnssLoc *loc);
void gnss_loc_stop_session (GnssLoc *loc);
void gnss_loc_pause_async (GnssLoc *loc, GAsyncReadyCallback callback, gpointer data);
void gnss_loc_resume (GnssLoc *loc);
void gnss_loc_free (GnssLoc *loc);
