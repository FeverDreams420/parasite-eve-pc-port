/* Starting the game with a double click on parasite-eve.exe.
 *
 * Started from Explorer or a shortcut (Windows: the exe is the only program
 * on its console), it works like playtest.bat:
 *   - it runs from its own folder (settings.ini, memcards, F12 folders),
 *   - everything it prints goes to run-log.txt and the console is closed,
 *   - the last 10 seconds of picture are kept for F12 (PE_RECORD_BEFORE=10,
 *     PE_RECORD_AFTER=5), unless those are already set.
 * Started from cmd or a .bat, nothing here changes anything.
 *
 * Disc 1 without a command line: settings.ini disc=..., else
 * "Parasite Eve (USA) (Disc 1).cue" next to the exe, else (Windows) a window
 * to pick it; the choice is saved as disc= in settings.ini.
 * Disc 2 (when it is not next to Disc 1 under the same name): settings.ini
 * disc2=..., else, when Disc 1 was found here too, a window to pick it
 * (Cancel saves disc2=none, so it is not asked again).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#include <commdlg.h>
#include <io.h>
#endif

const char *pe_host_settings_disc(void);
void pe_host_settings_set_disc(const char *path);
const char *pe_host_settings_disc2(void);
void pe_host_settings_set_disc2(const char *path);

#define DISC1_NAME "Parasite Eve (USA) (Disc 1).cue"

static int g_double_clicked;
static char g_disc_path[1024];
static char g_disc2_path[1024];
static int g_disc_found_here; /* Disc 1 came from pe_host_launch_find_disc */

static int file_exists(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        return 0;
    }
    fclose(f);
    return 1;
}

void pe_host_launch_setup(void) {
#ifdef _WIN32
    DWORD ids[2];
    char dir[MAX_PATH];
    DWORD n;
    char *slash;
    if (GetConsoleProcessList(ids, 2) != 1) {
        return; /* cmd, a .bat or a terminal: leave everything as it is */
    }
    g_double_clicked = 1;
    n = GetModuleFileNameA(NULL, dir, sizeof(dir));
    if (n > 0 && n < sizeof(dir) && (slash = strrchr(dir, '\\')) != NULL) {
        *slash = 0;
        SetCurrentDirectoryA(dir);
    }
    FreeConsole();
    if (freopen("run-log.txt", "w", stdout)) {
        _dup2(_fileno(stdout), _fileno(stderr));
        setvbuf(stdout, NULL, _IONBF, 0);
        setvbuf(stderr, NULL, _IONBF, 0);
        SetStdHandle(STD_OUTPUT_HANDLE, (HANDLE)_get_osfhandle(_fileno(stdout)));
        SetStdHandle(STD_ERROR_HANDLE, (HANDLE)_get_osfhandle(_fileno(stderr)));
    }
    if (!getenv("PE_RECORD_BEFORE")) {
        _putenv("PE_RECORD_BEFORE=10");
    }
    if (!getenv("PE_RECORD_AFTER")) {
        _putenv("PE_RECORD_AFTER=5");
    }
    fprintf(stderr, "pe: started without a console: log in run-log.txt, F12 keeps the last %s s\n",
            getenv("PE_RECORD_BEFORE"));
#endif
}

#ifdef _WIN32
static int pick_disc(char *out, size_t size, const char *title) {
    OPENFILENAMEA ofn;
    char file[MAX_PATH] = "";
    memset(&ofn, 0, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.lpstrFilter = "Disc image (*.cue, *.bin)\0*.cue;*.bin\0All files\0*.*\0";
    ofn.lpstrFile = file;
    ofn.nMaxFile = sizeof(file);
    ofn.lpstrTitle = title;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (!GetOpenFileNameA(&ofn)) {
        return -1;
    }
    snprintf(out, size, "%s", file);
    return 0;
}
#endif

const char *pe_host_launch_find_disc(void) {
    const char *saved = pe_host_settings_disc();
    g_disc_found_here = 1;
    if (saved[0] && file_exists(saved)) {
        snprintf(g_disc_path, sizeof(g_disc_path), "%s", saved);
        return g_disc_path;
    }
    if (saved[0]) {
        fprintf(stderr, "pe: the disc in settings.ini is not there any more: %s\n", saved);
    }
    if (file_exists(DISC1_NAME)) {
        return DISC1_NAME;
    }
#ifdef _WIN32
    if (pick_disc(g_disc_path, sizeof(g_disc_path), "Choose your Parasite Eve (USA) Disc 1 (.cue)") == 0) {
        pe_host_settings_set_disc(g_disc_path);
        fprintf(stderr, "pe: disc saved in settings.ini: %s\n", g_disc_path);
        return g_disc_path;
    }
#endif
    return NULL;
}

/* Disc 2 when pe_host_disc_open did not find it next to Disc 1, or NULL. */
const char *pe_host_launch_find_disc2(void) {
    const char *saved = pe_host_settings_disc2();
    if (strcmp(saved, "none") == 0) {
        fprintf(stderr, "pe: no Disc 2 (settings.ini disc2=none)\n");
        return NULL;
    }
    if (saved[0] && file_exists(saved)) {
        snprintf(g_disc2_path, sizeof(g_disc2_path), "%s", saved);
        return g_disc2_path;
    }
    if (saved[0]) {
        fprintf(stderr, "pe: Disc 2 in settings.ini is not there any more: %s\n", saved);
    }
#ifdef _WIN32
    if (!g_disc_found_here) {
        return NULL; /* Disc 1 from the command line: PE_DISC2 names Disc 2 */
    }
    if (pick_disc(g_disc2_path, sizeof(g_disc2_path),
                  "Choose your Parasite Eve (USA) Disc 2 (.cue), or Cancel if you have none") == 0) {
        pe_host_settings_set_disc2(g_disc2_path);
        fprintf(stderr, "pe: Disc 2 saved in settings.ini: %s\n", g_disc2_path);
        return g_disc2_path;
    }
    pe_host_settings_set_disc2("none");
    fprintf(stderr, "pe: no Disc 2 chosen (disc2=none in settings.ini; remove it to be asked again)\n");
#endif
    return NULL;
}

/* A message the player has to see: a window when there is no console. */
void pe_host_launch_error(const char *msg) {
    fprintf(stderr, "pe: %s\n", msg);
#ifdef _WIN32
    if (g_double_clicked) {
        MessageBoxA(NULL, msg, "Parasite Eve", MB_OK | MB_ICONERROR);
    }
#endif
}
