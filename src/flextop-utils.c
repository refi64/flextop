/* Copyright (c) 2020 Endless OS Foundation LLC.

   This program is free software: you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation, either version 2 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program.  If not, see <https://www.gnu.org/licenses/>. */

#include "flextop-utils.h"

#include <errno.h>
#include <glib.h>
#include <gtk/gtk.h>
#include <string.h>

gboolean ensure_running_inside_flatpak() {
  g_autoptr(GFile) flatpak_info = g_file_new_for_path("/.flatpak-info");
  if (!g_file_query_exists(flatpak_info, NULL)) {
    g_printerr("This may only be run inside a Flatpak!\n");
    return FALSE;
  }

  return TRUE;
}

gboolean mkdir_with_parents_exists_ok(GFile *dir, GError **error) {
  g_autoptr(GError) local_error = NULL;
  if (!g_file_make_directory_with_parents(dir, NULL, &local_error) &&
      !g_error_matches(local_error, G_IO_ERROR, G_IO_ERROR_EXISTS)) {
    g_propagate_error(error, local_error);
    return FALSE;
  }

  return TRUE;
}

FlatpakInfo *flatpak_info_new() { return g_new0(FlatpakInfo, 1); }

gboolean flatpak_info_load(FlatpakInfo *info, GError **error) {
  g_autoptr(GKeyFile) key_file = g_key_file_new();
  if (!g_key_file_load_from_file(key_file, "/.flatpak-info", G_KEY_FILE_NONE, error)) {
    return FALSE;
  }

  info->app = g_key_file_get_string(key_file, "Application", "name", error);
  info->branch = g_key_file_get_string(key_file, "Instance", "branch", error);
  info->arch = g_key_file_get_string(key_file, "Instance", "arch", error);

  if (info->app == NULL || info->branch == NULL || info->arch == NULL) {
    return FALSE;
  }

  return TRUE;
}

void flatpak_info_free(FlatpakInfo *info) {
  g_clear_pointer(&info->app, g_free);
  g_clear_pointer(&info->branch, g_free);
  g_clear_pointer(&info->arch, g_free);
}

static void replace_all(char *s, char c, char r) {
  char *p = s;
  while ((p = strchr(s, c))) {
    *p = r;
  }
}

static char *escape_app_id(FlatpakInfo *info) {
  g_autofree char *escaped_app_id = g_strdup(info->app);
  replace_all(escaped_app_id, '-', '_');
  replace_all(escaped_app_id, '.', '-');
  return g_steal_pointer(&escaped_app_id);
}

char *add_app_icon_prefix(FlatpakInfo *info, const char *icon) {
  g_autofree char *escaped_app_id = escape_app_id(info);
  return g_strdup_printf("flatpak-%s-%s", escaped_app_id, icon);
}

DataDir *data_dir_new_for_root(GFile *root) {
  DataDir *result = g_new0(DataDir, 1);
  result->root = g_object_ref(root);
  result->applications = g_file_get_child(root, "applications");
  result->icons = g_file_get_child(root, "icons");

  return result;
}

DataDir *data_dir_new_host(FlatpakInfo *info) {
  const char *home = g_get_home_dir();
  g_autofree char *share = g_build_filename(home, ".local", "share", NULL);
  g_autoptr(GFile) share_file = g_file_new_for_path(share);

  DataDir *result = data_dir_new_for_root(share_file);

  // Append a Flatpak-specific suffix onto the result applications directory.
  g_autoptr(GFile) original_applications = g_steal_pointer(&result->applications);
  g_autofree char *escaped_app_id = escape_app_id(info);
  g_autofree char *applications_subdir = g_strdup_printf("flatpak-%s", escaped_app_id);
  result->applications = g_file_get_child(original_applications, applications_subdir);

  return result;
}

DataDir *data_dir_new_private() {
  const char *root = g_get_user_data_dir();
  g_autoptr(GFile) root_file = g_file_new_for_path(root);
  return data_dir_new_for_root(root_file);
}

// This is reserved for the null device, so no conflicts with actual devices
// is basically a guarantee.
#define UNIX_DEVICE_FAIL 0

static gboolean query_path_info(GFile *file, guint32 *out_device, gboolean *out_writable,
                                GError **error) {
  if (out_device != NULL) {
    g_autoptr(GFileInfo) info =
        g_file_query_info(file, G_FILE_ATTRIBUTE_UNIX_DEVICE,
                          G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, NULL, error);
    if (info == NULL) {
      return FALSE;
    }

    *out_device = g_file_info_get_attribute_uint32(info, G_FILE_ATTRIBUTE_UNIX_DEVICE);
  }

  if (out_writable != NULL) {
    if (access(g_file_peek_path(file), W_OK) == -1) {
      if (errno != EROFS) {
        int err = errno;
        g_warning("Unexpected error from access(%s): %s [%d]", g_file_peek_path(file),
                  strerror(err), err);
      }

      *out_writable = FALSE;
    } else {
      *out_writable = TRUE;
    }
  }

  return TRUE;
}

static void get_lowest_existing_parent_info(GFile *file, guint32 *out_device,
                                            gboolean *out_writable) {
  g_autoptr(GError) error = NULL;
  g_autoptr(GFile) current = g_file_dup(file);

  while (!query_path_info(current, out_device, out_writable, &error)) {
    if (!g_error_matches(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND)) {
      g_warning("Unexpected error from querying %s (for %s): %s",
                g_file_peek_path(current), g_file_peek_path(file), error->message);
    }

    g_clear_error(&error);

    if (strcmp(g_file_peek_path(current), "/") == 0) {
      g_error("Reached / but no paths could have info retrieved (for %s)",
              g_file_peek_path(file));
    }

    GFile *next = g_file_get_parent(current);
    g_clear_object(&current);
    current = next;
  }
}

gboolean data_dir_test_access(DataDir *dir) {
  g_autoptr(GFile) root_file = g_file_new_for_path("/");
  guint32 root_device = 0;
  g_warn_if_fail(query_path_info(root_file, &root_device, NULL, NULL));

  g_debug("root_device = %" G_GUINT32_FORMAT, root_device);

  GFile *files_to_check[] = {dir->applications, dir->icons};
  for (gsize i = 0; i < G_N_ELEMENTS(files_to_check); i++) {
    guint32 device;
    gboolean writable;
    get_lowest_existing_parent_info(files_to_check[i], &device, &writable);

    if (root_device == device || !writable) {
      g_debug("device = %" G_GUINT32_FORMAT ", writable = %d", device, writable);
      return FALSE;
    }
  }

  return TRUE;
}

void data_dir_free(DataDir *dir) {
  g_object_unref(dir->root);
  g_object_unref(dir->applications);
  g_object_unref(dir->icons);
  g_free(dir);
}
