#pragma once

#include <glib.h>
#include <gio/gio.h>
#include <gio/gunixsocketaddress.h>
#include <libqmi-glib.h>
#include <libqrtr-glib.h>

#define DEFAULT_GNSS_SESSION_ID 1
#define DEFAULT_GNSS_INTERVAL_MS 1000
#define MAX_CLIENT_QUEUE_BYTES (64 * 1024)
