#pragma once

#include "gnss-common.h"

typedef gboolean (*GnssIndicationParser) (gpointer output, gpointer data, GError **error);

GTask *gnss_request_new (QmiClientLoc *client,
                        const gchar *signal,
                        guint timeout_ms,
                        GCancellable *cancellable,
                        GnssIndicationParser parser,
                        gpointer parser_data,
                        GAsyncReadyCallback callback,
                        gpointer user_data);
void gnss_request_response (GTask *task, GError *error);
