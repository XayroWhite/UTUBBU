#include <pspkernel.h>
#include <psppower.h>

#include "clock_probe.h"

static void sort_ints(int *values, int count)
{
    int i;
    for (i = 1; i < count; ++i) {
        int value = values[i];
        int j = i - 1;
        while (j >= 0 && values[j] > value) {
            values[j + 1] = values[j];
            --j;
        }
        values[j + 1] = value;
    }
}

UtubbuClockInfo utubbu_detect_clock(void)
{
    enum { SAMPLE_COUNT = 9 };
    int cpu[SAMPLE_COUNT];
    int bus[SAMPLE_COUNT];
    int i;
    UtubbuClockInfo result;

    /* Massimo clock ufficialmente supportato dalla PSP: evita che ARK lasci 222 MHz. */
    scePowerSetClockFrequency(333, 333, 166);
    sceKernelDelayThread(10000);

    for (i = 0; i < SAMPLE_COUNT; ++i) {
        cpu[i] = scePowerGetCpuClockFrequencyInt();
        bus[i] = scePowerGetBusClockFrequencyInt();
        sceKernelDelayThread(2000);
    }

    sort_ints(cpu, SAMPLE_COUNT);
    sort_ints(bus, SAMPLE_COUNT);

    result.cpu_mhz = cpu[SAMPLE_COUNT / 2];
    result.bus_mhz = bus[SAMPLE_COUNT / 2];
    result.samples = SAMPLE_COUNT;
    result.experimental = result.cpu_mhz > 333;
    return result;
}
