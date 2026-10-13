/* ntfsharness core: C entry points called by the Swift CLI. */
#ifndef NHCORE_H
#define NHCORE_H
#ifdef __cplusplus
extern "C" {
#endif
/* Runs one harness command. argv[0] is the command name. Returns a process exit code. */
int nh_cmd(int argc, char **argv);
/* Library version string reported by libntfs-3g. */
const char *nh_libntfs_version(void);
/* 1 when path is inside the allowed scratch root (images only live there). */
int nh_path_allowed(const char *path);
#ifdef __cplusplus
}
#endif
#endif
