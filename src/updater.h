#ifndef UPDATER_H
#define UPDATER_H

#define FW_VERSION_MAJOR 1
#define FW_VERSION_MINOR 0

void updater_main(void) __attribute__((noreturn)); /* update-mode loop */
void version_main(void); /* version display; returns when PLAY resumes boot */

#endif
