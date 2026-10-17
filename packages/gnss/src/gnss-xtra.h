#pragma once
#include "gnss-common.h"

typedef struct _GnssXtra GnssXtra;
typedef void (*GnssXtraStatusCb) (GnssXtra *xtra, gboolean success, const gchar *message, gpointer data);

GnssXtra *gnss_xtra_new (const gchar *cache_dir);
void gnss_xtra_configure (GnssXtra *xtra, GArray *servers, guint32 max_file, guint32 max_part);
void gnss_xtra_set_callback (GnssXtra *xtra, GnssXtraStatusCb callback, gpointer data);
gboolean gnss_xtra_is_cache_valid (GnssXtra *xtra, guint max_age_hours);
gboolean gnss_xtra_is_busy (GnssXtra *xtra);
void gnss_xtra_update_and_inject (GnssXtra *xtra, QmiClientLoc *client, GCancellable *cancellable);
void gnss_xtra_cancel (GnssXtra *xtra);
void gnss_xtra_free (GnssXtra *xtra);
