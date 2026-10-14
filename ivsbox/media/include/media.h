#ifndef __MEDIA_H__
#define __MEDIA_H__

// 录制状态
#define NoRecord 0
#define Recording 1

int media_init();
int media_fini();

int media_capture(const char *path);
int media_record_start(int id, const char *url);
int media_record_stop(int id);
int media_record_status(int id);

#endif
