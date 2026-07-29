#ifndef UTUBBU_CLOCK_PROBE_H
#define UTUBBU_CLOCK_PROBE_H

typedef struct UtubbuClockInfo {
    int cpu_mhz;
    int bus_mhz;
    int samples;
    int experimental;
} UtubbuClockInfo;

/* Reads the current clock only. It never changes CPU or bus frequency. */
UtubbuClockInfo utubbu_detect_clock(void);

#endif
