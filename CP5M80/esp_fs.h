/* esp_fs.h - ESP32 nao tem diretorio corrente: emulamos aqui */
#ifndef ESP_FS_H
#define ESP_FS_H
#include <stdio.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
static char esp_cwd[260] = "/sd";
static void esp_abs(char *o, const char *p)
{
    if (p[0] == '/') { snprintf(o, 260, "%s", p); }
    else if (!strcmp(p, ".")) { snprintf(o, 260, "%s", esp_cwd); }
    else if (!strcmp(p, "..")) {
        char *s; snprintf(o, 260, "%s", esp_cwd);
        s = strrchr(o, '/');
        if (s && s != o && strcmp(o, "/sd")) *s = 0;
    } else snprintf(o, 260, "%s/%s", esp_cwd, p);
}
static int esp_chdir(const char *p)
{
    char o[260]; struct stat st; DIR *d;
    esp_abs(o, p);
    if (!strcmp(o, "/sd")) { strcpy(esp_cwd, o); return 0; }
    if (strncmp(o, "/sd/", 4)) return -1;
    d = opendir(o); if (!d) return -1; closedir(d); (void)st;
    strcpy(esp_cwd, o); return 0;
}
static char *esp_getcwd(char *b, size_t n) { snprintf(b, n, "%s", esp_cwd); return b; }
static DIR *esp_opendir(const char *p) { char o[260]; esp_abs(o, p); return opendir(o); }
static int esp_stat(const char *p, struct stat *s) { char o[260]; esp_abs(o, p); return stat(o, s); }
#define chdir esp_chdir
#define getcwd esp_getcwd
#define opendir esp_opendir
#define stat(a,b) esp_stat(a,b)
#endif
