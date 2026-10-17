#include "gnss-common.h"
#include "../src/gnss-xtra.h"
#include <glib/gstdio.h>
#include <utime.h>

static void
test_xtra_cache_validity (void)
{
    gchar *tmp_dir = g_dir_make_tmp ("gnss-xtra-test-XXXXXX", NULL);
    g_assert_nonnull (tmp_dir);

    GnssXtra *xtra = gnss_xtra_new (tmp_dir);
    g_assert_nonnull (xtra);

    /* 1. Missing file -> FALSE */
    g_assert_false (gnss_xtra_is_cache_valid (xtra, 24));

    /* 2. File too small (< 1024) -> FALSE */
    gchar *file_path = g_build_filename (tmp_dir, "xtra3grcej.bin", NULL);
    g_file_set_contents (file_path, "short data", -1, NULL);
    g_assert_false (gnss_xtra_is_cache_valid (xtra, 24));

    /* 3. Valid size, fresh mtime -> TRUE */
    gchar *large_data = g_malloc0 (2048);
    g_file_set_contents (file_path, large_data, 2048, NULL);
    g_assert_true (gnss_xtra_is_cache_valid (xtra, 24));

    /* 4. Valid size, but old mtime (48h ago) -> FALSE */
    struct utimbuf ut;
    time_t old_time = time (NULL) - (48 * 3600);
    ut.actime = old_time;
    ut.modtime = old_time;
    utime (file_path, &ut);
    g_assert_false (gnss_xtra_is_cache_valid (xtra, 24));

    /* Clean up */
    g_unlink (file_path);
    g_rmdir (tmp_dir);
    g_free (file_path);
    g_free (large_data);
    g_free (tmp_dir);
    gnss_xtra_free (xtra);
}

int
main (int argc, char **argv)
{
    g_test_init (&argc, &argv, NULL);

    g_test_add_func ("/gnss/xtra/cache_validity", test_xtra_cache_validity);

    return g_test_run ();
}
