#ifndef POCKETJS_IPOD_PHOTO_FS_STORE_H
#define POCKETJS_IPOD_PHOTO_FS_STORE_H
#include <stddef.h>
#include <stdint.h>
int pjs_fs_store_open(const uint8_t *app_id, uint32_t length);
void pjs_fs_store_close(void);
int pjs_fs_read_json(const char *, uint32_t, uint32_t, char *, uint32_t, uint32_t *);
int pjs_fs_write(const char *, const char *, size_t, int32_t);
int pjs_fs_remove(const char *, int32_t);
int pjs_fs_list_json(const char *, uint32_t, char *, uint32_t, uint32_t *);
int pjs_fs_stat_json(const char *, char *, uint32_t, uint32_t *);
int pjs_fs_mkdir(const char *);
int pjs_fs_rename(const char *, const char *);
int pjs_fs_usage_json(char *, uint32_t, uint32_t *);
int pjs_fs_last_error(char *, uint32_t, uint32_t *);
int pjs_fs_store_last_error_code(void);
#endif
