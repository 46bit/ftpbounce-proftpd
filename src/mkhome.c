/*
 * ProFTPD - FTP server daemon
 * Copyright (c) 2003 The ProFTPD Project team
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307, USA.
 *
 * As a special exemption, The ProFTPD Project team and other respective
 * copyright holders give permission to link this program with OpenSSL, and
 * distribute the resulting executable, without including the source code for
 * OpenSSL in the source distribution.
 */

/*
 * Home-on-demand support
 * $Id: mkhome.c,v 1.4 2003-11-09 23:32:07 castaglia Exp $
 */

#include "conf.h"
#include "privs.h"

static int create_dir(const char *dir, uid_t uid, gid_t gid,
    mode_t mode) {
  mode_t prevmask;
  struct stat st;
  int res = -1;

  pr_fs_clear_cache();
  res = pr_fsio_stat(dir, &st);

  if (res == -1 && errno != ENOENT) {
    pr_log_pri(PR_LOG_WARNING, "error checking '%s': %s", dir,
      strerror(errno));
    return -1;
  }

  /* The directory already exists. */
  if (res == 0) {
    pr_log_debug(DEBUG3, "CreateHome: '%s' already exists", dir);
    return 0;
  }

  /* The given mode is absolute, not subject to any Umask setting. */
  prevmask = umask(0);

  if (pr_fsio_mkdir(dir, mode) < 0) {
    pr_log_pri(PR_LOG_WARNING, "error creating '%s': %s", dir,
      strerror(errno));
    return -1;
  }

  if (pr_fsio_chown(dir, uid, gid) < 0) {
    pr_log_pri(PR_LOG_WARNING, "error setting ownership of '%s': %s", dir,
      strerror(errno));
    return -1;
  }

  umask(prevmask);

  pr_log_debug(DEBUG6, "CreateHome: directory '%s' created", dir);
  return 0;
}

/* Walk along a path, making sure that all directories in that path exist,
 * creating them if necessary.
 */
static int create_path(pool *p, const char *path, const char *user, uid_t uid,
    gid_t gid, mode_t dir_mode, mode_t dst_mode) {
  char *currpath = NULL, *tmppath = NULL;
  struct stat st;

  pr_fs_clear_cache();
  if (pr_fsio_stat(path, &st) == 0)
    /* Path already exists, nothing to be done. */
    return 0;

  pr_log_debug(DEBUG3, "creating home directory '%s' for user '%s'", path,
    user);
  tmppath = pstrdup(p, path);

  currpath = "/";
  while (tmppath && *tmppath) {
    char *currdir = strsep(&tmppath, "/");
    currpath = pdircat(p, currpath, currdir, NULL);

    /* If tmppath is NULL, we are creating the last part of the path, so we
     * use the configured mode, and chown it to the given UID and GID.
     */
    if ((tmppath == NULL) || (*tmppath == '\0'))
      create_dir(currpath, uid, gid, dst_mode);
    else
      create_dir(currpath, 0, 0, dir_mode);

    pr_signals_handle();
  }

  pr_log_debug(DEBUG3, "home directory '%s' created", path);
  return 0;
}

static int copy_file(pool *p, const char *src, const char *dst, uid_t uid,
    gid_t gid, mode_t mode) {
  pr_fh_t *src_fh = NULL, *dst_fh = NULL;
  char buf[PR_TUNABLE_BUFFER_SIZE] = {'\0'};
  int res = 0;

  src_fh = pr_fsio_open(src, O_RDONLY);
  if (src_fh == NULL) {
    pr_log_debug(DEBUG2, "CreateHome: trouble with '%s': %s", src, 
      strerror(errno));
    return -1;
  }

  dst_fh = pr_fsio_open(dst, O_WRONLY|O_CREAT|O_EXCL);
  if (dst_fh == NULL) {
    pr_log_debug(DEBUG2, "CreateHome: trouble with %s': %s", dst, 
      strerror(errno));
    pr_fsio_close(src_fh);
    return -1;
  }

  /* Make sure the destination file starts with a zero size. */
  pr_fsio_truncate(dst, 0);

  while ((res = pr_fsio_read(src_fh, buf, sizeof(buf))) > 0) {
    if (pr_fsio_write(dst_fh, buf, res) != res) {
      pr_log_pri(PR_LOG_WARNING, "CreateHome: error writing to '%s': %s",
        dst, strerror(errno));
      break;
    }

    pr_signals_handle();
  }

  /* Make sure the destination file has the proper ownership and mode. */
  if (pr_fsio_chown(dst, uid, gid) < 0)
    pr_log_pri(PR_LOG_WARNING, "CreateHome: error chown'ing '%s' to %u/%u: %s",
      dst, (unsigned int) uid, (unsigned int) gid, strerror(errno));
 
  if (pr_fsio_chmod(dst, mode) < 0)
    pr_log_pri(PR_LOG_WARNING, "CreateHome: error chmod'ing '%s' to %04o: %s",
      dst, (unsigned int) mode, strerror(errno));

  pr_fsio_close(src_fh);
  if (pr_fsio_close(dst_fh) < 0)
    pr_log_pri(PR_LOG_WARNING, "CreateHome: error closing '%s': %s", dst,
      strerror(errno));

  return 0;
}

static int copy_symlink(pool *p, const char *src_dir, const char *src_path,
    const char *dst_dir, const char *dst_path, uid_t uid, gid_t gid) {
  char *link_path = pcalloc(p, PR_TUNABLE_BUFFER_SIZE);
  int len;

  if ((len = pr_fsio_readlink(src_path, link_path, sizeof(link_path)-1)) < 0) {
    pr_log_pri(PR_LOG_WARNING, "CreateHome: error reading link '%s': %s",
      src_path, strerror(errno));
    return -1;
  }
  link_path[len] = '\0';

  /* If the target of the link lies within the src path, rename that portion
   * of the link to be the corresponding part of the dst path.
   */
  if (strncmp(link_path, src_dir, strlen(src_dir)) == 0)
    link_path = pdircat(p, dst_dir, link_path + strlen(src_dir), NULL);

  if (pr_fsio_symlink(link_path, dst_path) < 0) {
    pr_log_pri(PR_LOG_WARNING, "CreateHome: error symlinking '%s' to '%s': %s",
      link_path, dst_path, strerror(errno));
    return -1;
  }

  /* Make sure the new symlink has the proper ownership. */
  if (pr_fsio_chown(dst_path, uid, gid) < 0)
    pr_log_pri(PR_LOG_WARNING, "CreateHome: error chown'ing '%s' to %u/%u: %s",
      dst_path, (unsigned int) uid, (unsigned int) gid, strerror(errno));

  return 0; 
}

/* srcdir is to be considered a "skeleton" directory, in the manner of
 * /etc/skel, and destdir is a user's newly created home directory that needs
 * to be populated with the files in srcdir.
 */
static int copy_dir(pool *p, const char *src_dir, const char *dst_dir,
    uid_t uid, gid_t gid) {
  DIR *dh = NULL;
  struct dirent *dent = NULL;

  dh = opendir(src_dir);
  if (dh == NULL) {
    pr_log_pri(PR_LOG_WARNING, "CreateHome: error copying '%s' skel files: %s",
      src_dir, strerror(errno));
    return -1;
  }

  while ((dent = readdir(dh)) != NULL) {
    struct stat st;
    char *src_path = pdircat(p, src_dir, dent->d_name, NULL);
    char *dst_path = pdircat(p, dst_dir, dent->d_name, NULL);

    pr_signals_handle();

    /* Skip "." and ".." */
    if (strcmp(dent->d_name, ".") == 0 || strcmp(dent->d_name, "..") == 0)
      continue;

    if (pr_fsio_lstat(src_path, &st) < 0) {
      pr_log_debug(DEBUG3, "CreateHome: unable to stat '%s' (%s), skipping",
        src_path, strerror(errno));
      continue;
    }

    /* Is this path to a directory? */
    if (S_ISDIR(st.st_mode)) {

      create_dir(dst_path, uid, gid, st.st_mode);
      copy_dir(p, src_path, dst_path, uid, gid);
      continue;


    /* Is this path to a regular file? */
    } else if (S_ISREG(st.st_mode)) {
      mode_t dst_mode = st.st_mode;

      /* Make sure to prevent S{U,G}ID permissions on target files. */

      if (dst_mode & S_ISUID)
        dst_mode &= ~S_ISUID;

      if (dst_mode & S_ISGID)
        dst_mode &= ~S_ISGID;

      copy_file(p, src_path, dst_path, uid, gid, dst_mode);

      continue;

    /* Is this path a symlink? */
    } else if (S_ISLNK(st.st_mode)) {

      copy_symlink(p, src_dir, src_path, dst_dir, dst_path, uid, gid);
      continue;

    /* All other file types are skipped */
    } else {
      pr_log_debug(DEBUG3, "CreateHome: skipping skel file '%s'", src_path);
      continue;
    }
  }

  closedir(dh);
  return 0;
}

/* Check for a CreateHome directive, and act on it if present.  If not, do
 * nothing.
 */
int create_home(pool *p, const char *home, const char *user, uid_t uid,
    gid_t gid) {
  config_rec *c = find_config(main_server->conf, CONF_PARAM, "CreateHome",
    FALSE);

  if (!c || (c && *((unsigned char *) c->argv[0]) == FALSE))
    return 0;

  PRIVS_ROOT

  /* Create the configured path. */
  if (create_path(p, home, user, uid, gid, *((mode_t *) c->argv[2]),
      *((mode_t *) c->argv[1])) < 0) {
    PRIVS_RELINQUISH
    return -1;
  }

  if (c->argv[3]) {
    char *skel_dir = c->argv[3];

    /* Populate the home directory with files from the configured
     * skeleton (a al /etc/skel) directory.
     */

    pr_log_debug(DEBUG4, "CreateHome: copying skel files from '%s' into '%s'",
      skel_dir, home);
    if (copy_dir(p, skel_dir, home, uid, gid) < 0)
      pr_log_debug(DEBUG4, "CreateHome: error copying skel files");
  }

  PRIVS_RELINQUISH
  return 0;
}
