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
#include <fcntl.h>
#include <glib.h>
#include <sys/file.h>

typedef int LockFd;
G_DEFINE_AUTO_CLEANUP_FREE_FUNC(LockFd, close, -1)

LockFd acquire_lock(FlatpakInfo *info, GError **error) {
  g_autofree char *lock_filename =
      g_build_filename(g_get_user_runtime_dir(), "app", info->app, ".flextop-lock", NULL);
  int fd = open(lock_filename, O_CREAT | O_RDWR, 0600);
  if (fd == -1) {
    int err = errno;
    g_set_error(error, G_IO_ERROR, g_io_error_from_errno(err), "Failed to open lock: %s",
                strerror(err));
    return -1;
  }

  if (flock(fd, LOCK_EX) == -1) {
    close(fd);

    int err = errno;
    g_set_error(error, G_IO_ERROR, g_io_error_from_errno(err), "Failed to set lock: %s",
                strerror(err));
    return -1;
  }

  return fd;
}

gboolean atomic_relink(GFile *link, const char *target, GError **error) {
  g_autofree char *temp_path = g_strdup_printf("%s.tmp", g_file_peek_path(link));
  g_autoptr(GFile) temp = g_file_new_for_path(temp_path);

  if (!g_file_delete(temp, NULL, error)) {
    if (!g_error_matches(*error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND)) {
      g_prefix_error(error, "Deleting old symlink: ");
      return FALSE;
    }

    g_clear_error(error);
  }

  if (!g_file_make_symbolic_link(temp, target, NULL, error)) {
    g_prefix_error(error, "Symlink %s as %s: ", target, temp_path);
    return FALSE;
  }

  if (!g_file_move(temp, link,
                   G_FILE_COPY_NOFOLLOW_SYMLINKS | G_FILE_COPY_NO_FALLBACK_FOR_MOVE |
                       G_FILE_COPY_OVERWRITE,
                   NULL, NULL, NULL, error)) {
    g_prefix_error(error, "Overwriting symlink: ");
    return FALSE;
  }

  return TRUE;
}

gboolean setup_applications_folder(DataDir *host, DataDir *priv, GError **error) {
  if (!mkdir_with_parents_exists_ok(host->applications, error)) {
    return FALSE;
  }

  GFileInfo *info = g_file_query_info(priv->applications, G_FILE_ATTRIBUTE_STANDARD_TYPE,
                                      G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, NULL, error);
  if (!info) {
    if (!g_error_matches(*error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND)) {
      g_prefix_error(error, "query %s", g_file_peek_path(priv->applications));
      return FALSE;
    }

    g_clear_error(error);
  }

  // If the applications path exists as a directory already, then someone has
  // tried installing PWAs or creating shortcuts without flextop. For safety,
  // it's easiest to just rename it to the first other path we can.
  if (info && g_file_info_get_file_type(info) == G_FILE_TYPE_DIRECTORY) {
    for (int i = 0;; i++) {
      g_autofree char *new_name =
          g_strdup_printf("%s.%d", g_file_peek_path(priv->applications), i);
      GFile *new_file = g_file_new_for_path(new_name);
      if (!g_file_move(priv->applications, new_file,
                       G_FILE_COPY_NOFOLLOW_SYMLINKS | G_FILE_COPY_NO_FALLBACK_FOR_MOVE,
                       NULL, NULL, NULL, error)) {
        if (g_error_matches(*error, G_IO_ERROR, G_IO_ERROR_EXISTS)) {
          // Just try the next name.
          g_clear_error(error);
          continue;
        }

        g_prefix_error(error, "Rename %s -> %s", g_file_peek_path(priv->applications),
                       g_file_peek_path(new_file));
        return FALSE;
      }

      break;
    }
  }

  if (!atomic_relink(priv->applications, g_file_peek_path(host->applications), error)) {
    return FALSE;
  }

  return TRUE;
}

int main() {
  g_set_prgname("flextop-init");

  g_autoptr(GError) error = NULL;

  if (!ensure_running_inside_flatpak()) {
    return 1;
  }

  g_autoptr(FlatpakInfo) info = flatpak_info_new();
  if (!flatpak_info_load(info, &error)) {
    g_warning("Failed to load flatpak info: %s", error->message);
    return 1;
  }

  g_auto(LockFd) lock = acquire_lock(info, &error);
  if (lock == -1) {
    g_warning("%s", error->message);
    return 1;
  }

  g_autoptr(DataDir) host = data_dir_new_host(info);
  g_autoptr(DataDir) priv = data_dir_new_private();

  if (!setup_applications_folder(host, priv, &error)) {
    g_warning("Failed to set up applications folder: %s", error->message);
    return 1;
  }

  return 0;
}
