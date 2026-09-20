#ifndef TURBO_FS_H
#define TURBO_FS_H

typedef struct turbo_fs_stat_s {
  int is_symlink;
  int is_file;
  int is_directory;
  unsigned long long size;
} turbo_fs_stat_t;

int turbo_fs_path_is_absolute(const char *path);
int turbo_fs_lstat(const char *path, turbo_fs_stat_t *metadata);

#endif
