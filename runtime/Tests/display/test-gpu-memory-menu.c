/* Tests ui/pingouin-gpu-memory.h (pingouin-cocoa-graphics-memory.patch). */
#include "pingouin-gpu-memory.h"

static int failures;

static void expect(const char *what, const char *got, const char *want)
{
    if (strcmp(got, want)) {
        printf("FAIL: %s: got '%s', want '%s'\n", what, got, want);
        failures++;
    }
}

int main(void)
{
    char line[160];
    PingouinGpuMemory m;

    /* The status file as the renderer writes it. */
    const char *file = "in_use_mb=1638\npeak_mb=2662\nbudget_mb=36864\npressure=normal\n"
                       "refused=0\nlost=1\nlost_last=Hypr_land\nlost_recent=quickshell,Hyprland\n";
    if (!pingouin_gpu_memory_parse(file, &m)) {
        printf("FAIL: status file not read\n");
        failures++;
    }
    pingouin_gpu_memory_line(&m, line, sizeof(line));
    expect("normal", line, "Graphics memory: 1.6 GB (peak 2.6 GB)");

    pingouin_gpu_memory_parse("in_use_mb=900\npeak_mb=1100\npressure=warn", &m);
    pingouin_gpu_memory_line(&m, line, sizeof(line));
    expect("warn, no last newline", line,
           "Graphics memory: 900 MB (peak 1.1 GB), the Mac is short of memory");

    pingouin_gpu_memory_parse("in_use_mb=6400\npeak_mb=6400\npressure=critical\n", &m);
    pingouin_gpu_memory_line(&m, line, sizeof(line));
    expect("critical", line, "Graphics memory: 6.2 GB (peak 6.2 GB), the Mac is short of memory");

    /* Nothing written yet, junk, negative and over-long values. */
    if (pingouin_gpu_memory_parse("", &m) || pingouin_gpu_memory_parse("peak_mb=5\n", &m)) {
        printf("FAIL: a file without in_use_mb counts as read\n");
        failures++;
    }
    pingouin_gpu_memory_line(NULL, line, sizeof(line));
    expect("not yet", line, "Graphics memory: not measured yet");
    char lng[600];
    memset(lng, 'x', sizeof(lng) - 1);
    lng[sizeof(lng) - 1] = '\0';
    memcpy(lng, "pressure=", 9);
    char text[800];
    snprintf(text, sizeof(text), "%s\nin_use_mb=-5\npeak_mb=abc\n", lng);
    pingouin_gpu_memory_parse(text, &m);
    pingouin_gpu_memory_line(&m, line, sizeof(line));
    expect("junk", line, "Graphics memory: 0 MB (peak 0 MB)");

    pingouin_vm_memory_line(16ULL << 30, line, sizeof(line));
    expect("vm 16 GB", line, "VM memory: 16 GB");
    pingouin_vm_memory_line(48ULL << 30, line, sizeof(line));
    expect("vm 48 GB", line, "VM memory: 48 GB");
    pingouin_vm_memory_line(1536ULL << 20, line, sizeof(line));
    expect("vm 1.5 GB", line, "VM memory: 1.5 GB");

    if (failures) {
        return 1;
    }
    printf("gpu memory menu: all checks passed\n");
    return 0;
}
