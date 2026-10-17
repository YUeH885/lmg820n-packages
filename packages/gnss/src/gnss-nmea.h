#pragma once

#include <glib.h>

/* Computes NMEA checksum (XOR of characters between '$'/'!' and '*'). */
guint8 gnss_nmea_calc_checksum (const gchar *sentence);

/* Verifies checksum if '*' followed by two hex digits is present. Returns TRUE if valid or no checksum present. */
gboolean gnss_nmea_verify_checksum (const gchar *sentence);

/* Validates whether string is a well-formed NMEA sentence. */
gboolean gnss_nmea_is_valid_sentence (const gchar *sentence);

/* Normalizes a single sentence to ensure leading '$'/'!' and trailing "\r\n". */
gchar *gnss_nmea_normalize_sentence (const gchar *sentence);

/* Splits a raw buffer possibly containing multiple lines and returns array of normalized sentences. */
gchar **gnss_nmea_split_and_normalize (const gchar *raw_data);
