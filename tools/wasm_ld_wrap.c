/* wasm-ld wrapper for the browser build (tools/build_wasm.py).
 *
 * Emscripten runs wasm-ld from its LLVM directory; build_wasm.py points it
 * (EM_LLVM_ROOT) at a mirror of that directory where this program stands in
 * for wasm-ld.exe. It runs the real linker (wasm-ld-real.exe), and when the
 * link reports function signature mismatches it relinks with each mismatched
 * callee kept alive (--export-if-defined) and then runs tools/wasm_sigfix.py
 * on the output, so the trapping mismatch stubs become adapters before
 * emcc's post-link (ASYNCIFY) instruments them. Windows only (zig cc).
 *
 * Environment: MP6_WASM_SIGFIX = "<python.exe>|<path to wasm_sigfix.py>". */
#include <process.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

#define MAX_NAMES 4096

static char g_names[MAX_NAMES][256];
static int g_nameCount;

static int run_capture(const char *cmdline, const char *logPath)
{
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    SECURITY_ATTRIBUTES sa = { sizeof(sa), NULL, TRUE };
    HANDLE log = CreateFileA(logPath, GENERIC_WRITE, FILE_SHARE_READ, &sa, CREATE_ALWAYS, 0, NULL);
    DWORD code = 1;
    char *mutableCmd = _strdup(cmdline);
    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    si.hStdOutput = log;
    si.hStdError = log;
    if (!CreateProcessA(NULL, mutableCmd, NULL, NULL, TRUE, 0, NULL, NULL, &si, &pi)) {
        fprintf(stderr, "wasm_ld_wrap: cannot start: %s\n", cmdline);
        CloseHandle(log);
        free(mutableCmd);
        return 1;
    }
    WaitForSingleObject(pi.hProcess, INFINITE);
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    CloseHandle(log);
    free(mutableCmd);
    return (int)code;
}

static void forward_log(const char *logPath)
{
    FILE *f = fopen(logPath, "rb");
    char line[4096];
    if (!f) return;
    while (fgets(line, sizeof(line), f)) {
        const char *key = "function signature mismatch: ";
        const char *hit = strstr(line, key);
        fputs(line, stderr);
        if (hit && g_nameCount < MAX_NAMES) {
            char name[256];
            int i, dup = 0;
            sscanf(hit + strlen(key), "%255s", name);
            for (i = 0; i < g_nameCount; i++) dup |= strcmp(g_names[i], name) == 0;
            if (!dup) strcpy(g_names[g_nameCount++], name);
        }
    }
    fclose(f);
}

int main(void)
{
    char self[MAX_PATH], real[MAX_PATH + 32], logPath[MAX_PATH + 32];
    const char *cmd = GetCommandLineA();
    const char *args;
    char *full, *out = NULL;
    size_t len;
    int rc, i;

    GetModuleFileNameA(NULL, self, sizeof(self));
    snprintf(real, sizeof(real), "%.*s-real.exe", (int)(strlen(self) - 4), self);
    snprintf(logPath, sizeof(logPath), "%s.log", self);

    /* Skip argv[0] in the raw command line, keeping the rest verbatim. */
    args = cmd;
    if (*args == '"') {
        args = strchr(args + 1, '"');
        args = args ? args + 1 : cmd + strlen(cmd);
    } else {
        while (*args && *args != ' ' && *args != '\t') args++;
    }

    len = strlen(real) + strlen(args) + 8;
    full = malloc(len);
    snprintf(full, len, "\"%s\"%s", real, args);
    rc = run_capture(full, logPath);
    forward_log(logPath);
    if (rc != 0 || g_nameCount == 0) return rc;

    /* Relink with every mismatched callee kept as a GC root. */
    len = strlen(full) + (size_t)g_nameCount * 300 + 8;
    {
        char *relink = malloc(len);
        strcpy(relink, full);
        for (i = 0; i < g_nameCount; i++) {
            strcat(relink, " --export-if-defined=");
            strcat(relink, g_names[i]);
        }
        rc = run_capture(relink, logPath);
        g_nameCount = 0;
        forward_log(logPath);
        free(relink);
        if (rc != 0) return rc;
    }

    /* Find the output file: "-o <path>" in the arguments or response file. */
    {
        char *argsCopy = _strdup(args), *tok, *ctx = NULL, *prev = NULL;
        for (tok = strtok_s(argsCopy, " \t", &ctx); tok; tok = strtok_s(NULL, " \t", &ctx)) {
            if (prev && strcmp(prev, "-o") == 0) out = tok;
            prev = tok;
        }
        if (out == NULL) {
            /* emcc passes everything through @response files. */
            free(argsCopy);
            argsCopy = _strdup(args);
            for (tok = strtok_s(argsCopy, " \t", &ctx); tok; tok = strtok_s(NULL, " \t", &ctx)) {
                char *p = tok;
                if (*p == '\'' || *p == '"') p++;
                if (*p == '@') {
                    char path[MAX_PATH];
                    FILE *f;
                    char word[4096];
                    int wantOut = 0;
                    snprintf(path, sizeof(path), "%s", p + 1);
                    path[strcspn(path, "'\"")] = 0;
                    f = fopen(path, "rb");
                    while (f && fscanf(f, "%4095s", word) == 1) {
                        if (wantOut) {
                            char *w = word;
                            size_t n;
                            if (*w == '"') w++;
                            n = strlen(w);
                            if (n && w[n - 1] == '"') w[n - 1] = 0;
                            out = _strdup(w);
                            break;
                        }
                        wantOut = strcmp(word, "-o") == 0;
                    }
                    if (f) fclose(f);
                }
            }
        }
    }
    if (out == NULL) {
        fprintf(stderr, "wasm_ld_wrap: could not find the -o output to fix\n");
        return 1;
    }
    {
        const char *spec = getenv("MP6_WASM_SIGFIX");
        char python[MAX_PATH], script[MAX_PATH], fix[3 * MAX_PATH];
        const char *bar = spec ? strchr(spec, '|') : NULL;
        if (!bar) {
            fprintf(stderr, "wasm_ld_wrap: MP6_WASM_SIGFIX not set\n");
            return 1;
        }
        snprintf(python, sizeof(python), "%.*s", (int)(bar - spec), spec);
        snprintf(script, sizeof(script), "%s", bar + 1);
        snprintf(fix, sizeof(fix), "\"%s\" \"%s\" \"%s\"", python, script, out);
        rc = run_capture(fix, logPath);
        forward_log(logPath);
        return rc;
    }
}
