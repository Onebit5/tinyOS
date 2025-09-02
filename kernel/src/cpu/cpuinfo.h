#ifndef CPU_CPUINFO_H
#define CPU_CPUINFO_H

/* what the cpu says about itself when asked nicely */

/* the 12-character vendor string, e.g. "GenuineIntel". buf needs 13 */
void cpu_vendor(char *buf);

/* the 48-character marketing name, e.g. "AMD Ryzen 9 ...". buf needs 49.
 * older or emulated cpus may not have one, in which case you get the
 * vendor string instead */
void cpu_brand(char *buf);

#endif
