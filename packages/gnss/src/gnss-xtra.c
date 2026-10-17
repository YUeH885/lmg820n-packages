#include "gnss-xtra.h"
#include "gnss-request.h"
#include <glib/gstdio.h>
#include <unistd.h>

#define DEFAULT_CACHE_DIR "/var/lib/lmg820n-gnss"
#define XTRA_FILENAME "xtra3grcej.bin"
#define XTRA_CHUNK_SIZE 1024
#define XTRA_PROTOCOL_MAX_FILE (G_MAXUINT16 * XTRA_CHUNK_SIZE)

typedef struct _XtraOperation XtraOperation;

struct _GnssXtra {
    guint refs;
    gchar *cache_dir;
    gchar *cache_path;
    gchar **servers;
    guint32 max_file;
    guint16 max_part;
    XtraOperation *operation;
    GnssXtraStatusCb callback;
    gpointer user_data;
};

struct _XtraOperation {
    guint refs;
    GnssXtra *owner;
    gboolean active;
    QmiClientLoc *client;
    GCancellable *cancellable;
    GCancellable *parent;
    gulong parent_cancelled_id;
    GSubprocess *process;
    gchar *temp_path;
    guint url_index;
    GBytes *data;
    gsize offset;
    guint16 chunk_size;
    guint16 total_parts;
    guint16 part_number;
    guint64 validity_start;
    guint16 validity_hours;
};

static void inject_part (XtraOperation *operation);
static void download_file (XtraOperation *operation);
static void inject_file (XtraOperation *operation);

static GnssXtra *
xtra_ref (GnssXtra *xtra)
{
    xtra->refs++;
    return xtra;
}

static void
xtra_unref (GnssXtra *xtra)
{
    if (--xtra->refs)
        return;
    g_strfreev (xtra->servers);
    g_free (xtra->cache_dir);
    g_free (xtra->cache_path);
    g_free (xtra);
}

static XtraOperation *
operation_ref (XtraOperation *operation)
{
    operation->refs++;
    return operation;
}

static void
operation_unref (XtraOperation *operation)
{
    if (--operation->refs)
        return;
    if (operation->parent_cancelled_id)
        g_signal_handler_disconnect (operation->parent, operation->parent_cancelled_id);
    if (operation->temp_path)
        g_unlink (operation->temp_path);
    g_free (operation->temp_path);
    g_clear_object (&operation->process);
    g_clear_object (&operation->parent);
    g_clear_object (&operation->cancellable);
    g_clear_object (&operation->client);
    g_clear_pointer (&operation->data, g_bytes_unref);
    xtra_unref (operation->owner);
    g_free (operation);
}

static void
finish_operation (XtraOperation *operation, gboolean success, const gchar *message)
{
    if (!operation->active)
        return;
    GnssXtra *xtra = operation->owner;
    operation->active = FALSE;
    xtra->operation = NULL;
    if (!success && g_cancellable_is_cancelled (operation->cancellable))
        g_debug ("[XTRA] Assistance cancelled: %s", message);
    else if (!success)
        g_warning ("[XTRA] Assistance failed: %s", message);
    if (xtra->callback)
        xtra->callback (xtra, success, message, xtra->user_data);
    operation_unref (operation);
}

static void
parent_cancelled (GCancellable *parent, gpointer data)
{
    (void)parent;
    XtraOperation *operation = data;
    g_cancellable_cancel (operation->cancellable);
    if (operation->process)
        g_subprocess_force_exit (operation->process);
}

GnssXtra *
gnss_xtra_new (const gchar *cache_dir)
{
    GnssXtra *xtra = g_new0 (GnssXtra, 1);
    xtra->refs = 1;
    xtra->cache_dir = g_strdup (cache_dir ? cache_dir : DEFAULT_CACHE_DIR);
    xtra->cache_path = g_build_filename (xtra->cache_dir, XTRA_FILENAME, NULL);
    xtra->max_file = XTRA_PROTOCOL_MAX_FILE;
    xtra->max_part = XTRA_CHUNK_SIZE;
    return xtra;
}

void
gnss_xtra_configure (GnssXtra *xtra, GArray *servers, guint32 max_file, guint32 max_part)
{
    g_strfreev (xtra->servers);
    xtra->servers = g_new0 (gchar *, servers->len + 1);
    for (guint i = 0; i < servers->len; i++)
        xtra->servers[i] = g_strdup (g_array_index (servers, gchar *, i));
    xtra->max_file = MIN (max_file, XTRA_PROTOCOL_MAX_FILE);
    xtra->max_part = MIN (max_part, XTRA_CHUNK_SIZE);
}

void
gnss_xtra_set_callback (GnssXtra *xtra, GnssXtraStatusCb callback, gpointer data)
{
    xtra->callback = callback;
    xtra->user_data = data;
}

gboolean
gnss_xtra_is_cache_valid (GnssXtra *xtra, guint max_age_hours)
{
    GStatBuf info;
    if (g_stat (xtra->cache_path, &info) || info.st_size < 1024 || info.st_size > xtra->max_file)
        return FALSE;
    gint64 age = g_get_real_time () / G_USEC_PER_SEC - info.st_mtime;
    return age >= 0 && age < (gint64)max_age_hours * 3600;
}

static gboolean
check_status (QmiLocIndicationStatus status, GError **error)
{
    if (status == QMI_LOC_INDICATION_STATUS_SUCCESS)
        return TRUE;
    g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED, "XTRA indication rejected: %s",
                 qmi_loc_indication_status_get_string (status));
    return FALSE;
}

static gboolean
parse_part (gpointer output, gpointer data, GError **error)
{
    XtraOperation *operation = data;
    QmiLocIndicationStatus status;
    guint16 part;
    if (!qmi_indication_loc_inject_xtra_data_output_get_indication_status (output, &status, error) ||
        !check_status (status, error) ||
        !qmi_indication_loc_inject_xtra_data_output_get_part_number (output, &part, error))
        return FALSE;
    if (part != operation->part_number) {
        g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "XTRA part mismatch: expected %u, received %u",
                     operation->part_number, part);
        return FALSE;
    }
    return TRUE;
}

static gboolean
parse_validity (gpointer output, gpointer data, GError **error)
{
    XtraOperation *operation = data;
    QmiLocIndicationStatus status;
    return qmi_indication_loc_get_predicted_orbits_data_validity_output_get_indication_status (output, &status, error) &&
           check_status (status, error) &&
           qmi_indication_loc_get_predicted_orbits_data_validity_output_get_validity_info (
               output, &operation->validity_start, &operation->validity_hours, error);
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
RESPONSE_CALLBACK (inject_xtra_data, QmiMessageLocInjectXtraDataOutput)
RESPONSE_CALLBACK (get_predicted_orbits_data_validity, QmiMessageLocGetPredictedOrbitsDataValidityOutput)

static void
validity_done (GObject *source, GAsyncResult *result, gpointer data)
{
    (void)source;
    XtraOperation *operation = data;
    g_autoptr(GError) error = NULL;
    gboolean success = g_task_propagate_boolean (G_TASK (result), &error);
    if (success) {
        g_autoptr(GDateTime) start = g_date_time_new_from_unix_utc (operation->validity_start);
        g_autofree gchar *date = start ? g_date_time_format_iso8601 (start) : NULL;
        g_message ("[XTRA] Injected %u parts; validity start=%s duration=%u hours", operation->total_parts,
                   date ? date : "invalid", operation->validity_hours);
    }
    finish_operation (operation, success, success ? "XTRA accepted; validity read back" : error->message);
    operation_unref (operation);
}

static void
part_done (GObject *source, GAsyncResult *result, gpointer data)
{
    (void)source;
    XtraOperation *operation = data;
    g_autoptr(GError) error = NULL;
    if (!g_task_propagate_boolean (G_TASK (result), &error))
        finish_operation (operation, FALSE, error->message);
    else
        inject_part (operation);
    operation_unref (operation);
}

static void
inject_part (XtraOperation *operation)
{
    if (g_cancellable_is_cancelled (operation->cancellable)) {
        finish_operation (operation, FALSE, "XTRA operation cancelled");
        return;
    }
    gsize size;
    const guint8 *data = g_bytes_get_data (operation->data, &size);
    if (operation->offset == size) {
        GTask *request = gnss_request_new (operation->client, "get-predicted-orbits-data-validity", 10000,
            operation->cancellable, parse_validity, operation, validity_done, operation_ref (operation));
        qmi_client_loc_get_predicted_orbits_data_validity (operation->client, NULL, 10, operation->cancellable,
            get_predicted_orbits_data_validity_response, request);
        return;
    }
    gsize count = MIN (size - operation->offset, operation->chunk_size);
    operation->part_number++;
    g_autoptr(QmiMessageLocInjectXtraDataInput) input = qmi_message_loc_inject_xtra_data_input_new ();
    g_autoptr(GArray) bytes = g_array_sized_new (FALSE, FALSE, 1, count);
    g_array_append_vals (bytes, data + operation->offset, count);
    qmi_message_loc_inject_xtra_data_input_set_total_size (input, size, NULL);
    qmi_message_loc_inject_xtra_data_input_set_total_parts (input, operation->total_parts, NULL);
    qmi_message_loc_inject_xtra_data_input_set_part_number (input, operation->part_number, NULL);
    qmi_message_loc_inject_xtra_data_input_set_part_data (input, bytes, NULL);
    operation->offset += count;
    GTask *request = gnss_request_new (operation->client, "inject-xtra-data", 10000,
        operation->cancellable, parse_part, operation, part_done, operation_ref (operation));
    qmi_client_loc_inject_xtra_data (operation->client, input, 10, operation->cancellable,
                                    inject_xtra_data_response, request);
}

static void
inject_file (XtraOperation *operation)
{
    g_autoptr(GError) error = NULL;
    gchar *bytes;
    gsize size;
    if (!g_file_get_contents (operation->owner->cache_path, &bytes, &size, &error)) {
        finish_operation (operation, FALSE, error->message);
        return;
    }
    operation->data = g_bytes_new_take (bytes, size);
    operation->chunk_size = operation->owner->max_part;
    if (!operation->chunk_size || size < 1024 || size > operation->owner->max_file ||
        (size + operation->chunk_size - 1) / operation->chunk_size > G_MAXUINT16) {
        finish_operation (operation, FALSE, "XTRA file exceeds firmware/protocol limits");
        return;
    }
    operation->total_parts = (size + operation->chunk_size - 1) / operation->chunk_size;
    g_message ("[XTRA] Injecting %zu bytes in %u parts", size, operation->total_parts);
    inject_part (operation);
}

static void
download_done (GObject *source, GAsyncResult *result, gpointer data)
{
    XtraOperation *operation = data;
    g_autoptr(GError) error = NULL;
    gboolean success = g_subprocess_wait_check_finish (G_SUBPROCESS (source), result, &error);
    g_clear_object (&operation->process);
    if (g_cancellable_is_cancelled (operation->cancellable)) {
        finish_operation (operation, FALSE, "XTRA download cancelled");
    } else if (success) {
        GStatBuf info;
        if (g_stat (operation->temp_path, &info) || info.st_size < 1024 || info.st_size > operation->owner->max_file) {
            finish_operation (operation, FALSE, "Downloaded XTRA size is outside firmware limits");
        } else if (g_rename (operation->temp_path, operation->owner->cache_path)) {
            finish_operation (operation, FALSE, g_strerror (errno));
        } else {
            g_clear_pointer (&operation->temp_path, g_free);
            inject_file (operation);
        }
    } else {
        g_warning ("[XTRA] Download failed: %s", error->message);
        g_unlink (operation->temp_path);
        g_clear_pointer (&operation->temp_path, g_free);
        operation->url_index++;
        download_file (operation);
    }
    operation_unref (operation);
}

static void
download_file (XtraOperation *operation)
{
    GnssXtra *xtra = operation->owner;
    g_autoptr(GError) error = NULL;
    if (!xtra->servers || !xtra->servers[operation->url_index]) {
        finish_operation (operation, FALSE, "No remaining firmware-reported XTRA servers");
        return;
    }
    if (g_mkdir_with_parents (xtra->cache_dir, 0750)) {
        finish_operation (operation, FALSE, g_strerror (errno));
        return;
    }
    operation->temp_path = g_build_filename (xtra->cache_dir, ".xtra-download-XXXXXX", NULL);
    gint fd = g_mkstemp (operation->temp_path);
    if (fd < 0) {
        finish_operation (operation, FALSE, g_strerror (errno));
        return;
    }
    close (fd);
    g_autofree gchar *max_size = g_strdup_printf ("%u", xtra->max_file);
    operation->process = g_subprocess_new (G_SUBPROCESS_FLAGS_NONE, &error, "/usr/bin/curl",
        "--silent", "--show-error", "--fail", "--proto", "=https",
        "--user-agent", "lmg820n-gnssd/" DAEMON_VERSION " (Linux; aarch64; LG-G820N)",
        "--connect-timeout", "10", "--max-time", "30", "--max-filesize", max_size,
        "--url", xtra->servers[operation->url_index], "--output", operation->temp_path, NULL);
    if (!operation->process) {
        finish_operation (operation, FALSE, error->message);
        return;
    }
    g_subprocess_wait_check_async (operation->process, operation->cancellable, download_done, operation_ref (operation));
}

gboolean
gnss_xtra_is_busy (GnssXtra *xtra)
{
    return xtra->operation != NULL;
}

void
gnss_xtra_update_and_inject (GnssXtra *xtra, QmiClientLoc *client, GCancellable *cancellable)
{
    if (xtra->operation)
        return;
    if (cancellable && g_cancellable_is_cancelled (cancellable))
        return;
    if (!xtra->servers)
        return;
    XtraOperation *operation = g_new0 (XtraOperation, 1);
    operation->refs = 1;
    operation->owner = xtra_ref (xtra);
    operation->active = TRUE;
    operation->client = g_object_ref (client);
    operation->cancellable = g_cancellable_new ();
    if (cancellable) {
        operation->parent = g_object_ref (cancellable);
        operation->parent_cancelled_id = g_signal_connect (cancellable, "cancelled", G_CALLBACK (parent_cancelled), operation);
    }
    xtra->operation = operation;
    /* Keep the operation alive if setup fails before the first asynchronous call. */
    operation_ref (operation);
    if (gnss_xtra_is_cache_valid (xtra, 24))
        inject_file (operation);
    else
        download_file (operation);
    operation_unref (operation);
}

void
gnss_xtra_cancel (GnssXtra *xtra)
{
    if (xtra->operation)
        parent_cancelled (NULL, xtra->operation);
}

void
gnss_xtra_free (GnssXtra *xtra)
{
    xtra->callback = NULL;
    xtra->user_data = NULL;
    gnss_xtra_cancel (xtra);
    xtra_unref (xtra);
}
