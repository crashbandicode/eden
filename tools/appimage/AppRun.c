// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later
// Native entry point: no shell is needed to launch the AppImage payload.
#define _GNU_SOURCE
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void environment(const char* key, const char* root, const char* suffix, int append) {
    char* value = NULL;
    const char* previous = append ? getenv(key) : NULL;
    if (asprintf(&value, "%s/%s%s%s", root, suffix, previous ? ":" : "",
                 previous ? previous : "") < 0 || setenv(key, value, 1) != 0) {
        perror("AppRun environment");
        exit(1);
    }
    free(value);
}

int main(int argc, char** argv) {
    char root[PATH_MAX];
    const ssize_t length = readlink("/proc/self/exe", root, sizeof(root) - 1);
    if (length < 0 || length >= (ssize_t)sizeof(root) - 1) {
        perror("AppRun location");
        return 1;
    }
    root[length] = '\0';
    char* slash = strrchr(root, '/');
    if (!slash) return 1;
    *slash = '\0';
    environment("LD_LIBRARY_PATH", root, "usr/lib", 1);
    environment("QT_PLUGIN_PATH", root, "usr/plugins", 0);
    environment("QT_QPA_PLATFORM_PLUGIN_PATH", root, "usr/plugins/platforms", 0);
    environment("PATH", root, "usr/bin", 1);
    if (!getenv("XDG_DATA_DIRS")) setenv("XDG_DATA_DIRS", "/usr/local/share:/usr/share", 1);
    environment("XDG_DATA_DIRS", root, "usr/share", 1);
    // Window screenshots include native GPU content on X11/Xwayland.
    if (!getenv("QT_QPA_PLATFORM")) setenv("QT_QPA_PLATFORM", "xcb", 1);
    char* executable = NULL;
    if (asprintf(&executable, "%s/usr/bin/eden", root) < 0) return 1;
    (void)argc;
    argv[0] = executable;
    execv(executable, argv);
    perror("AppRun exec");
    free(executable);
    return 1;
}
