#include "gnss-xtra.h"
#include <glib/gstdio.h>

/* Exported libqmi parsers construct typed QMI fixtures for the mock transport. */
extern QmiMessageLocInjectXtraDataOutput *qmi_message_loc_inject_xtra_data_response_parse (QmiMessage *, GError **);
extern QmiMessageLocGetPredictedOrbitsDataValidityOutput *qmi_message_loc_get_predicted_orbits_data_validity_response_parse (QmiMessage *, GError **);
extern QmiIndicationLocInjectXtraDataOutput *qmi_indication_loc_inject_xtra_data_indication_parse (QmiMessage *, GError **);
extern QmiIndicationLocGetPredictedOrbitsDataValidityOutput *qmi_indication_loc_get_predicted_orbits_data_validity_indication_parse (QmiMessage *, GError **);

typedef struct {
    guint refs;
    QmiClientLoc *client;
    guint16 part;
    guint16 message;
    GAsyncReadyCallback callback;
    gpointer data;
} Peer;

static guint sent_parts;
static guint pending_events;
static guint validity_queries;
static gboolean wrong_part;
static gboolean indication_first;
static gboolean finished;
static gboolean accepted;
static guint8 payload[2500];

static Peer *peer_ref (Peer *peer) { peer->refs++; return peer; }
static void peer_unref (gpointer data)
{
    Peer *peer = data;
    if (!--peer->refs) {
        g_object_unref (peer->client);
        g_free (peer);
    }
}

static QmiMessage *
indication (guint16 message)
{
    QmiMessage *wire = qmi_message_new (QMI_SERVICE_LOC, 1, 0, message);
    /* LOC transaction flags use bit 2 for an indication. */
    wire->data[6] = 0x04;
    guint32 status = GUINT32_TO_LE (QMI_LOC_INDICATION_STATUS_SUCCESS);
    g_assert_true (qmi_message_add_raw_tlv (wire, 0x01, (guint8 *)&status, sizeof status, NULL));
    g_assert_true (qmi_message_is_indication (wire));
    return wire;
}

static gboolean
deliver_indication (gpointer data)
{
    Peer *peer = data;
    g_autoptr(QmiMessage) wire = indication (peer->message);
    g_autoptr(GError) error = NULL;
    if (peer->message == 0xa7) {
        guint16 part = GUINT16_TO_LE (peer->part + wrong_part);
        g_assert_true (qmi_message_add_raw_tlv (wire, 0x10, (guint8 *)&part, sizeof part, NULL));
        g_autoptr(QmiIndicationLocInjectXtraDataOutput) output =
            qmi_indication_loc_inject_xtra_data_indication_parse (wire, &error);
        g_assert_no_error (error);
        g_assert_nonnull (output);
        g_signal_emit_by_name (peer->client, "inject-xtra-data", output);
    } else {
        guint8 info[10];
        guint64 start = GUINT64_TO_LE (1791676800);
        guint16 hours = GUINT16_TO_LE (168);
        memcpy (info, &start, 8);
        memcpy (info + 8, &hours, 2);
        g_assert_true (qmi_message_add_raw_tlv (wire, 0x10, info, sizeof info, NULL));
        g_autoptr(QmiIndicationLocGetPredictedOrbitsDataValidityOutput) output =
            qmi_indication_loc_get_predicted_orbits_data_validity_indication_parse (wire, &error);
        g_assert_no_error (error);
        g_assert_nonnull (output);
        g_signal_emit_by_name (peer->client, "get-predicted-orbits-data-validity", output);
    }
    pending_events--;
    return G_SOURCE_REMOVE;
}

static gboolean
deliver_response (gpointer data)
{
    Peer *peer = data;
    peer->callback (G_OBJECT (peer->client), NULL, peer->data);
    pending_events--;
    return G_SOURCE_REMOVE;
}

static void
schedule (QmiClientLoc *client, guint16 message, guint16 part,
           GAsyncReadyCallback callback, gpointer data)
{
    Peer *peer = g_new0 (Peer, 1);
    peer->refs = 1;
    peer->client = g_object_ref (client);
    peer->message = message;
    peer->part = part;
    peer->callback = callback;
    peer->data = data;
    pending_events += 2;
    g_idle_add_full (indication_first ? G_PRIORITY_DEFAULT : G_PRIORITY_DEFAULT_IDLE,
                     deliver_indication, peer_ref (peer), peer_unref);
    g_idle_add_full (indication_first ? G_PRIORITY_DEFAULT_IDLE : G_PRIORITY_DEFAULT,
                     deliver_response, peer_ref (peer), peer_unref);
    peer_unref (peer);
}

void
qmi_client_loc_inject_xtra_data (QmiClientLoc *client, QmiMessageLocInjectXtraDataInput *input,
                                guint timeout, GCancellable *cancellable,
                                GAsyncReadyCallback callback, gpointer data)
{
    (void)timeout; (void)cancellable;
    guint16 part, parts;
    guint32 total;
    GArray *bytes;
    g_assert_true (qmi_message_loc_inject_xtra_data_input_get_total_size (input, &total, NULL));
    g_assert_true (qmi_message_loc_inject_xtra_data_input_get_total_parts (input, &parts, NULL));
    g_assert_true (qmi_message_loc_inject_xtra_data_input_get_part_number (input, &part, NULL));
    g_assert_true (qmi_message_loc_inject_xtra_data_input_get_part_data (input, &bytes, NULL));
    g_assert_cmpuint (total, ==, sizeof payload);
    g_assert_cmpuint (parts, ==, 3);
    g_assert_cmpuint (part, ==, ++sent_parts);
    g_assert_cmpmem (bytes->data, bytes->len, payload + (part - 1) * 1024,
                     MIN (1024, sizeof payload - (part - 1) * 1024));
    schedule (client, 0xa7, part, callback, data);
}

QmiMessageLocInjectXtraDataOutput *
qmi_client_loc_inject_xtra_data_finish (QmiClientLoc *client, GAsyncResult *result, GError **error)
{
    (void)client; (void)result;
    g_autoptr(QmiMessage) request = qmi_message_new (QMI_SERVICE_LOC, 1, 1, 0xa7);
    g_autoptr(QmiMessage) reply = qmi_message_response_new (request, QMI_PROTOCOL_ERROR_NONE);
    return qmi_message_loc_inject_xtra_data_response_parse (reply, error);
}

void
qmi_client_loc_get_predicted_orbits_data_validity (QmiClientLoc *client,
    gpointer input, guint timeout,
    GCancellable *cancellable, GAsyncReadyCallback callback, gpointer data)
{
    (void)input; (void)timeout; (void)cancellable;
    validity_queries++;
    schedule (client, 0x37, 0, callback, data);
}

QmiMessageLocGetPredictedOrbitsDataValidityOutput *
qmi_client_loc_get_predicted_orbits_data_validity_finish (QmiClientLoc *client, GAsyncResult *result, GError **error)
{
    (void)client; (void)result;
    g_autoptr(QmiMessage) request = qmi_message_new (QMI_SERVICE_LOC, 1, 1, 0x37);
    g_autoptr(QmiMessage) reply = qmi_message_response_new (request, QMI_PROTOCOL_ERROR_NONE);
    return qmi_message_loc_get_predicted_orbits_data_validity_response_parse (reply, error);
}

static void
status_changed (GnssXtra *xtra, gboolean success, const gchar *message, gpointer data)
{
    (void)xtra; (void)message; (void)data;
    finished = TRUE;
    accepted = success;
}

static void
test_injection (gconstpointer data)
{
    guint scenario = GPOINTER_TO_UINT (data);
    indication_first = scenario != 1;
    wrong_part = scenario == 2;
    sent_parts = pending_events = validity_queries = 0;
    finished = accepted = FALSE;
    g_autofree gchar *dir = g_dir_make_tmp ("gnss-xtra-protocol-XXXXXX", NULL);
    g_autofree gchar *path = g_build_filename (dir, "xtra3grcej.bin", NULL);
    for (guint i = 0; i < sizeof payload; i++)
        payload[i] = i % 251;
    g_assert_true (g_file_set_contents (path, (gchar *)payload, sizeof payload, NULL));
    GnssXtra *xtra = gnss_xtra_new (dir);
    g_autoptr(GArray) servers = g_array_new (FALSE, FALSE, sizeof (gchar *));
    gchar *url = "https://example.invalid/xtra.bin";
    g_array_append_val (servers, url);
    gnss_xtra_configure (xtra, servers, 10000, 4096);
    gnss_xtra_set_callback (xtra, status_changed, NULL);
    g_autoptr(QmiClientLoc) client = g_object_new (QMI_TYPE_CLIENT_LOC, NULL);
    g_autoptr(GCancellable) cancellable = g_cancellable_new ();
    gnss_xtra_update_and_inject (xtra, client, cancellable);
    if (wrong_part)
        g_test_expect_message (G_LOG_DOMAIN, G_LOG_LEVEL_WARNING, "*part mismatch*");
    if (scenario == 3) {
        gnss_xtra_free (xtra);
        xtra = NULL;
    }
    while (pending_events || (scenario != 3 && !finished))
        g_main_context_iteration (NULL, TRUE);
    while (g_main_context_iteration (NULL, FALSE));
    if (scenario < 2) {
        g_assert_true (accepted);
        g_assert_cmpuint (sent_parts, ==, 3);
        g_assert_cmpuint (validity_queries, ==, 1);
    } else {
        g_test_assert_expected_messages ();
        g_assert_false (accepted);
        g_assert_cmpuint (sent_parts, ==, 1);
        g_assert_cmpuint (validity_queries, ==, 0);
    }
    if (xtra)
        gnss_xtra_free (xtra);
    g_unlink (path);
    g_rmdir (dir);
}

int
main (int argc, char **argv)
{
    g_test_init (&argc, &argv, NULL);
    g_test_add_data_func ("/xtra/indication-first", GUINT_TO_POINTER (0), test_injection);
    g_test_add_data_func ("/xtra/response-first", GUINT_TO_POINTER (1), test_injection);
    g_test_add_data_func ("/xtra/wrong-part", GUINT_TO_POINTER (2), test_injection);
    g_test_add_data_func ("/xtra/free-with-request-pending", GUINT_TO_POINTER (3), test_injection);
    return g_test_run ();
}
