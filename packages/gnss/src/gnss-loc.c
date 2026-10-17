#include "gnss-loc.h"
#include "gnss-request.h"
#include "gnss-xtra.h"
#include <sys/timex.h>

/* libqmi 1.38 omits Qualcomm's extended NMEA bits. */
#define LOC_NMEA_TYPE_PQGSA (1u << 15)
#define LOC_NMEA_TYPE_PQGSV (1u << 16)
#define REQUIRED_NMEA_TYPES (QMI_LOC_NMEA_TYPE_GGA | QMI_LOC_NMEA_TYPE_RMC | \
                             QMI_LOC_NMEA_TYPE_GSA | QMI_LOC_NMEA_TYPE_GSV | QMI_LOC_NMEA_TYPE_VTG | \
                             LOC_NMEA_TYPE_PQGSA | LOC_NMEA_TYPE_PQGSV)

/* The firmware initializes its NMEA mask asynchronously after the LOC service
 * appears and overwrites an early Set NMEA Types request, so a missing
 * readback is retried with a delay until the mask settles. */
#define NMEA_RETRY_MAX 20
#ifndef NMEA_RETRY_DELAY_MS
#define NMEA_RETRY_DELAY_MS 500
#endif

static const gchar *
satellite_system_name (guint32 system)
{
    switch (system) {
    case QMI_LOC_SYSTEM_GPS: return "gps";
    case QMI_LOC_SYSTEM_GALILEO: return "galileo";
    case QMI_LOC_SYSTEM_SBAS: return "sbas";
    case QMI_LOC_SYSTEM_COMPASS: return "compass";
    case QMI_LOC_SYSTEM_GLONASS: return "glonass";
    case 6: return "beidou";
    case 7: return "qzss";
    default: return "unknown";
    }
}

static const gchar *
satellite_status_name (guint32 status)
{
    /* libqmi 1.38's status enum is offset from the LOC wire values. */
    switch (status) {
    case 1: return "idle";
    case 2: return "searching";
    case 3: return "tracking";
    default: return "unknown";
    }
}

typedef enum {
    INIT_GET_MODE,
    INIT_SET_MODE,
    INIT_GET_NMEA,
    INIT_SET_NMEA,
    INIT_VERIFY_NMEA,
    INIT_REGISTER_EVENTS,
} InitStep;

typedef struct {
    guint refs;
    GnssLoc *owner;
    gboolean active;
    gboolean initializing;
    QrtrNode *node;
    QmiDevice *device;
    QmiClientLoc *client;
    GCancellable *cancellable;
    GnssXtra *xtra;
    InitStep step;
    guint nmea_attempts;
    QmiLocOperationMode mode;
    QmiLocNmeaType nmea_types;
    gboolean time_pending;
    gboolean time_accepted;
    gboolean time_requested;
    gboolean orbits_requested;
    gboolean start_pending;
    gboolean stop_pending;
    gboolean tracking;
    gboolean engine_on;
} LocConnection;

struct _GnssLoc {
    guint refs;
    QrtrBus *bus;
    GPtrArray *watched_nodes;
    GCancellable *discovery_cancellable;
    LocConnection *connection;
    gboolean enabled;
    gboolean ready;
    gboolean closed;
    guint closing_connections;
    GTask *pause_task;
    GError *close_error;
    GnssLocNmeaCb nmea_cb;
    GnssLocReadyCb ready_cb;
    GnssLocSessionCb session_cb;
    GnssLocErrorCb error_cb;
    gpointer user_data;
};

static void detach_connection (LocConnection *connection);
static void initialize_connection (LocConnection *connection);
static gboolean retry_init (gpointer data);
static void attach_node (GnssLoc *loc, QrtrNode *node);
static void inject_time (LocConnection *connection);
static void update_assistance (LocConnection *connection);
static gboolean connection_current (LocConnection *connection);

static void
orbits_done (GnssXtra *xtra, gboolean success, const gchar *message, gpointer data)
{
    (void)xtra; (void)success; (void)message;
    update_assistance (data);
}

static void
update_assistance (LocConnection *connection)
{
    if (!connection_current (connection) || !connection->engine_on || !connection->tracking ||
        connection->stop_pending || connection->time_pending || gnss_xtra_is_busy (connection->xtra))
        return;
    /* This firmware rejects UTC injection while XTRA parts are in flight. */
    if (connection->time_requested) {
        connection->time_requested = FALSE;
        inject_time (connection);
        if (connection->time_pending)
            return;
    }
    if (connection->orbits_requested) {
        connection->orbits_requested = FALSE;
        gnss_xtra_update_and_inject (connection->xtra, connection->client, connection->cancellable);
    }
}

static GnssLoc *
loc_ref (GnssLoc *loc)
{
    loc->refs++;
    return loc;
}

static void
loc_unref (GnssLoc *loc)
{
    if (--loc->refs)
        return;
    g_clear_object (&loc->bus);
    g_clear_object (&loc->discovery_cancellable);
    g_clear_error (&loc->close_error);
    g_ptr_array_unref (loc->watched_nodes);
    g_free (loc);
}

static LocConnection *
connection_ref (LocConnection *connection)
{
    connection->refs++;
    return connection;
}

static void
connection_unref (LocConnection *connection)
{
    if (--connection->refs)
        return;
    gnss_xtra_free (connection->xtra);
    g_clear_object (&connection->client);
    g_clear_object (&connection->device);
    g_clear_object (&connection->node);
    g_clear_object (&connection->cancellable);
    loc_unref (connection->owner);
    g_free (connection);
}

static gboolean
connection_current (LocConnection *connection)
{
    return connection->active && connection->owner->connection == connection &&
           connection->owner->enabled && !connection->owner->closed;
}

static void
complete_pause (GnssLoc *loc)
{
    if (loc->pause_task && !loc->closing_connections) {
        GTask *task = g_steal_pointer (&loc->pause_task);
        if (loc->close_error)
            g_task_return_error (task, g_steal_pointer (&loc->close_error));
        else
            g_task_return_boolean (task, TRUE);
        g_object_unref (task);
    }
}

static void
connection_closed (GObject *source, GAsyncResult *result, gpointer data)
{
    QmiDevice *device = QMI_DEVICE (source);
    LocConnection *connection = data;
    GnssLoc *loc = connection->owner;
    g_autoptr(GError) error = NULL;
    if (!qmi_device_close_finish (device, result, &error)) {
        g_warning ("[LOC] Closing QRTR endpoint failed: %s", error->message);
        if (!loc->close_error)
            loc->close_error = g_steal_pointer (&error);
    }
    loc->closing_connections--;
    complete_pause (loc);
    connection_unref (connection);
}

static void
close_device (LocConnection *connection)
{
    if (connection->device) {
        qmi_device_close_async (connection->device, GNSS_LOC_CLEANUP_TIMEOUT_SECONDS, NULL,
                               connection_closed, connection);
    } else {
        GnssLoc *loc = connection->owner;
        loc->closing_connections--;
        complete_pause (loc);
        connection_unref (connection);
    }
}

static void
client_released (GObject *source, GAsyncResult *result, gpointer data)
{
    QmiDevice *device = QMI_DEVICE (source);
    LocConnection *connection = data;
    g_autoptr(GError) error = NULL;
    if (!qmi_device_release_client_finish (device, result, &error))
        g_warning ("[LOC] Releasing LOC client failed: %s", error->message);
    close_device (connection);
}

static void
cleanup_connection (LocConnection *connection)
{
    if (connection->initializing)
        return;
    if (connection->client && connection->device) {
        qmi_device_release_client (connection->device, QMI_CLIENT (connection->client),
                                   QMI_DEVICE_RELEASE_CLIENT_FLAGS_RELEASE_CID, GNSS_LOC_CLEANUP_TIMEOUT_SECONDS, NULL,
                                   client_released, connection);
    } else {
        close_device (connection);
    }
}

static void
detach_connection (LocConnection *connection)
{
    GnssLoc *loc = connection->owner;
    if (!connection->active)
        return;
    connection->active = FALSE;
    loc->connection = NULL;
    gboolean was_ready = loc->ready;
    loc->ready = FALSE;
    g_cancellable_cancel (connection->cancellable);
    gnss_xtra_cancel (connection->xtra);
    if (connection->client)
        g_signal_handlers_disconnect_by_data (connection->client, connection);
    if (connection->device)
        g_signal_handlers_disconnect_by_data (connection->device, connection);
    loc->closing_connections++;
    connection_ref (connection);
    cleanup_connection (connection);
    if (was_ready && loc->ready_cb)
        loc->ready_cb (loc, FALSE, loc->user_data);
    connection_unref (connection);
}

static void
connection_failed (LocConnection *connection, const gchar *message)
{
    if (!connection_current (connection))
        return;
    GnssLoc *loc = connection->owner;
    g_warning ("[LOC] Initialization failed: %s", message);
    if (loc->error_cb)
        loc->error_cb (loc, message, loc->user_data);
    if (connection->active)
        detach_connection (connection);
}

static gboolean
check_status (QmiLocIndicationStatus status, GError **error)
{
    if (status == QMI_LOC_INDICATION_STATUS_SUCCESS)
        return TRUE;
    g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED, "LOC indication rejected: %s",
                 qmi_loc_indication_status_get_string (status));
    return FALSE;
}

#define RESPONSE_CALLBACK(name, Type) \
static void name##_response (GObject *source, GAsyncResult *result, gpointer data) { \
    QmiClientLoc *client = QMI_CLIENT_LOC (source); \
    g_autoptr(GError) error = NULL; \
    g_autoptr(Type) output = qmi_client_loc_##name##_finish (client, result, &error); \
    if (output) qmi_message_loc_##name##_output_get_result (output, &error); \
    gnss_request_response (data, g_steal_pointer (&error)); \
    g_object_unref (data); \
}

RESPONSE_CALLBACK (get_operation_mode, QmiMessageLocGetOperationModeOutput)
RESPONSE_CALLBACK (set_operation_mode, QmiMessageLocSetOperationModeOutput)
RESPONSE_CALLBACK (get_nmea_types, QmiMessageLocGetNmeaTypesOutput)
RESPONSE_CALLBACK (set_nmea_types, QmiMessageLocSetNmeaTypesOutput)
RESPONSE_CALLBACK (inject_utc_time, QmiMessageLocInjectUtcTimeOutput)
RESPONSE_CALLBACK (get_predicted_orbits_data_source, QmiMessageLocGetPredictedOrbitsDataSourceOutput)

static gboolean
parse_mode (gpointer output, gpointer data, GError **error)
{
    LocConnection *connection = data;
    QmiLocIndicationStatus status;
    return qmi_indication_loc_get_operation_mode_output_get_indication_status (output, &status, error) &&
           check_status (status, error) &&
           qmi_indication_loc_get_operation_mode_output_get_operation_mode (output, &connection->mode, error);
}

static gboolean
parse_nmea (gpointer output, gpointer data, GError **error)
{
    LocConnection *connection = data;
    QmiLocIndicationStatus status;
    return qmi_indication_loc_get_nmea_types_output_get_indication_status (output, &status, error) &&
           check_status (status, error) &&
           qmi_indication_loc_get_nmea_types_output_get_nmea_types (output, &connection->nmea_types, error);
}

#define STATUS_PARSER(name) \
static gboolean parse_##name (gpointer output, gpointer data, GError **error) { \
    (void)data; \
    QmiLocIndicationStatus status; \
    return qmi_indication_loc_##name##_output_get_indication_status (output, &status, error) && \
           check_status (status, error); \
}
STATUS_PARSER (set_operation_mode)
STATUS_PARSER (set_nmea_types)
STATUS_PARSER (inject_utc_time)

static gboolean
retry_init (gpointer data)
{
    LocConnection *connection = data;
    if (connection_current (connection))
        initialize_connection (connection);
    connection_unref (connection);
    return G_SOURCE_REMOVE;
}

static void
init_request_done (GObject *source, GAsyncResult *result, gpointer data)
{
    (void)source;
    LocConnection *connection = data;
    g_autoptr(GError) error = NULL;
    gboolean success = g_task_propagate_boolean (G_TASK (result), &error);
    if (connection_current (connection)) {
        if (!success) {
            connection_failed (connection, error->message);
        } else {
            switch (connection->step) {
            case INIT_GET_MODE:
                connection->step = connection->mode == QMI_LOC_OPERATION_MODE_STANDALONE ? INIT_GET_NMEA : INIT_SET_MODE;
                break;
            case INIT_SET_MODE: connection->step = INIT_GET_NMEA; break;
            case INIT_GET_NMEA:
                connection->step = (connection->nmea_types & REQUIRED_NMEA_TYPES) == REQUIRED_NMEA_TYPES ?
                                   INIT_REGISTER_EVENTS : INIT_SET_NMEA;
                break;
            case INIT_SET_NMEA: connection->step = INIT_VERIFY_NMEA; break;
            case INIT_VERIFY_NMEA:
                if ((connection->nmea_types & REQUIRED_NMEA_TYPES) == REQUIRED_NMEA_TYPES) {
                    connection->step = INIT_REGISTER_EVENTS;
                } else if (++connection->nmea_attempts < NMEA_RETRY_MAX) {
                    connection->step = INIT_SET_NMEA;
                } else {
                    g_autofree gchar *message = g_strdup_printf (
                        "NMEA mask readback 0x%x is missing required types 0x%x after %u attempts",
                        connection->nmea_types, REQUIRED_NMEA_TYPES, connection->nmea_attempts);
                    connection_failed (connection, message);
                    connection_unref (connection);
                    return;
                }
                break;
            default: g_assert_not_reached ();
            }
            if (connection->step == INIT_SET_NMEA && connection->nmea_attempts > 0)
                g_timeout_add (NMEA_RETRY_DELAY_MS, retry_init, connection_ref (connection));
            else
                initialize_connection (connection);
        }
    }
    connection_unref (connection);
}

static gboolean
parse_sources (gpointer output, gpointer data, GError **error)
{
    LocConnection *connection = data;
    QmiLocIndicationStatus status;
    GArray *servers;
    guint32 max_file = G_MAXUINT16 * 1024u;
    guint32 max_part = 1024;
    if (!qmi_indication_loc_get_predicted_orbits_data_source_output_get_indication_status (output, &status, error) ||
        !check_status (status, error) ||
        !qmi_indication_loc_get_predicted_orbits_data_source_output_get_server_list (output, &servers, error))
        return FALSE;
    g_autoptr(GError) sizes_error = NULL;
    if (!qmi_indication_loc_get_predicted_orbits_data_source_output_get_allowed_sizes (
            output, &max_file, &max_part, &sizes_error) &&
        !g_error_matches (sizes_error, QMI_CORE_ERROR, QMI_CORE_ERROR_TLV_NOT_FOUND)) {
        g_propagate_error (error, g_steal_pointer (&sizes_error));
        return FALSE;
    }
    gnss_xtra_configure (connection->xtra, servers, max_file, max_part);
    return TRUE;
}

static void
sources_done (GObject *source, GAsyncResult *result, gpointer data)
{
    (void)source;
    LocConnection *connection = data;
    g_autoptr(GError) error = NULL;
    gboolean success = g_task_propagate_boolean (G_TASK (result), &error);
    if (connection_current (connection)) {
        if (!success)
            g_warning ("[LOC] XTRA source query failed: %s", error->message);
        else if (connection->tracking) {
            connection->orbits_requested = TRUE;
            update_assistance (connection);
        }
    }
    connection_unref (connection);
}

static void
registered (GObject *source, GAsyncResult *result, gpointer data)
{
    QmiClientLoc *client = QMI_CLIENT_LOC (source);
    LocConnection *connection = data;
    GnssLoc *loc = connection->owner;
    g_autoptr(GError) error = NULL;
    g_autoptr(QmiMessageLocRegisterEventsOutput) output = qmi_client_loc_register_events_finish (client, result, &error);
    gboolean success = output && qmi_message_loc_register_events_output_get_result (output, &error);
    if (connection_current (connection)) {
        if (!success) {
            connection_failed (connection, error->message);
        } else {
            loc->ready = TRUE;
            g_message ("[LOC] Ready: standalone, NMEA mask 0x%x", connection->nmea_types);
            GTask *request = gnss_request_new (client, "get-predicted-orbits-data-source", 10000,
                                               connection->cancellable, parse_sources, connection,
                                               sources_done, connection_ref (connection));
            qmi_client_loc_get_predicted_orbits_data_source (client, NULL, 10, connection->cancellable,
                get_predicted_orbits_data_source_response, request);
            if (loc->ready_cb)
                loc->ready_cb (loc, TRUE, loc->user_data);
        }
    }
    connection_unref (connection);
}

static void
initialize_connection (LocConnection *connection)
{
    GTask *request;
    switch (connection->step) {
    case INIT_GET_MODE:
        request = gnss_request_new (connection->client, "get-operation-mode", 10000, connection->cancellable,
                                    parse_mode, connection, init_request_done, connection_ref (connection));
        qmi_client_loc_get_operation_mode (connection->client, NULL, 10, connection->cancellable,
                                           get_operation_mode_response, request);
        break;
    case INIT_SET_MODE: {
        g_autoptr(QmiMessageLocSetOperationModeInput) input = qmi_message_loc_set_operation_mode_input_new ();
        qmi_message_loc_set_operation_mode_input_set_operation_mode (input, QMI_LOC_OPERATION_MODE_STANDALONE, NULL);
        request = gnss_request_new (connection->client, "set-operation-mode", 10000, connection->cancellable,
                                    parse_set_operation_mode, NULL, init_request_done, connection_ref (connection));
        qmi_client_loc_set_operation_mode (connection->client, input, 10, connection->cancellable,
                                           set_operation_mode_response, request);
        break;
    }
    case INIT_GET_NMEA:
    case INIT_VERIFY_NMEA:
        request = gnss_request_new (connection->client, "get-nmea-types", 10000, connection->cancellable,
                                    parse_nmea, connection, init_request_done, connection_ref (connection));
        qmi_client_loc_get_nmea_types (connection->client, NULL, 10, connection->cancellable,
                                       get_nmea_types_response, request);
        break;
    case INIT_SET_NMEA: {
        g_autoptr(QmiMessageLocSetNmeaTypesInput) input = qmi_message_loc_set_nmea_types_input_new ();
        qmi_message_loc_set_nmea_types_input_set_nmea_types (input, connection->nmea_types | REQUIRED_NMEA_TYPES, NULL);
        request = gnss_request_new (connection->client, "set-nmea-types", 10000, connection->cancellable,
                                    parse_set_nmea_types, NULL, init_request_done, connection_ref (connection));
        qmi_client_loc_set_nmea_types (connection->client, input, 10, connection->cancellable,
                                       set_nmea_types_response, request);
        break;
    }
    case INIT_REGISTER_EVENTS: {
        g_autoptr(QmiMessageLocRegisterEventsInput) input = qmi_message_loc_register_events_input_new ();
        QmiLocEventRegistrationFlag mask = QMI_LOC_EVENT_REGISTRATION_FLAG_POSITION_REPORT |
            QMI_LOC_EVENT_REGISTRATION_FLAG_GNSS_SATELLITE_INFO | QMI_LOC_EVENT_REGISTRATION_FLAG_NMEA |
            QMI_LOC_EVENT_REGISTRATION_FLAG_ENGINE_STATE | QMI_LOC_EVENT_REGISTRATION_FLAG_INJECT_TIME_REQUEST |
            QMI_LOC_EVENT_REGISTRATION_FLAG_INJECT_PREDICTED_ORBITS_REQUEST;
        qmi_message_loc_register_events_input_set_event_registration_mask (input, mask, NULL);
        qmi_client_loc_register_events (connection->client, input, 10, connection->cancellable,
                                       registered, connection_ref (connection));
        break;
    }
    }
}

static void
on_nmea (QmiClientLoc *client, QmiIndicationLocNmeaOutput *output, gpointer data)
{
    (void)client;
    LocConnection *connection = data;
    const gchar *sentence;
    if (connection_current (connection) && connection->tracking && connection->owner->nmea_cb &&
        qmi_indication_loc_nmea_output_get_nmea_string (output, &sentence, NULL))
        connection->owner->nmea_cb (sentence, connection->owner->user_data);
}

static void
on_position (QmiClientLoc *client, QmiIndicationLocPositionReportOutput *output, gpointer data)
{
    (void)client;
    LocConnection *connection = data;
    QmiLocSessionStatus status;
    guint8 session;
    gdouble latitude, longitude;
    gfloat uncertainty;
    if (!connection_current (connection) ||
        !qmi_indication_loc_position_report_output_get_session_id (output, &session, NULL) ||
        session != DEFAULT_GNSS_SESSION_ID ||
        !qmi_indication_loc_position_report_output_get_session_status (output, &status, NULL))
        return;
    g_debug ("[LOC] Position status: %s", qmi_loc_session_status_get_string (status));
    if (status == QMI_LOC_SESSION_STATUS_SUCCESS &&
        qmi_indication_loc_position_report_output_get_latitude (output, &latitude, NULL) &&
        qmi_indication_loc_position_report_output_get_longitude (output, &longitude, NULL) &&
        qmi_indication_loc_position_report_output_get_horizontal_uncertainty_circular (output, &uncertainty, NULL))
        g_debug ("[LOC] Position: lat=%.8f lon=%.8f horizontal_uncertainty=%.1fm", latitude, longitude, uncertainty);
}

static void
on_satellites (QmiClientLoc *client, QmiIndicationLocGnssSvInfoOutput *output, gpointer data)
{
    (void)client;
    LocConnection *connection = data;
    GArray *list;
    if (!connection_current (connection) || !qmi_indication_loc_gnss_sv_info_output_get_list (output, &list, NULL))
        return;
    for (guint i = 0; i < list->len; i++) {
        QmiIndicationLocGnssSvInfoOutputListElement *sv = &g_array_index (list, QmiIndicationLocGnssSvInfoOutputListElement, i);
        guint required = QMI_LOC_SATELLITE_VALID_INFORMATION_SYSTEM | QMI_LOC_SATELLITE_VALID_INFORMATION_GNSS_SATELLITE_ID |
                         QMI_LOC_SATELLITE_VALID_INFORMATION_PROCESS_STATUS;
        if ((sv->valid_information & required) != required)
            continue;
        g_debug ("[LOC] SV: system=%s(%u) id=%u status=%s(%u)", satellite_system_name (sv->system),
                 (guint)sv->system, sv->gnss_satellite_id,
                 satellite_status_name (sv->satellite_status), (guint)sv->satellite_status);
    }
}

static void
on_engine (QmiClientLoc *client, QmiIndicationLocEngineStateOutput *output, gpointer data)
{
    (void)client;
    LocConnection *connection = data;
    QmiLocEngineState state;
    if (connection_current (connection) && qmi_indication_loc_engine_state_output_get_engine_state (output, &state, NULL)) {
        connection->engine_on = state == QMI_LOC_ENGINE_STATE_ON;
        g_message ("[LOC] Engine state: %s", qmi_loc_engine_state_get_string (state));
        if (connection->engine_on && !connection->time_accepted && !connection->time_pending)
            connection->time_requested = TRUE;
        update_assistance (connection);
    }
}

static void
on_time_request (QmiClientLoc *client, QmiIndicationLocInjectTimeRequestOutput *output, gpointer data)
{
    (void)client; (void)output;
    LocConnection *connection = data;
    if (connection_current (connection) && connection->tracking && !connection->time_pending) {
        connection->time_requested = TRUE;
        update_assistance (connection);
    }
}

static void
on_orbits_request (QmiClientLoc *client, QmiIndicationLocInjectPredictedOrbitsRequestOutput *output, gpointer data)
{
    (void)output;
    LocConnection *connection = data;
    (void)client;
    if (connection_current (connection) && connection->tracking) {
        connection->orbits_requested = TRUE;
        update_assistance (connection);
    }
}

static void
allocated (GObject *source, GAsyncResult *result, gpointer data)
{
    QmiDevice *device = QMI_DEVICE (source);
    LocConnection *connection = data;
    g_autoptr(GError) error = NULL;
    g_autoptr(QmiClient) client = qmi_device_allocate_client_finish (device, result, &error);
    connection->initializing = FALSE;
    if (client)
        connection->client = QMI_CLIENT_LOC (g_steal_pointer (&client));
    if (connection_current (connection)) {
        if (!connection->client) {
            connection_failed (connection, error->message);
        } else {
            g_signal_connect (connection->client, "nmea", G_CALLBACK (on_nmea), connection);
            g_signal_connect (connection->client, "position-report", G_CALLBACK (on_position), connection);
            g_signal_connect (connection->client, "gnss-sv-info", G_CALLBACK (on_satellites), connection);
            g_signal_connect (connection->client, "engine-state", G_CALLBACK (on_engine), connection);
            g_signal_connect (connection->client, "inject-time-request", G_CALLBACK (on_time_request), connection);
            g_signal_connect (connection->client, "inject-predicted-orbits-request", G_CALLBACK (on_orbits_request), connection);
            initialize_connection (connection);
        }
    } else {
        cleanup_connection (connection);
    }
    connection_unref (connection);
}

static void
opened (GObject *source, GAsyncResult *result, gpointer data)
{
    QmiDevice *device = QMI_DEVICE (source);
    LocConnection *connection = data;
    g_autoptr(GError) error = NULL;
    gboolean success = qmi_device_open_finish (device, result, &error);
    if (connection_current (connection)) {
        if (!success) {
            connection->initializing = FALSE;
            connection_failed (connection, error->message);
        } else
            qmi_device_allocate_client (device, QMI_SERVICE_LOC, QMI_CID_NONE, 10, connection->cancellable,
                                       allocated, connection_ref (connection));
    } else {
        connection->initializing = FALSE;
        cleanup_connection (connection);
    }
    connection_unref (connection);
}

static void
on_device_removed (QmiDevice *device, gpointer data)
{
    (void)device;
    LocConnection *connection = data;
    if (connection_current (connection))
        detach_connection (connection);
}

static void
created (GObject *source, GAsyncResult *result, gpointer data)
{
    (void)source;
    LocConnection *connection = data;
    g_autoptr(GError) error = NULL;
    g_autoptr(QmiDevice) device = qmi_device_new_from_node_finish (result, &error);
    if (device)
        connection->device = g_steal_pointer (&device);
    if (connection_current (connection)) {
        if (!connection->device) {
            connection->initializing = FALSE;
            connection_failed (connection, error->message);
        } else {
            g_signal_connect (connection->device, "device-removed", G_CALLBACK (on_device_removed), connection);
            qmi_device_open (connection->device, QMI_DEVICE_OPEN_FLAGS_EXPECT_INDICATIONS, 10,
                             connection->cancellable, opened, connection_ref (connection));
        }
    } else {
        connection->initializing = FALSE;
        cleanup_connection (connection);
    }
    connection_unref (connection);
}

static void
attach_node (GnssLoc *loc, QrtrNode *node)
{
    if (!loc->enabled || loc->closed || loc->connection || qrtr_node_lookup_port (node, QMI_SERVICE_LOC) < 0)
        return;
    LocConnection *connection = g_new0 (LocConnection, 1);
    connection->refs = 1;
    connection->owner = loc_ref (loc);
    connection->active = TRUE;
    connection->initializing = TRUE;
    connection->node = g_object_ref (node);
    connection->cancellable = g_cancellable_new ();
    connection->xtra = gnss_xtra_new (NULL);
    gnss_xtra_set_callback (connection->xtra, orbits_done, connection);
    loc->connection = connection;
    g_message ("[LOC] Attaching QRTR node %u", qrtr_node_get_id (node));
    qmi_device_new_from_node (node, connection->cancellable, created, connection_ref (connection));
}

static void
service_added (QrtrNode *node, guint service, gpointer data)
{
    if (service == QMI_SERVICE_LOC)
        attach_node (data, node);
}

static void
service_removed (QrtrNode *node, guint service, gpointer data)
{
    GnssLoc *loc = data;
    if (service == QMI_SERVICE_LOC && loc->connection && loc->connection->node == node)
        detach_connection (loc->connection);
}

static void
watch_node (GnssLoc *loc, QrtrNode *node)
{
    g_ptr_array_add (loc->watched_nodes, g_object_ref (node));
    g_signal_connect (node, QRTR_NODE_SIGNAL_SERVICE_ADDED, G_CALLBACK (service_added), loc);
    g_signal_connect (node, QRTR_NODE_SIGNAL_SERVICE_REMOVED, G_CALLBACK (service_removed), loc);
    attach_node (loc, node);
}

static void
node_added (QrtrBus *bus, guint node_id, gpointer data)
{
    g_autoptr(QrtrNode) node = qrtr_bus_get_node (bus, node_id);
    if (node)
        watch_node (data, node);
}

static void
node_removed (QrtrBus *bus, guint node_id, gpointer data)
{
    (void)bus;
    GnssLoc *loc = data;
    if (loc->connection && qrtr_node_get_id (loc->connection->node) == node_id)
        detach_connection (loc->connection);
    for (guint i = 0; i < loc->watched_nodes->len; i++) {
        QrtrNode *node = g_ptr_array_index (loc->watched_nodes, i);
        if (qrtr_node_get_id (node) == node_id) {
            g_signal_handlers_disconnect_by_data (node, loc);
            g_ptr_array_remove_index (loc->watched_nodes, i);
            break;
        }
    }
}

static void
bus_created (GObject *source, GAsyncResult *result, gpointer data)
{
    (void)source;
    GnssLoc *loc = data;
    g_autoptr(GError) error = NULL;
    g_autoptr(QrtrBus) bus = qrtr_bus_new_finish (result, &error);
    if (!loc->closed) {
        if (!bus) {
            if (loc->error_cb)
                loc->error_cb (loc, error->message, loc->user_data);
        } else {
            loc->bus = g_steal_pointer (&bus);
            g_signal_connect (loc->bus, QRTR_BUS_SIGNAL_NODE_ADDED, G_CALLBACK (node_added), loc);
            g_signal_connect (loc->bus, QRTR_BUS_SIGNAL_NODE_REMOVED, G_CALLBACK (node_removed), loc);
            GList *nodes = qrtr_bus_get_nodes (loc->bus);
            for (GList *l = nodes; l; l = l->next)
                watch_node (loc, l->data);
            g_list_free_full (nodes, g_object_unref);
        }
    }
    loc_unref (loc);
}

static void
time_done (GObject *source, GAsyncResult *result, gpointer data)
{
    (void)source;
    LocConnection *connection = data;
    g_autoptr(GError) error = NULL;
    gboolean success = g_task_propagate_boolean (G_TASK (result), &error);
    connection->time_pending = FALSE;
    if (success)
        connection->time_accepted = TRUE;
    if (connection_current (connection)) {
        if (success)
            g_message ("[LOC] UTC assistance accepted");
        else
            g_warning ("[LOC] UTC assistance failed: %s", error->message);
        update_assistance (connection);
    }
    connection_unref (connection);
}

static void
inject_time (LocConnection *connection)
{
    struct timex clock = {0};
    /* Start acknowledgement precedes engine readiness on this firmware. */
    if (!connection->engine_on || !connection->tracking || connection->stop_pending ||
        connection->time_pending || gnss_xtra_is_busy (connection->xtra))
        return;
    gint state = adjtimex (&clock);
    if (state < 0 || state == TIME_ERROR || (clock.status & STA_UNSYNC)) {
        g_debug ("[LOC] UTC assistance deferred: system clock is unsynchronized");
        return;
    }
    guint64 uncertainty = ((guint64)MAX (clock.maxerror, clock.esterror) + 999) / 1000 + 1;
    if (uncertainty > G_MAXUINT32) {
        g_warning ("[LOC] UTC uncertainty exceeds protocol range");
        return;
    }
    connection->time_pending = TRUE;
    g_autoptr(QmiMessageLocInjectUtcTimeInput) input = qmi_message_loc_inject_utc_time_input_new ();
    qmi_message_loc_inject_utc_time_input_set_utc_time (input, g_get_real_time () / 1000, NULL);
    qmi_message_loc_inject_utc_time_input_set_time_uncertainty (input, uncertainty, NULL);
    qmi_message_loc_inject_utc_time_input_set_time_source (input, QMI_LOC_INJECTED_TIME_SOURCE_AP, NULL);
    GTask *request = gnss_request_new (connection->client, "inject-utc-time", 10000, connection->cancellable,
                                       parse_inject_utc_time, NULL, time_done, connection_ref (connection));
    qmi_client_loc_inject_utc_time (connection->client, input, 10, connection->cancellable,
                                   inject_utc_time_response, request);
}

static void
session_started (GObject *source, GAsyncResult *result, gpointer data)
{
    QmiClientLoc *client = QMI_CLIENT_LOC (source);
    LocConnection *connection = data;
    g_autoptr(GError) error = NULL;
    g_autoptr(QmiMessageLocStartOutput) output = qmi_client_loc_start_finish (client, result, &error);
    gboolean success = output && qmi_message_loc_start_output_get_result (output, &error);
    if (connection_current (connection)) {
        connection->start_pending = FALSE;
        connection->tracking = success;
        if (!success)
            g_warning ("[LOC] Start failed: %s", error->message);
        if (connection->owner->session_cb)
            connection->owner->session_cb (connection->owner, TRUE, success, connection->owner->user_data);
        if (connection_current (connection) && success && !connection->stop_pending) {
            if (!connection->time_accepted && !connection->time_pending)
                connection->time_requested = TRUE;
            connection->orbits_requested = TRUE;
            update_assistance (connection);
        }
    }
    connection_unref (connection);
}

static void
session_stopped (GObject *source, GAsyncResult *result, gpointer data)
{
    QmiClientLoc *client = QMI_CLIENT_LOC (source);
    LocConnection *connection = data;
    g_autoptr(GError) error = NULL;
    g_autoptr(QmiMessageLocStopOutput) output = qmi_client_loc_stop_finish (client, result, &error);
    gboolean success = output && qmi_message_loc_stop_output_get_result (output, &error);
    if (connection_current (connection)) {
        connection->stop_pending = FALSE;
        if (success)
            connection->tracking = FALSE;
        else
            g_warning ("[LOC] Stop unconfirmed: %s", error->message);
        if (connection->owner->session_cb)
            connection->owner->session_cb (connection->owner, FALSE, success, connection->owner->user_data);
    }
    connection_unref (connection);
}

GnssLoc *
gnss_loc_new (void)
{
    GnssLoc *loc = g_new0 (GnssLoc, 1);
    loc->refs = 1;
    loc->enabled = TRUE;
    loc->watched_nodes = g_ptr_array_new_with_free_func (g_object_unref);
    loc->discovery_cancellable = g_cancellable_new ();
    qrtr_bus_new (1000, loc->discovery_cancellable, bus_created, loc_ref (loc));
    return loc;
}

void
gnss_loc_set_callbacks (GnssLoc *loc, GnssLocNmeaCb nmea_cb, GnssLocReadyCb ready_cb,
                        GnssLocSessionCb session_cb, GnssLocErrorCb error_cb, gpointer data)
{
    loc->nmea_cb = nmea_cb;
    loc->ready_cb = ready_cb;
    loc->session_cb = session_cb;
    loc->error_cb = error_cb;
    loc->user_data = data;
}

void
gnss_loc_start_session (GnssLoc *loc)
{
    LocConnection *connection = loc->connection;
    g_return_if_fail (loc->ready && connection && !connection->tracking &&
                      !connection->start_pending && !connection->stop_pending);
    connection->start_pending = TRUE;
    g_autoptr(QmiMessageLocStartInput) input = qmi_message_loc_start_input_new ();
    qmi_message_loc_start_input_set_session_id (input, DEFAULT_GNSS_SESSION_ID, NULL);
    qmi_message_loc_start_input_set_fix_recurrence_type (input, QMI_LOC_FIX_RECURRENCE_TYPE_REQUEST_PERIODIC_FIXES, NULL);
    qmi_message_loc_start_input_set_minimum_interval_between_position_reports (input, DEFAULT_GNSS_INTERVAL_MS, NULL);
    qmi_message_loc_start_input_set_intermediate_report_state (input, QMI_LOC_INTERMEDIATE_REPORT_STATE_DISABLE, NULL);
    g_message ("[LOC] Start session %u, interval %u ms", DEFAULT_GNSS_SESSION_ID, DEFAULT_GNSS_INTERVAL_MS);
    qmi_client_loc_start (connection->client, input, 10, connection->cancellable,
                          session_started, connection_ref (connection));
}

void
gnss_loc_stop_session (GnssLoc *loc)
{
    LocConnection *connection = loc->connection;
    g_return_if_fail (loc->ready && connection && connection->tracking &&
                      !connection->start_pending && !connection->stop_pending);
    connection->stop_pending = TRUE;
    /* A later Start must observe readiness for its own engine activation. */
    connection->engine_on = FALSE;
    g_autoptr(QmiMessageLocStopInput) input = qmi_message_loc_stop_input_new ();
    qmi_message_loc_stop_input_set_session_id (input, DEFAULT_GNSS_SESSION_ID, NULL);
    g_message ("[LOC] Stop session %u", DEFAULT_GNSS_SESSION_ID);
    qmi_client_loc_stop (connection->client, input, 10, connection->cancellable,
                         session_stopped, connection_ref (connection));
}

void
gnss_loc_pause_async (GnssLoc *loc, GAsyncReadyCallback callback, gpointer data)
{
    g_return_if_fail (!loc->pause_task);
    loc->enabled = FALSE;
    loc->pause_task = g_task_new (NULL, NULL, callback, data);
    if (loc->connection)
        detach_connection (loc->connection);
    complete_pause (loc);
}

void
gnss_loc_resume (GnssLoc *loc)
{
    g_return_if_fail (!loc->closed && !loc->pause_task);
    loc->enabled = TRUE;
    for (guint i = 0; i < loc->watched_nodes->len; i++)
        attach_node (loc, g_ptr_array_index (loc->watched_nodes, i));
}

void
gnss_loc_free (GnssLoc *loc)
{
    loc->closed = TRUE;
    loc->enabled = FALSE;
    loc->nmea_cb = NULL;
    loc->ready_cb = NULL;
    loc->session_cb = NULL;
    loc->error_cb = NULL;
    g_cancellable_cancel (loc->discovery_cancellable);
    if (loc->connection)
        detach_connection (loc->connection);
    if (loc->bus)
        g_signal_handlers_disconnect_by_data (loc->bus, loc);
    for (guint i = 0; i < loc->watched_nodes->len; i++)
        g_signal_handlers_disconnect_by_data (g_ptr_array_index (loc->watched_nodes, i), loc);
    loc_unref (loc);
}
