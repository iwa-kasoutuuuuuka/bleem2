#include "mips.h"
#include <stdio.h>
#include <string.h>

/*
 * Debug log: ring buffer kept in RAM, mirrored to serial (printf) and
 * periodically written to the VMU (/vmu/a1/BLEEMLOG) so the host can read
 * it back from Flycast's vmu_save_A1.bin even without a serial console.
 */

int g_dlog_serial = 1;
#define LOG_SIZE   16384
#define MAX_LINES  512

static char log_buf[LOG_SIZE];
static int  log_len = 0;
static int  log_dirty = 0;
static int  line_starts[MAX_LINES];
static int  line_count = 0;

static void rebuild_lines(void) {
    line_count = 0;
    line_starts[line_count++] = 0;
    for (int i = 0; i < log_len && line_count < MAX_LINES; i++) {
        if (log_buf[i] == '\n' && i + 1 < log_len)
            line_starts[line_count++] = i + 1;
    }
}

void dlog(const char *fmt, ...) {
    char tmp[256];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(tmp, sizeof(tmp) - 2, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if (n > (int)sizeof(tmp) - 2) n = sizeof(tmp) - 2;
    if (n == 0 || tmp[n - 1] != '\n') { tmp[n++] = '\n'; tmp[n] = 0; }

    if (g_dlog_serial) printf("%s", tmp);

    if (log_len + n + 1 >= LOG_SIZE) {
        int keep = LOG_SIZE / 2;
        memmove(log_buf, log_buf + (log_len - keep), keep);
        log_len = keep;
    }
    memcpy(log_buf + log_len, tmp, n);
    log_len += n;
    log_buf[log_len] = 0;
    log_dirty = 1;
    rebuild_lines();
}

u32 g_vmu_flush_ms = 0;
static char win_buf[2048]; static int win_len = 0, win_dirty = 0;
void dlog_win(const char *line) {
    int n = (int)strlen(line);
    if (win_len + n + 2 >= (int)sizeof(win_buf)) return;
    memcpy(win_buf + win_len, line, n); win_len += n; win_buf[win_len++] = 0x0A; win_buf[win_len] = 0; win_dirty = 1;
}
static void win_flush(void) {
    if (!win_dirty) return;
    file_t f = fs_open("/vmu/a1/BLEEMWIN", O_WRONLY | O_TRUNC);
    if (f == FILEHND_INVALID) return;
    fs_write(f, win_buf, win_len); fs_close(f); win_dirty = 0;
}
int dlog_flush_vmu(void) {
    if (!log_dirty) return 0;
    uint64_t t0 = timer_ms_gettime64();
    file_t f = fs_open("/vmu/a1/BLEEMLOG", O_WRONLY | O_TRUNC);
    if (f == FILEHND_INVALID) return -1;
    fs_write(f, log_buf, log_len);
    fs_close(f);
    log_dirty = 0;
    g_vmu_flush_ms = (u32)(timer_ms_gettime64() - t0);
    win_flush();
    return 0;
}

int dlog_line_count(void) { return line_count; }

/* n = 1 -> last line, 2 -> second to last ... (copies into static buf) */
const char *dlog_line_from_end(int n) {
    static char out[96];
    if (n < 1 || n > line_count) return "";
    int idx = line_count - n;
    int s = line_starts[idx];
    int i = 0;
    while (s + i < log_len && log_buf[s + i] != '\n' && i < (int)sizeof(out) - 1) {
        out[i] = log_buf[s + i];
        i++;
    }
    out[i] = 0;
    return out;
}
