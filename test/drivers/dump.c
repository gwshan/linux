// SPDX-License-Identifier: GPL-2.0+
/*
 * Driver to create /proc/dump, which receives various items to be dumped
 * by writing. The specified item will be dumped on reading /proc/dump.
 *
 * Author: Gavin Shan <gshan@redhat.com>
 */

#include <linux/kernel.h>
#include <linux/printk.h>
#include <linux/module.h>
#include <linux/uaccess.h>
#include <linux/bitfield.h>
#include <linux/atomic.h>
#include <linux/mm.h>
#include <linux/sched.h>
#include <linux/acpi.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>

#define DRIVER_VERSION	"0.1"
#define DRIVER_AUTHOR	"Gavin Shan, Redhat Inc"
#define DRIVER_DESC	"Dump items through procfs"

enum {
	DUMP_OPT_HELP,
	DUMP_OPT_REGISTER,
	DUMP_OPT_FEATURE_REGISTER,
	DUMP_OPT_CACHE_REGISTER,
	DUMP_OPT_MPAM_REGISTER,
	DUMP_OPT_PROCESS,
	DUMP_OPT_MM,
	DUMP_OPT_MM_MT,
	DUMP_OPT_MAX
};

static struct proc_dir_entry *pde;
static int dump_option = DUMP_OPT_MPAM_REGISTER;
static const char * const dump_options[] = {
	"help",
	"register",
	"feature_register",
	"cache_register",
	"mpam_register",
	"process",
	"mm",
	"mm_maple_tree",
	"test",
};

static void dump_show_help(struct seq_file *m)
{
	int i;

	seq_puts(m, "\n");
	seq_puts(m, "Available options:\n");
	seq_puts(m, "\n");
	for (i = 0; i < ARRAY_SIZE(dump_options); i++)
		seq_printf(m, "%s\n", dump_options[i]);

	seq_puts(m, "\n");
}

static void dump_show_register(struct seq_file *m)
{
	unsigned long v;

	/* PSTATE */
	v = read_sysreg(NZCV);
	seq_printf(m, "PSTATE:\n");
	seq_puts  (m, "----------------------------------------------\n");
	v = read_sysreg(NZCV);
	seq_printf(m, "   31 N                 %lx\n", FIELD_GET(GENMASK(31, 31), v));
	seq_printf(m, "   30 Z                 %lx\n", FIELD_GET(GENMASK(30, 30), v));
	seq_printf(m, "   29 C                 %lx\n", FIELD_GET(GENMASK(29, 29), v));
	seq_printf(m, "   28 V                 %lx\n", FIELD_GET(GENMASK(28, 28), v));
	v = read_sysreg(DAIF);
	seq_printf(m, "   09 D                 %lx\n", FIELD_GET(GENMASK( 9,  9), v));
	seq_printf(m, "   08 A                 %lx\n", FIELD_GET(GENMASK( 8,  8), v));
	seq_printf(m, "   07 I                 %lx\n", FIELD_GET(GENMASK( 7,  7), v));
	seq_printf(m, "   06 F                 %lx\n", FIELD_GET(GENMASK( 6,  6), v));
	v = read_sysreg(CurrentEL);
	seq_printf(m, "03:02 EL                %lx\n", FIELD_GET(GENMASK( 3,  2), v));
	seq_puts  (m, "\n");

	v = read_sysreg(CurrentEL);
	if (v == 0x4) {		/* EL1 */
		/* SCTLR_EL1 */
		v = read_sysreg_s(SYS_SCTLR_EL1);
		seq_printf(m, "SCTLR_EL1:              %08x_%08x\n",
			   (unsigned int)(v >> 32), (unsigned int)(v & 0xffffffff));
		seq_puts  (m, "----------------------------------------------\n");
		seq_printf(m, "   63 TIDCP             %lx\n", FIELD_GET(GENMASK(63, 63), v));
		seq_printf(m, "   62 SPINTMASK         %lx\n", FIELD_GET(GENMASK(62, 62), v));
		seq_printf(m, "   61 NMI               %lx\n", FIELD_GET(GENMASK(61, 61), v));
		seq_printf(m, "   60 EnTP2             %lx\n", FIELD_GET(GENMASK(60, 60), v));
		seq_printf(m, "   59 TCSO              %lx\n", FIELD_GET(GENMASK(59, 59), v));
		seq_printf(m, "   58 TCSO0             %lx\n", FIELD_GET(GENMASK(58, 58), v));
		seq_printf(m, "   57 EPAN              %lx\n", FIELD_GET(GENMASK(57, 57), v));
		seq_printf(m, "   56 EnALS             %lx\n", FIELD_GET(GENMASK(56, 56), v));
		seq_printf(m, "   55 EnAS0             %lx\n", FIELD_GET(GENMASK(55, 55), v));
		seq_printf(m, "   54 EnASR             %lx\n", FIELD_GET(GENMASK(54, 54), v));
		seq_printf(m, "   53 TME               %lx\n", FIELD_GET(GENMASK(53, 53), v));
		seq_printf(m, "   52 TME0              %lx\n", FIELD_GET(GENMASK(52, 52), v));
		seq_printf(m, "   51 TMT               %lx\n", FIELD_GET(GENMASK(51, 51), v));
		seq_printf(m, "   50 TMT0              %lx\n", FIELD_GET(GENMASK(50, 50), v));
		seq_printf(m, "49:46 TWEDEL            %lx\n", FIELD_GET(GENMASK(49, 46), v));
		seq_printf(m, "   45 TWEDEn            %lx\n", FIELD_GET(GENMASK(45, 45), v));
		seq_printf(m, "   44 DSSBS             %lx\n", FIELD_GET(GENMASK(44, 44), v));
		seq_printf(m, "   43 ATA               %lx\n", FIELD_GET(GENMASK(43, 43), v));
		seq_printf(m, "   42 ATA0              %lx\n", FIELD_GET(GENMASK(42, 42), v));
		seq_printf(m, "41:40 TCF               %lx\n", FIELD_GET(GENMASK(41, 40), v));
		seq_printf(m, "39:38 TCF0              %lx\n", FIELD_GET(GENMASK(39, 38), v));
		seq_printf(m, "   37 ITFSB             %lx\n", FIELD_GET(GENMASK(37, 37), v));
		seq_printf(m, "   36 BT1               %lx\n", FIELD_GET(GENMASK(36, 36), v));
		seq_printf(m, "   35 BT0               %lx\n", FIELD_GET(GENMASK(35, 35), v));
		seq_printf(m, "   34 Res0              %lx\n", FIELD_GET(GENMASK(34, 34), v));
		seq_printf(m, "   33 MSCEn             %lx\n", FIELD_GET(GENMASK(33, 33), v));
		seq_printf(m, "   32 CMOW              %lx\n", FIELD_GET(GENMASK(32, 32), v));
		seq_printf(m, "   31 EnIA              %lx\n", FIELD_GET(GENMASK(31, 31), v));
		seq_printf(m, "   30 EnIB              %lx\n", FIELD_GET(GENMASK(30, 30), v));
		seq_printf(m, "   29 LSMAOE            %lx\n", FIELD_GET(GENMASK(29, 29), v));
		seq_printf(m, "   28 nTLSMD            %lx\n", FIELD_GET(GENMASK(28, 28), v));
		seq_printf(m, "   27 EnDA              %lx\n", FIELD_GET(GENMASK(27, 27), v));
		seq_printf(m, "   26 UCI               %lx\n", FIELD_GET(GENMASK(26, 26), v));
		seq_printf(m, "   25 EE                %lx\n", FIELD_GET(GENMASK(25, 25), v));
		seq_printf(m, "   24 EOE               %lx\n", FIELD_GET(GENMASK(24, 24), v));
		seq_printf(m, "   23 SPAN              %lx\n", FIELD_GET(GENMASK(23, 23), v));
		seq_printf(m, "   22 EIS               %lx\n", FIELD_GET(GENMASK(22, 22), v));
		seq_printf(m, "   21 IESB              %lx\n", FIELD_GET(GENMASK(21, 21), v));
		seq_printf(m, "   20 TSCXT             %lx\n", FIELD_GET(GENMASK(20, 20), v));
		seq_printf(m, "   19 WXN               %lx\n", FIELD_GET(GENMASK(19, 19), v));
		seq_printf(m, "   18 nTWE              %lx\n", FIELD_GET(GENMASK(18, 18), v));
		seq_printf(m, "   17 Res1              %lx\n", FIELD_GET(GENMASK(17, 17), v));
		seq_printf(m, "   16 nTWI              %lx\n", FIELD_GET(GENMASK(16, 16), v));
		seq_printf(m, "   15 UCT               %lx\n", FIELD_GET(GENMASK(15, 15), v));
		seq_printf(m, "   14 DZE               %lx\n", FIELD_GET(GENMASK(14, 14), v));
		seq_printf(m, "   13 EnDB              %lx\n", FIELD_GET(GENMASK(13, 13), v));
		seq_printf(m, "   12 I                 %lx\n", FIELD_GET(GENMASK(12, 12), v));
		seq_printf(m, "   11 EOS               %lx\n", FIELD_GET(GENMASK(11, 11), v));
		seq_printf(m, "   10 EnRCTX            %lx\n", FIELD_GET(GENMASK(10, 10), v));
		seq_printf(m, "   09 UMA               %lx\n", FIELD_GET(GENMASK( 9,  9), v));
		seq_printf(m, "   08 SED               %lx\n", FIELD_GET(GENMASK( 8,  8), v));
		seq_printf(m, "   07 ITD               %lx\n", FIELD_GET(GENMASK( 7,  7), v));
		seq_printf(m, "   06 nAA               %lx\n", FIELD_GET(GENMASK( 6,  6), v));
		seq_printf(m, "   05 CP15BEN           %lx\n", FIELD_GET(GENMASK( 5,  5), v));
		seq_printf(m, "   04 SA0               %lx\n", FIELD_GET(GENMASK( 4,  4), v));
		seq_printf(m, "   03 SA                %lx\n", FIELD_GET(GENMASK( 3,  3), v));
		seq_printf(m, "   02 C                 %lx\n", FIELD_GET(GENMASK( 2,  2), v));
		seq_printf(m, "   01 A                 %lx\n", FIELD_GET(GENMASK( 1,  1), v));
		seq_printf(m, "   00 M                 %lx\n", FIELD_GET(GENMASK( 0,  0), v));

		/* VBAR_EL1 */
		v = read_sysreg_s(SYS_VBAR_EL1);
		seq_printf(m, "VBAR_EL1:               %08x_%08x\n",
			   (unsigned int)(v >> 32), (unsigned int)(v & 0xffffffff));
		seq_puts  (m, "----------------------------------------------\n");
		seq_printf(m, "63:11 Base              %lx\n", FIELD_GET(GENMASK(63, 11), v));
		seq_printf(m, "10:00 Res0              %lx\n", FIELD_GET(GENMASK(10,  0), v));
		seq_puts  (m, "\n");

		/* TCR_EL1 */
		v = read_sysreg_s(SYS_TCR_EL1);
		seq_printf(m, "TCR_EL1:                %08x_%08x\n",
			   (unsigned int)(v >> 32), (unsigned int)(v & 0xffffffff));
		seq_puts  (m, "----------------------------------------------\n");
		seq_printf(m, "63:62 Res0              %lx\n", FIELD_GET(GENMASK(63, 62), v));
		seq_printf(m, "   61 MTX1              %lx\n", FIELD_GET(GENMASK(61, 61), v));
		seq_printf(m, "   60 MTX0              %lx\n", FIELD_GET(GENMASK(60, 60), v));
		seq_printf(m, "   59 DS                %lx\n", FIELD_GET(GENMASK(59, 59), v));
		seq_printf(m, "   58 TCMA1             %lx\n", FIELD_GET(GENMASK(58, 58), v));
		seq_printf(m, "   57 TCMA0             %lx\n", FIELD_GET(GENMASK(57, 57), v));
		seq_printf(m, "   56 E0PD1             %lx\n", FIELD_GET(GENMASK(56, 56), v));
		seq_printf(m, "   55 E0PD0             %lx\n", FIELD_GET(GENMASK(55, 55), v));
		seq_printf(m, "   54 NFD1              %lx\n", FIELD_GET(GENMASK(54, 54), v));
		seq_printf(m, "   53 NFD0              %lx\n", FIELD_GET(GENMASK(53, 53), v));
		seq_printf(m, "   52 TBID1             %lx\n", FIELD_GET(GENMASK(52, 52), v));
		seq_printf(m, "   51 TBID0             %lx\n", FIELD_GET(GENMASK(51, 51), v));
		seq_printf(m, "   50 HWU162            %lx\n", FIELD_GET(GENMASK(50, 50), v));
		seq_printf(m, "   49 HWU161            %lx\n", FIELD_GET(GENMASK(49, 49), v));
		seq_printf(m, "   48 HWU160            %lx\n", FIELD_GET(GENMASK(48, 48), v));
		seq_printf(m, "   47 HWU159            %lx\n", FIELD_GET(GENMASK(47, 47), v));
		seq_printf(m, "   46 HWU062            %lx\n", FIELD_GET(GENMASK(46, 46), v));
		seq_printf(m, "   45 HWU061            %lx\n", FIELD_GET(GENMASK(45, 45), v));
		seq_printf(m, "   44 HWU060            %lx\n", FIELD_GET(GENMASK(44, 44), v));
		seq_printf(m, "   43 HWU059            %lx\n", FIELD_GET(GENMASK(43, 43), v));
		seq_printf(m, "   42 HPD1              %lx\n", FIELD_GET(GENMASK(42, 42), v));
		seq_printf(m, "   41 HDP0              %lx\n", FIELD_GET(GENMASK(41, 41), v));
		seq_printf(m, "   40 HD                %lx\n", FIELD_GET(GENMASK(40, 40), v));
		seq_printf(m, "   39 HA                %lx\n", FIELD_GET(GENMASK(39, 39), v));
		seq_printf(m, "   38 TBI1              %lx\n", FIELD_GET(GENMASK(38, 38), v));
		seq_printf(m, "   37 TBI0              %lx\n", FIELD_GET(GENMASK(37, 37), v));
		seq_printf(m, "   36 AS                %lx\n", FIELD_GET(GENMASK(36, 36), v));
		seq_printf(m, "   35 Res0              %lx\n", FIELD_GET(GENMASK(35, 35), v));
		seq_printf(m, "34:32 IPS               %lx\n", FIELD_GET(GENMASK(34, 32), v));
		seq_printf(m, "31:30 TG1               %lx\n", FIELD_GET(GENMASK(31, 30), v));
		seq_printf(m, "29:28 SH1               %lx\n", FIELD_GET(GENMASK(29, 28), v));
		seq_printf(m, "27:26 ORGN1             %lx\n", FIELD_GET(GENMASK(27, 26), v));
		seq_printf(m, "25:24 IRGN1             %lx\n", FIELD_GET(GENMASK(25, 24), v));
		seq_printf(m, "   23 EPD1              %lx\n", FIELD_GET(GENMASK(23, 23), v));
		seq_printf(m, "   22 A1                %lx\n", FIELD_GET(GENMASK(22, 22), v));
		seq_printf(m, "21:16 T1SZ              %lx\n", FIELD_GET(GENMASK(21, 16), v));
		seq_printf(m, "15:14 TG0               %lx\n", FIELD_GET(GENMASK(15, 14), v));
		seq_printf(m, "13:12 SH0               %lx\n", FIELD_GET(GENMASK(13, 12), v));
		seq_printf(m, "11:10 ORGN0             %lx\n", FIELD_GET(GENMASK(11, 10), v));
		seq_printf(m, "09:08 IRGN0             %lx\n", FIELD_GET(GENMASK( 9,  8), v));
		seq_printf(m, "   07 EPD0              %lx\n", FIELD_GET(GENMASK( 7,  7), v));
		seq_printf(m, "   06 Res1              %lx\n", FIELD_GET(GENMASK( 6,  6), v));
		seq_printf(m, "05:00 T0SZ              %lx\n", FIELD_GET(GENMASK( 5,  0), v));
		seq_puts  (m, "\n");

		/* TTBR0_EL1 */
		v = read_sysreg_s(SYS_TTBR0_EL1);
		seq_printf(m, "TTBR0_EL1:              %08x_%08x\n",
			   (unsigned int)(v >> 32), (unsigned int)(v & 0xffffffff));
		seq_puts  (m, "----------------------------------------------\n");
		seq_printf(m, "63:48 ASID              %lx\n", FIELD_GET(GENMASK(63, 48), v));
		seq_printf(m, "47:01 BADDR[47:1]       %lx\n", FIELD_GET(GENMASK(47,  1), v));
		seq_printf(m, "   00 CnP               %lx\n", FIELD_GET(GENMASK( 0,  0), v));
		seq_puts  (m, "\n");

		/* TTBR1_EL1 */
		v = read_sysreg_s(SYS_TTBR1_EL1);
		seq_printf(m, "TTBR1_EL1:              %08x_%08x\n",
			   (unsigned int)(v >> 32), (unsigned int)(v & 0xffffffff));
		seq_puts  (m, "----------------------------------------------\n");
		seq_printf(m, "63:48 ASID              %lx\n", FIELD_GET(GENMASK(63, 48), v));
		seq_printf(m, "47:01 BADDR[47:1]       %lx\n", FIELD_GET(GENMASK(47,  1), v));
		seq_printf(m, "   00 CnP               %lx\n", FIELD_GET(GENMASK( 0,  0), v));
		seq_puts  (m, "\n");
	} else {		/* EL2 */
		/* SCTLR_EL2 */
		v = read_sysreg_s(SYS_SCTLR_EL2);
		seq_printf(m, "SCTLR_EL2:              %08x_%08x\n",
			   (unsigned int)(v >> 32), (unsigned int)(v & 0xffffffff));
		seq_puts  (m, "----------------------------------------------\n");
		seq_printf(m, "   63 TIDCP             %lx\n", FIELD_GET(GENMASK(63, 63), v));
		seq_printf(m, "   62 SPINTMASK         %lx\n", FIELD_GET(GENMASK(62, 62), v));
		seq_printf(m, "   61 NMI               %lx\n", FIELD_GET(GENMASK(61, 61), v));
		seq_printf(m, "   60 EnTP2             %lx\n", FIELD_GET(GENMASK(60, 60), v));
		seq_printf(m, "   59 TCSO              %lx\n", FIELD_GET(GENMASK(59, 59), v));
		seq_printf(m, "   58 TCSO0             %lx\n", FIELD_GET(GENMASK(58, 58), v));
		seq_printf(m, "   57 EPAN              %lx\n", FIELD_GET(GENMASK(57, 57), v));
		seq_printf(m, "   56 EnALS             %lx\n", FIELD_GET(GENMASK(56, 56), v));
		seq_printf(m, "   55 EnAS0             %lx\n", FIELD_GET(GENMASK(55, 55), v));
		seq_printf(m, "   54 EnASR             %lx\n", FIELD_GET(GENMASK(54, 54), v));
		seq_printf(m, "   53 TME               %lx\n", FIELD_GET(GENMASK(53, 53), v));
		seq_printf(m, "   52 TME0              %lx\n", FIELD_GET(GENMASK(52, 52), v));
		seq_printf(m, "   51 TMT               %lx\n", FIELD_GET(GENMASK(51, 51), v));
		seq_printf(m, "   50 TMT0              %lx\n", FIELD_GET(GENMASK(50, 50), v));
		seq_printf(m, "49:46 TWEDEL            %lx\n", FIELD_GET(GENMASK(49, 46), v));
		seq_printf(m, "   45 TWEDEn            %lx\n", FIELD_GET(GENMASK(45, 45), v));
		seq_printf(m, "   44 DSSBS             %lx\n", FIELD_GET(GENMASK(44, 44), v));
		seq_printf(m, "   43 ATA               %lx\n", FIELD_GET(GENMASK(43, 43), v));
		seq_printf(m, "   42 ATA0              %lx\n", FIELD_GET(GENMASK(42, 42), v));
		seq_printf(m, "41:40 TCF               %lx\n", FIELD_GET(GENMASK(41, 40), v));
		seq_printf(m, "39:38 TCF0              %lx\n", FIELD_GET(GENMASK(39, 38), v));
		seq_printf(m, "   37 ITFSB             %lx\n", FIELD_GET(GENMASK(37, 37), v));
		seq_printf(m, "   36 BT1               %lx\n", FIELD_GET(GENMASK(36, 36), v));
		seq_printf(m, "   35 BT0               %lx\n", FIELD_GET(GENMASK(35, 35), v));
		seq_printf(m, "   34 Res0              %lx\n", FIELD_GET(GENMASK(34, 34), v));
		seq_printf(m, "   33 MSCEn             %lx\n", FIELD_GET(GENMASK(33, 33), v));
		seq_printf(m, "   32 CMOW              %lx\n", FIELD_GET(GENMASK(32, 32), v));
		seq_printf(m, "   31 EnIA              %lx\n", FIELD_GET(GENMASK(31, 31), v));
		seq_printf(m, "   30 EnIB              %lx\n", FIELD_GET(GENMASK(30, 30), v));
		seq_printf(m, "   29 LSMAOE            %lx\n", FIELD_GET(GENMASK(29, 29), v));
		seq_printf(m, "   28 nTLSMD            %lx\n", FIELD_GET(GENMASK(28, 28), v));
		seq_printf(m, "   27 EnDA              %lx\n", FIELD_GET(GENMASK(27, 27), v));
		seq_printf(m, "   26 UCI               %lx\n", FIELD_GET(GENMASK(26, 26), v));
		seq_printf(m, "   25 EE                %lx\n", FIELD_GET(GENMASK(25, 25), v));
		seq_printf(m, "   24 EOE               %lx\n", FIELD_GET(GENMASK(24, 24), v));
		seq_printf(m, "   23 SPAN              %lx\n", FIELD_GET(GENMASK(23, 23), v));
		seq_printf(m, "   22 EIS               %lx\n", FIELD_GET(GENMASK(22, 22), v));
		seq_printf(m, "   21 IESB              %lx\n", FIELD_GET(GENMASK(21, 21), v));
		seq_printf(m, "   20 TSCXT             %lx\n", FIELD_GET(GENMASK(20, 20), v));
		seq_printf(m, "   19 WXN               %lx\n", FIELD_GET(GENMASK(19, 19), v));
		seq_printf(m, "   18 nTWE              %lx\n", FIELD_GET(GENMASK(18, 18), v));
		seq_printf(m, "   17 Res1              %lx\n", FIELD_GET(GENMASK(17, 17), v));
		seq_printf(m, "   16 nTWI              %lx\n", FIELD_GET(GENMASK(16, 16), v));
		seq_printf(m, "   15 UCT               %lx\n", FIELD_GET(GENMASK(15, 15), v));
		seq_printf(m, "   14 DZE               %lx\n", FIELD_GET(GENMASK(14, 14), v));
		seq_printf(m, "   13 EnDB              %lx\n", FIELD_GET(GENMASK(13, 13), v));
		seq_printf(m, "   12 I                 %lx\n", FIELD_GET(GENMASK(12, 12), v));
		seq_printf(m, "   11 EOS               %lx\n", FIELD_GET(GENMASK(11, 11), v));
		seq_printf(m, "   10 EnRCTX            %lx\n", FIELD_GET(GENMASK(10, 10), v));
		seq_printf(m, "   09 Res2               %lx\n", FIELD_GET(GENMASK( 9,  9), v));
		seq_printf(m, "   08 SED               %lx\n", FIELD_GET(GENMASK( 8,  8), v));
		seq_printf(m, "   07 ITD               %lx\n", FIELD_GET(GENMASK( 7,  7), v));
		seq_printf(m, "   06 nAA               %lx\n", FIELD_GET(GENMASK( 6,  6), v));
		seq_printf(m, "   05 CP15BEN           %lx\n", FIELD_GET(GENMASK( 5,  5), v));
		seq_printf(m, "   04 SA0               %lx\n", FIELD_GET(GENMASK( 4,  4), v));
		seq_printf(m, "   03 SA                %lx\n", FIELD_GET(GENMASK( 3,  3), v));
		seq_printf(m, "   02 C                 %lx\n", FIELD_GET(GENMASK( 2,  2), v));
		seq_printf(m, "   01 A                 %lx\n", FIELD_GET(GENMASK( 1,  1), v));
		seq_printf(m, "   00 M                 %lx\n", FIELD_GET(GENMASK( 0,  0), v));
		seq_puts  (m, "\n");

		/* HCR_EL2 */
		v = read_sysreg_s(SYS_HCR_EL2);
		seq_printf(m, "HCR_EL2:                %08x_%08x\n",
			   (unsigned int)(v >> 32), (unsigned int)(v & 0xffffffff));
		seq_puts  (m, "----------------------------------------------\n");
		seq_printf(m, "63:60 TWEDEL            %lx\n", FIELD_GET(GENMASK(63, 60), v));
		seq_printf(m, "   59 TWEDEn            %lx\n", FIELD_GET(GENMASK(59, 59), v));
		seq_printf(m, "   58 TID5              %lx\n", FIELD_GET(GENMASK(58, 58), v));
		seq_printf(m, "   57 DCT               %lx\n", FIELD_GET(GENMASK(57, 57), v));
		seq_printf(m, "   56 ATA               %lx\n", FIELD_GET(GENMASK(56, 56), v));
		seq_printf(m, "   55 TTLBOS            %lx\n", FIELD_GET(GENMASK(55, 55), v));
		seq_printf(m, "   54 TTLBIS            %lx\n", FIELD_GET(GENMASK(54, 54), v));
		seq_printf(m, "   53 EnSCXT            %lx\n", FIELD_GET(GENMASK(53, 53), v));
		seq_printf(m, "   52 TOCU              %lx\n", FIELD_GET(GENMASK(52, 52), v));
		seq_printf(m, "   51 AMVOFFEN          %lx\n", FIELD_GET(GENMASK(51, 51), v));
		seq_printf(m, "   50 TICAB             %lx\n", FIELD_GET(GENMASK(50, 50), v));
		seq_printf(m, "   49 TID4              %lx\n", FIELD_GET(GENMASK(49, 49), v));
		seq_printf(m, "   48 GPF               %lx\n", FIELD_GET(GENMASK(48, 48), v));
		seq_printf(m, "   47 FIEN              %lx\n", FIELD_GET(GENMASK(47, 47), v));
		seq_printf(m, "   46 FWB               %lx\n", FIELD_GET(GENMASK(46, 46), v));
		seq_printf(m, "   45 NV2               %lx\n", FIELD_GET(GENMASK(45, 45), v));
		seq_printf(m, "   44 AT                %lx\n", FIELD_GET(GENMASK(44, 44), v));
		seq_printf(m, "   43 NV1               %lx\n", FIELD_GET(GENMASK(43, 43), v));
		seq_printf(m, "   42 NV                %lx\n", FIELD_GET(GENMASK(42, 42), v));
		seq_printf(m, "   41 API               %lx\n", FIELD_GET(GENMASK(41, 41), v));
		seq_printf(m, "   40 APK               %lx\n", FIELD_GET(GENMASK(40, 40), v));
		seq_printf(m, "   39 TME               %lx\n", FIELD_GET(GENMASK(39, 39), v));
		seq_printf(m, "   38 MIOCNCE           %lx\n", FIELD_GET(GENMASK(38, 38), v));
		seq_printf(m, "   37 TEA               %lx\n", FIELD_GET(GENMASK(37, 37), v));
		seq_printf(m, "   36 TERR              %lx\n", FIELD_GET(GENMASK(36, 36), v));
		seq_printf(m, "   35 TLOR              %lx\n", FIELD_GET(GENMASK(35, 35), v));
		seq_printf(m, "   34 E2H               %lx\n", FIELD_GET(GENMASK(34, 34), v));
		seq_printf(m, "   33 ID                %lx\n", FIELD_GET(GENMASK(33, 33), v));
		seq_printf(m, "   32 CD                %lx\n", FIELD_GET(GENMASK(32, 32), v));
		seq_printf(m, "   31 RW                %lx\n", FIELD_GET(GENMASK(31, 31), v));
		seq_printf(m, "   30 TRVM              %lx\n", FIELD_GET(GENMASK(30, 30), v));
		seq_printf(m, "   29 HCD               %lx\n", FIELD_GET(GENMASK(29, 29), v));
		seq_printf(m, "   28 TDZ               %lx\n", FIELD_GET(GENMASK(28, 28), v));
		seq_printf(m, "   27 TGE               %lx\n", FIELD_GET(GENMASK(27, 27), v));
		seq_printf(m, "   26 TVM               %lx\n", FIELD_GET(GENMASK(26, 26), v));
		seq_printf(m, "   25 TTLB              %lx\n", FIELD_GET(GENMASK(25, 25), v));
		seq_printf(m, "   24 TPU               %lx\n", FIELD_GET(GENMASK(24, 24), v));
		seq_printf(m, "   23 Res0              %lx\n", FIELD_GET(GENMASK(23, 23), v));
		seq_printf(m, "   22 TSW               %lx\n", FIELD_GET(GENMASK(22, 22), v));
		seq_printf(m, "   21 TACR              %lx\n", FIELD_GET(GENMASK(21, 21), v));
		seq_printf(m, "   20 TIDCP             %lx\n", FIELD_GET(GENMASK(20, 20), v));
		seq_printf(m, "   19 TSC               %lx\n", FIELD_GET(GENMASK(19, 19), v));
		seq_printf(m, "   18 TID3              %lx\n", FIELD_GET(GENMASK(18, 18), v));
		seq_printf(m, "   17 TID2              %lx\n", FIELD_GET(GENMASK(17, 17), v));
		seq_printf(m, "   16 TID1              %lx\n", FIELD_GET(GENMASK(16, 16), v));
		seq_printf(m, "   15 TID0              %lx\n", FIELD_GET(GENMASK(15, 15), v));
		seq_printf(m, "   14 TWE               %lx\n", FIELD_GET(GENMASK(14, 14), v));
		seq_printf(m, "   13 TWI               %lx\n", FIELD_GET(GENMASK(13, 13), v));
		seq_printf(m, "   12 DC                %lx\n", FIELD_GET(GENMASK(12, 12), v));
		seq_printf(m, "11:10 BSU               %lx\n", FIELD_GET(GENMASK(11, 10), v));
		seq_printf(m, "   09 FB                %lx\n", FIELD_GET(GENMASK( 9,  9), v));
		seq_printf(m, "   08 VSE               %lx\n", FIELD_GET(GENMASK( 8,  8), v));
		seq_printf(m, "   07 VI                %lx\n", FIELD_GET(GENMASK( 7,  7), v));
		seq_printf(m, "   06 VF                %lx\n", FIELD_GET(GENMASK( 6,  6), v));
		seq_printf(m, "   05 AMO               %lx\n", FIELD_GET(GENMASK( 5,  5), v));
		seq_printf(m, "   04 IMO               %lx\n", FIELD_GET(GENMASK( 4,  4), v));
		seq_printf(m, "   03 FMO               %lx\n", FIELD_GET(GENMASK( 3,  3), v));
		seq_printf(m, "   02 PTW               %lx\n", FIELD_GET(GENMASK( 2,  2), v));
		seq_printf(m, "   01 SWIO              %lx\n", FIELD_GET(GENMASK( 1,  1), v));
		seq_printf(m, "   00 VM                %lx\n", FIELD_GET(GENMASK( 0,  0), v));
		seq_puts  (m, "\n");

		/* VBAR_EL2 */
		v = read_sysreg_s(SYS_VBAR_EL2);
		seq_printf(m, "VBAR_EL2:               %08x_%08x\n",
			   (unsigned int)(v >> 32), (unsigned int)(v & 0xffffffff));
		seq_puts  (m, "----------------------------------------------\n");
		seq_printf(m, "63:11 Base              %lx\n", FIELD_GET(GENMASK(63, 11), v));
		seq_printf(m, "10:00 Res0              %lx\n", FIELD_GET(GENMASK(10,  0), v));
		seq_puts  (m, "\n");

		/* TCR_EL2 */
		v = read_sysreg_s(SYS_TCR_EL2);
		seq_printf(m, "TCR_EL2:                %08x_%08x\n",
			   (unsigned int)(v >> 32), (unsigned int)(v & 0xffffffff));
		seq_puts  (m, "----------------------------------------------\n");
		seq_printf(m, "63:62 Res0              %lx\n", FIELD_GET(GENMASK(63, 62), v));
		seq_printf(m, "   61 MTX1              %lx\n", FIELD_GET(GENMASK(61, 61), v));
		seq_printf(m, "   60 MTX0              %lx\n", FIELD_GET(GENMASK(60, 60), v));
		seq_printf(m, "   59 DS                %lx\n", FIELD_GET(GENMASK(59, 59), v));
		seq_printf(m, "   58 TCMA1             %lx\n", FIELD_GET(GENMASK(58, 58), v));
		seq_printf(m, "   57 TCMA0             %lx\n", FIELD_GET(GENMASK(57, 57), v));
		seq_printf(m, "   56 E0PD1             %lx\n", FIELD_GET(GENMASK(56, 56), v));
		seq_printf(m, "   55 E0PD0             %lx\n", FIELD_GET(GENMASK(55, 55), v));
		seq_printf(m, "   54 NFD1              %lx\n", FIELD_GET(GENMASK(54, 54), v));
		seq_printf(m, "   53 NFD0              %lx\n", FIELD_GET(GENMASK(53, 53), v));
		seq_printf(m, "   52 TBID1             %lx\n", FIELD_GET(GENMASK(52, 52), v));
		seq_printf(m, "   51 TBID0             %lx\n", FIELD_GET(GENMASK(51, 51), v));
		seq_printf(m, "   50 HWU162            %lx\n", FIELD_GET(GENMASK(50, 50), v));
		seq_printf(m, "   49 HWU161            %lx\n", FIELD_GET(GENMASK(49, 49), v));
		seq_printf(m, "   48 HWU160            %lx\n", FIELD_GET(GENMASK(48, 48), v));
		seq_printf(m, "   47 HWU159            %lx\n", FIELD_GET(GENMASK(47, 47), v));
		seq_printf(m, "   46 HWU062            %lx\n", FIELD_GET(GENMASK(46, 46), v));
		seq_printf(m, "   45 HWU061            %lx\n", FIELD_GET(GENMASK(45, 45), v));
		seq_printf(m, "   44 HWU060            %lx\n", FIELD_GET(GENMASK(44, 44), v));
		seq_printf(m, "   43 HWU059            %lx\n", FIELD_GET(GENMASK(43, 43), v));
		seq_printf(m, "   42 HPD1              %lx\n", FIELD_GET(GENMASK(42, 42), v));
		seq_printf(m, "   41 HDP0              %lx\n", FIELD_GET(GENMASK(41, 41), v));
		seq_printf(m, "   40 HD                %lx\n", FIELD_GET(GENMASK(40, 40), v));
		seq_printf(m, "   39 HA                %lx\n", FIELD_GET(GENMASK(39, 39), v));
		seq_printf(m, "   38 TBI1              %lx\n", FIELD_GET(GENMASK(38, 38), v));
		seq_printf(m, "   37 TBI0              %lx\n", FIELD_GET(GENMASK(37, 37), v));
		seq_printf(m, "   36 AS                %lx\n", FIELD_GET(GENMASK(36, 36), v));
		seq_printf(m, "   35 Res0              %lx\n", FIELD_GET(GENMASK(35, 35), v));
		seq_printf(m, "34:32 IPS               %lx\n", FIELD_GET(GENMASK(34, 32), v));
		seq_printf(m, "31:30 TG1               %lx\n", FIELD_GET(GENMASK(31, 30), v));
		seq_printf(m, "29:28 SH1               %lx\n", FIELD_GET(GENMASK(29, 28), v));
		seq_printf(m, "27:26 ORGN1             %lx\n", FIELD_GET(GENMASK(27, 26), v));
		seq_printf(m, "25:24 IRGN1             %lx\n", FIELD_GET(GENMASK(25, 24), v));
		seq_printf(m, "   23 EPD1              %lx\n", FIELD_GET(GENMASK(23, 23), v));
		seq_printf(m, "   22 A1                %lx\n", FIELD_GET(GENMASK(22, 22), v));
		seq_printf(m, "21:16 T1SZ              %lx\n", FIELD_GET(GENMASK(21, 16), v));
		seq_printf(m, "15:14 TG0               %lx\n", FIELD_GET(GENMASK(15, 14), v));
		seq_printf(m, "13:12 SH0               %lx\n", FIELD_GET(GENMASK(13, 12), v));
		seq_printf(m, "11:10 ORGN0             %lx\n", FIELD_GET(GENMASK(11, 10), v));
		seq_printf(m, "09:08 IRGN0             %lx\n", FIELD_GET(GENMASK( 9,  8), v));
		seq_printf(m, "   07 EPD0              %lx\n", FIELD_GET(GENMASK( 7,  7), v));
		seq_printf(m, "   06 Res1              %lx\n", FIELD_GET(GENMASK( 6,  6), v));
		seq_printf(m, "05:00 T0SZ              %lx\n", FIELD_GET(GENMASK( 5,  0), v));
		seq_puts  (m, "\n");

		/* TTBR0_EL2 */
		v = read_sysreg_s(SYS_TTBR0_EL2);
		seq_printf(m, "TTBR0_EL2:              %08x_%08x\n",
			   (unsigned int)(v >> 32), (unsigned int)(v & 0xffffffff));
		seq_puts  (m, "----------------------------------------------\n");
		seq_printf(m, "63:48 ASID              %lx\n", FIELD_GET(GENMASK(63, 48), v));
		seq_printf(m, "47:01 BADDR[47:1]       %lx\n", FIELD_GET(GENMASK(47,  1), v));
		seq_printf(m, "   00 CnP               %lx\n", FIELD_GET(GENMASK( 0,  0), v));
		seq_puts  (m, "\n");

		/* TTBR1_EL2 */
		v = read_sysreg_s(SYS_TTBR1_EL2);
		seq_printf(m, "TTBR1_EL2:              %08x_%08x\n",
			   (unsigned int)(v >> 32), (unsigned int)(v & 0xffffffff));
		seq_puts  (m, "----------------------------------------------\n");
		seq_printf(m, "63:48 ASID              %lx\n", FIELD_GET(GENMASK(63, 48), v));
		seq_printf(m, "47:01 BADDR[47:1]       %lx\n", FIELD_GET(GENMASK(47,  1), v));
		seq_printf(m, "   00 CnP               %lx\n", FIELD_GET(GENMASK( 0,  0), v));
		seq_puts  (m, "\n");

	}
}

/*
 * Some of the system registers may be not defined by the Linux
 * kernel, so we need to define them.
 */
#ifndef SYS_ID_AA64PFR2_EL1
#define SYS_ID_AA64PFR2_EL1	sys_reg(3, 0, 0, 4, 2)
#endif
#ifndef SYS_ID_AA64FPFR0_EL1
#define SYS_ID_AA64FPFR0_EL1	sys_reg(3, 0, 0, 4, 7)
#endif
#ifndef SYS_ID_AA64ISAR3_EL1
#define SYS_ID_AA64ISAR3_EL1	sys_reg(3, 0, 0, 6, 3)
#endif
#ifndef SYS_ID_AA64MMFR4_EL1
#define SYS_ID_AA64MMFR4_EL1	sys_reg(3, 0, 0, 7, 4)
#endif

static void dump_show_feature_register(struct seq_file *m)
{
	unsigned long v;

	/* ID_AA64PFR0_EL1  3 0 0 4 0: AArch64 Processor Feature Register 0 */
	v = read_sysreg_s(SYS_ID_AA64PFR0_EL1);
	seq_printf(m, "ID_AA64PFR0_EL1:        %08x_%08x\n",
		   (unsigned int)(v >> 32), (unsigned int)(v & 0xffffffff));
	seq_puts  (m, "----------------------------------------------\n");
	seq_printf(m, "63:60 CSV3              %lx\n", FIELD_GET(GENMASK(63, 60), v));
	seq_printf(m, "59:56 CSV2              %lx\n", FIELD_GET(GENMASK(59, 56), v));
	seq_printf(m, "55:52 RME               %lx\n", FIELD_GET(GENMASK(55, 52), v));
	seq_printf(m, "51:48 DIT               %lx\n", FIELD_GET(GENMASK(51, 48), v));
	seq_printf(m, "47:44 AMU               %lx\n", FIELD_GET(GENMASK(47, 44), v));
	seq_printf(m, "43:40 MPAM              %lx\n", FIELD_GET(GENMASK(43, 40), v));
	seq_printf(m, "39:36 SEL2              %lx\n", FIELD_GET(GENMASK(39, 36), v));
	seq_printf(m, "35:32 SVE               %lx\n", FIELD_GET(GENMASK(35, 32), v));
	seq_printf(m, "31:28 RAS               %lx\n", FIELD_GET(GENMASK(31, 28), v));
	seq_printf(m, "27:24 GIC               %lx\n", FIELD_GET(GENMASK(27, 24), v));
	seq_printf(m, "23:20 AdvSIMD           %lx\n", FIELD_GET(GENMASK(23, 20), v));
	seq_printf(m, "19:16 FP                %lx\n", FIELD_GET(GENMASK(19, 16), v));
	seq_printf(m, "15:12 EL3               %lx\n", FIELD_GET(GENMASK(15, 12), v));
	seq_printf(m, "11:08 EL2               %lx\n", FIELD_GET(GENMASK(11,  8), v));
	seq_printf(m, "07:04 EL1               %lx\n", FIELD_GET(GENMASK( 7,  4), v));
	seq_printf(m, "03:00 EL0               %lx\n", FIELD_GET(GENMASK( 3,  0), v));
	seq_puts  (m, "\n");

	/* ID_AA64PFR1_EL1  3 0 0 4 1: AArch64 Processor Feature Register 1 */
	v = read_sysreg_s(SYS_ID_AA64PFR1_EL1);
	seq_printf(m, "ID_AA64PFR1_EL1:        %08x_%08x\n",
		   (unsigned int)(v >> 32), (unsigned int)(v & 0xffffffff));
	seq_puts  (m, "----------------------------------------------\n");
	seq_printf(m, "63:60 PFAR              %lx\n", FIELD_GET(GENMASK(63, 60), v));
	seq_printf(m, "59:56 DF2               %lx\n", FIELD_GET(GENMASK(59, 56), v));
	seq_printf(m, "55:52 MTEX              %lx\n", FIELD_GET(GENMASK(55, 52), v));
	seq_printf(m, "51:48 THE               %lx\n", FIELD_GET(GENMASK(51, 48), v));
	seq_printf(m, "47:44 GCS               %lx\n", FIELD_GET(GENMASK(47, 44), v));
	seq_printf(m, "43:40 MTE_frac          %lx\n", FIELD_GET(GENMASK(43, 40), v));
	seq_printf(m, "39:36 NMI               %lx\n", FIELD_GET(GENMASK(39, 36), v));
	seq_printf(m, "35:32 CSV2_frac         %lx\n", FIELD_GET(GENMASK(35, 32), v));
	seq_printf(m, "31:28 RNDR_trap         %lx\n", FIELD_GET(GENMASK(31, 28), v));
	seq_printf(m, "27:24 SME               %lx\n", FIELD_GET(GENMASK(27, 24), v));
	seq_printf(m, "23:20 Res0              %lx\n", FIELD_GET(GENMASK(23, 20), v));
	seq_printf(m, "19:16 MPAM_frac         %lx\n", FIELD_GET(GENMASK(19, 16), v));
	seq_printf(m, "15:12 RAS_frac          %lx\n", FIELD_GET(GENMASK(15, 12), v));
	seq_printf(m, "11:08 MTE               %lx\n", FIELD_GET(GENMASK(11,  8), v));
	seq_printf(m, "07:04 SSBS              %lx\n", FIELD_GET(GENMASK( 7,  4), v));
	seq_printf(m, "03:00 BT                %lx\n", FIELD_GET(GENMASK( 3,  0), v));
	seq_puts  (m, "\n");

	/* ID_AA64PFR2_EL1  3 0 0 4 2: AArch64 Processor Feature Register 2 */
	v = read_sysreg_s(SYS_ID_AA64PFR2_EL1);
	seq_printf(m, "ID_AA64PFR2_EL1:        %08x_%08x\n",
		   (unsigned int)(v >> 32), (unsigned int)(v & 0xffffffff));
	seq_puts  (m, "----------------------------------------------\n");
	seq_printf(m, "63:36 Res0              %lx\n", FIELD_GET(GENMASK(63, 36), v));
	seq_printf(m, "35:32 FPMR              %lx\n", FIELD_GET(GENMASK(35, 32), v));
	seq_printf(m, "31:12 Res1              %lx\n", FIELD_GET(GENMASK(31, 12), v));
	seq_printf(m, "11:08 MTEFAR            %lx\n", FIELD_GET(GENMASK(11,  8), v));
	seq_printf(m, "07:04 MTESTOREONLY      %lx\n", FIELD_GET(GENMASK( 7,  4), v));
	seq_printf(m, "03:00 MTEPERM           %lx\n", FIELD_GET(GENMASK( 3,   0), v));
	seq_puts  (m, "\n");

	/* ID_AA64ZFR0_EL1  3 0 0 4 4: AArch64 SVE Feature Register 0 */
	v = read_sysreg_s(SYS_ID_AA64ZFR0_EL1);
	seq_printf(m, "ID_AA64ZFR0_EL1:        %08x_%08x\n",
		   (unsigned int)(v >> 32), (unsigned int)(v & 0xffffffff));
	seq_puts  (m, "----------------------------------------------\n");
	seq_printf(m, "63:60 Res0              %lx\n", FIELD_GET(GENMASK(63, 60), v));
	seq_printf(m, "59:56 F64MM             %lx\n", FIELD_GET(GENMASK(59, 56), v));
	seq_printf(m, "55:52 F32MM             %lx\n", FIELD_GET(GENMASK(55, 52), v));
	seq_printf(m, "51:48 Res1              %lx\n", FIELD_GET(GENMASK(51, 48), v));
	seq_printf(m, "47:44 I8MM              %lx\n", FIELD_GET(GENMASK(47, 44), v));
	seq_printf(m, "43:40 SM4               %lx\n", FIELD_GET(GENMASK(43, 40), v));
	seq_printf(m, "39:36 Res2              %lx\n", FIELD_GET(GENMASK(39, 36), v));
	seq_printf(m, "35:32 SHA3              %lx\n", FIELD_GET(GENMASK(35, 32), v));
	seq_printf(m, "31:28 Res3              %lx\n", FIELD_GET(GENMASK(31, 28), v));
	seq_printf(m, "27:24 B16B16            %lx\n", FIELD_GET(GENMASK(27, 24), v));
	seq_printf(m, "23:20 BF16              %lx\n", FIELD_GET(GENMASK(23, 20), v));
	seq_printf(m, "19:16 BitPerm           %lx\n", FIELD_GET(GENMASK(19, 16), v));
	seq_printf(m, "15:08 Res4              %lx\n", FIELD_GET(GENMASK(15,  8), v));
	seq_printf(m, "07:04 AES               %lx\n", FIELD_GET(GENMASK( 7,  4), v));
	seq_printf(m, "03:00 SVEver            %lx\n", FIELD_GET(GENMASK( 3,  0), v));
	seq_puts  (m, "\n");

	/* ID_AA64SMFR0_EL1  3 0 0 4 5: AArch64 SME Feature Register 0 */
	v = read_sysreg_s(SYS_ID_AA64SMFR0_EL1);
	seq_printf(m, "ID_AA64SMFR0_EL1:       %08x_%08x\n",
		   (unsigned int)(v >> 32), (unsigned int)(v & 0xffffffff));
	seq_puts  (m, "----------------------------------------------\n");
	seq_printf(m, "   63 FA64              %lx\n", FIELD_GET(GENMASK(63, 63), v));
	seq_printf(m, "62:61 Res0              %lx\n", FIELD_GET(GENMASK(62, 61), v));
	seq_printf(m, "   60 LUTv2             %lx\n", FIELD_GET(GENMASK(60, 60), v));
	seq_printf(m, "59:56 SMEver            %lx\n", FIELD_GET(GENMASK(59, 56), v));
	seq_printf(m, "55:52 I16T64            %lx\n", FIELD_GET(GENMASK(55, 52), v));
	seq_printf(m, "51:49 Res1              %lx\n", FIELD_GET(GENMASK(51, 49), v));
	seq_printf(m, "   48 F64F64            %lx\n", FIELD_GET(GENMASK(48, 48), v));
	seq_printf(m, "47:44 I16I32            %lx\n", FIELD_GET(GENMASK(47, 44), v));
	seq_printf(m, "   43 B16B16            %lx\n", FIELD_GET(GENMASK(43, 43), v));
	seq_printf(m, "   42 F16F16            %lx\n", FIELD_GET(GENMASK(42, 42), v));
	seq_printf(m, "   41 F8F16             %lx\n", FIELD_GET(GENMASK(41, 41), v));
	seq_printf(m, "   40 F8F32             %lx\n", FIELD_GET(GENMASK(40, 40), v));
	seq_printf(m, "39:36 I8I32             %lx\n", FIELD_GET(GENMASK(39, 36), v));
	seq_printf(m, "   35 F16F32            %lx\n", FIELD_GET(GENMASK(35, 35), v));
	seq_printf(m, "   34 B16F32            %lx\n", FIELD_GET(GENMASK(34, 34), v));
	seq_printf(m, "   33 BI32I32           %lx\n", FIELD_GET(GENMASK(33, 33), v));
	seq_printf(m, "   32 F32F32            %lx\n", FIELD_GET(GENMASK(32, 32), v));
	seq_printf(m, "   31 Res2              %lx\n", FIELD_GET(GENMASK(31, 31), v));
	seq_printf(m, "   30 SF8FMA            %lx\n", FIELD_GET(GENMASK(30, 30), v));
	seq_printf(m, "   29 SF8DP4            %lx\n", FIELD_GET(GENMASK(29, 29), v));
	seq_printf(m, "   28 SF8DP2            %lx\n", FIELD_GET(GENMASK(28, 28), v));
	seq_printf(m, "27:00 Res3              %lx\n", FIELD_GET(GENMASK(27,  0), v));
	seq_puts  (m, "\n");

	/* ID_AA64FPFR0_EL1  3 0 0 4 7: AArch64 FloatPoint Feature Register 0 */
	v = read_sysreg_s(SYS_ID_AA64FPFR0_EL1);
	seq_printf(m, "ID_AA64FPFR0_EL1:       %08x_%08x\n",
		   (unsigned int)(v >> 32), (unsigned int)(v & 0xffffffff));
	seq_puts  (m, "----------------------------------------------\n");
	seq_printf(m, "63:32 Res0              %lx\n", FIELD_GET(GENMASK(63, 60), v));
	seq_printf(m, "   31 F8CVT             %lx\n", FIELD_GET(GENMASK(31, 31), v));
	seq_printf(m, "   30 F8FMA             %lx\n", FIELD_GET(GENMASK(30, 30), v));
	seq_printf(m, "   29 F8DP4             %lx\n", FIELD_GET(GENMASK(29, 29), v));
	seq_printf(m, "   28 F8DP2             %lx\n", FIELD_GET(GENMASK(28, 28), v));
	seq_printf(m, "27:02 Res1              %lx\n", FIELD_GET(GENMASK(27,  2), v));
	seq_printf(m, "   01 F8E4M3            %lx\n", FIELD_GET(GENMASK( 1,  1), v));
	seq_printf(m, "   00 F8E5M2            %lx\n", FIELD_GET(GENMASK( 0,  0), v));
	seq_puts  (m, "\n");

	/* ID_AA64DFR0_EL1  3 0 0 5 0: AArch64 Debug Feature Register 0 */
	v = read_sysreg_s(SYS_ID_AA64DFR0_EL1);
	seq_printf(m, "ID_AA64DFR0_EL1:        %08x_%08x\n",
		   (unsigned int)(v >> 32), (unsigned int)(v & 0xffffffff));
	seq_puts  (m, "----------------------------------------------\n");
	seq_printf(m, "63:60 HPMN0             %lx\n", FIELD_GET(GENMASK(63, 60), v));
	seq_printf(m, "59:56 ExtTrcBuff        %lx\n", FIELD_GET(GENMASK(59, 56), v));
	seq_printf(m, "55:52 BRBE              %lx\n", FIELD_GET(GENMASK(55, 52), v));
	seq_printf(m, "51:48 MTPMU             %lx\n", FIELD_GET(GENMASK(51, 48), v));
	seq_printf(m, "47:44 TraceBuffer       %lx\n", FIELD_GET(GENMASK(47, 44), v));
	seq_printf(m, "43:40 TraceFilt         %lx\n", FIELD_GET(GENMASK(43, 40), v));
	seq_printf(m, "39:36 DoubleLock        %lx\n", FIELD_GET(GENMASK(39, 36), v));
	seq_printf(m, "35:32 PMSVer            %lx\n", FIELD_GET(GENMASK(35, 32), v));
	seq_printf(m, "31:28 CTX_CMPs          %lx\n", FIELD_GET(GENMASK(31, 28), v));
	seq_printf(m, "27:24 Res0              %lx\n", FIELD_GET(GENMASK(27, 24), v));
	seq_printf(m, "23:20 WRPs              %lx\n", FIELD_GET(GENMASK(23, 20), v));
	seq_printf(m, "19:16 Res1              %lx\n", FIELD_GET(GENMASK(19, 16), v));
	seq_printf(m, "15:12 BRPs              %lx\n", FIELD_GET(GENMASK(15, 12), v));
	seq_printf(m, "11:08 PMUVer            %lx\n", FIELD_GET(GENMASK(11,  8), v));
	seq_printf(m, "07:04 TraceVer          %lx\n", FIELD_GET(GENMASK( 7,  4), v));
	seq_printf(m, "03:00 DebugVer          %lx\n", FIELD_GET(GENMASK( 3,  0), v));
	seq_puts  (m, "\n");

	/* ID_AA64DFR1_EL1  3 0 0 5 1: AArch64 Debug Feature Register 1 */
	v = read_sysreg_s(SYS_ID_AA64DFR1_EL1);
	seq_printf(m, "ID_AA64DFR1_EL1:        %08x_%08x\n",
		   (unsigned int)(v >> 32), (unsigned int)(v & 0xffffffff));
	seq_puts  (m, "----------------------------------------------\n");
	seq_printf(m, "63:56 ABL_CMPs          %lx\n", FIELD_GET(GENMASK(63, 56), v));
	seq_printf(m, "55:52 DPFZS             %lx\n", FIELD_GET(GENMASK(55, 52), v));
	seq_printf(m, "51:48 EBEP              %lx\n", FIELD_GET(GENMASK(51, 48), v));
	seq_printf(m, "47:44 ITE               %lx\n", FIELD_GET(GENMASK(47, 44), v));
	seq_printf(m, "43:40 ABLE              %lx\n", FIELD_GET(GENMASK(43, 40), v));
	seq_printf(m, "39:36 PMICNTR           %lx\n", FIELD_GET(GENMASK(39, 36), v));
	seq_printf(m, "35:32 SPMU              %lx\n", FIELD_GET(GENMASK(35, 32), v));
	seq_printf(m, "31:24 CTX_CMPs          %lx\n", FIELD_GET(GENMASK(31, 24), v));
	seq_printf(m, "23:16 WRPs              %lx\n", FIELD_GET(GENMASK(23, 16), v));
	seq_printf(m, "15:08 BRPs              %lx\n", FIELD_GET(GENMASK(15,  8), v));
	seq_printf(m, "07:00 SYSPMUID          %lx\n", FIELD_GET(GENMASK( 7,  0), v));
	seq_puts  (m, "\n");

	/* ID_AA64AFR0_EL1  3 0 0 5 4: AArch64 Auxiliary Feature Register 0 */
	v = read_sysreg_s(SYS_ID_AA64AFR0_EL1);
	seq_printf(m, "ID_AA64AFR0_EL1:        %08x_%08x\n",
		   (unsigned int)(v >> 32), (unsigned int)(v & 0xffffffff));
	seq_puts  (m, "----------------------------------------------\n");
	seq_printf(m, "63:32 Res0              %lx\n", FIELD_GET(GENMASK(63, 32), v));
	seq_printf(m, "31:28 IMPDEF7           %lx\n", FIELD_GET(GENMASK(31, 28), v));
	seq_printf(m, "27:24 IMPDEF6           %lx\n", FIELD_GET(GENMASK(27, 24), v));
	seq_printf(m, "23:20 IMPDEF5           %lx\n", FIELD_GET(GENMASK(23, 20), v));
	seq_printf(m, "19:16 IMPDEF4           %lx\n", FIELD_GET(GENMASK(19, 16), v));
	seq_printf(m, "15:12 IMPDEF3           %lx\n", FIELD_GET(GENMASK(15, 12), v));
	seq_printf(m, "11:08 IMPDEF2           %lx\n", FIELD_GET(GENMASK(11,  8), v));
	seq_printf(m, "07:04 IMPDEF1           %lx\n", FIELD_GET(GENMASK( 7,  4), v));
	seq_printf(m, "03:00 IMPDEF0           %lx\n", FIELD_GET(GENMASK( 3,  0), v));
	seq_puts  (m, "\n");

	/* ID_AA64AFR1_EL1  3 0 0 5 5: AArch64 Auxiliary Feature Register 1 */
	v = read_sysreg_s(SYS_ID_AA64AFR1_EL1);
	seq_printf(m, "ID_AA64AFR1_EL1:        %08x_%08x\n",
		   (unsigned int)(v >> 32), (unsigned int)(v & 0xffffffff));
	seq_puts  (m, "----------------------------------------------\n");
	seq_printf(m, "63:00 Res0              %lx\n", FIELD_GET(GENMASK(63,  0), v));
	seq_puts  (m, "\n");

	/* ID_AA64ISAR0_EL1  3 0 0 6 0: AArch64 Instruction Set Attribute Register 0 */
	v = read_sysreg_s(SYS_ID_AA64ISAR0_EL1);
	seq_printf(m, "ID_AA64ISAR0_EL1:       %08x_%08x\n",
		   (unsigned int)(v >> 32), (unsigned int)(v & 0xffffffff));
	seq_puts  (m, "----------------------------------------------\n");
	seq_printf(m, "63:60 RNDR              %lx\n", FIELD_GET(GENMASK(63, 60), v));
	seq_printf(m, "59:56 TLB               %lx\n", FIELD_GET(GENMASK(59, 56), v));
	seq_printf(m, "55:52 TS                %lx\n", FIELD_GET(GENMASK(55, 52), v));
	seq_printf(m, "51:48 FHM               %lx\n", FIELD_GET(GENMASK(51, 48), v));
	seq_printf(m, "47:44 DP                %lx\n", FIELD_GET(GENMASK(47, 44), v));
	seq_printf(m, "43:40 SM4               %lx\n", FIELD_GET(GENMASK(43, 40), v));
	seq_printf(m, "39:36 SM3               %lx\n", FIELD_GET(GENMASK(39, 36), v));
	seq_printf(m, "35:32 SHA3              %lx\n", FIELD_GET(GENMASK(35, 32), v));
	seq_printf(m, "31:28 RDM               %lx\n", FIELD_GET(GENMASK(31, 28), v));
	seq_printf(m, "27:24 TME               %lx\n", FIELD_GET(GENMASK(27, 24), v));
	seq_printf(m, "23:20 ATOMIC            %lx\n", FIELD_GET(GENMASK(23, 20), v));
	seq_printf(m, "19:16 CRC32             %lx\n", FIELD_GET(GENMASK(19, 16), v));
	seq_printf(m, "15:12 SHA2              %lx\n", FIELD_GET(GENMASK(15, 12), v));
	seq_printf(m, "11:08 SHA1              %lx\n", FIELD_GET(GENMASK(11,  8), v));
	seq_printf(m, "07:04 AES               %lx\n", FIELD_GET(GENMASK( 7,  4), v));
	seq_printf(m, "03:00 Res0              %lx\n", FIELD_GET(GENMASK( 3,  0), v));
	seq_puts  (m, "\n");

	/* ID_AA64ISAR1_EL1  3 0 0 6 1: AArch64 Instruction Set Attribute Register 1 */
	v = read_sysreg_s(SYS_ID_AA64ISAR1_EL1);
	seq_printf(m, "ID_AA64ISAR1_EL1:       %08x_%08x\n",
		   (unsigned int)(v >> 32), (unsigned int)(v & 0xffffffff));
	seq_puts  (m, "----------------------------------------------\n");
	seq_printf(m, "63:60 LS64              %lx\n", FIELD_GET(GENMASK(63, 60), v));
	seq_printf(m, "59:56 XS                %lx\n", FIELD_GET(GENMASK(59, 56), v));
	seq_printf(m, "55:52 I8MM              %lx\n", FIELD_GET(GENMASK(55, 52), v));
	seq_printf(m, "51:48 DGH               %lx\n", FIELD_GET(GENMASK(51, 48), v));
	seq_printf(m, "47:44 BF16              %lx\n", FIELD_GET(GENMASK(47, 44), v));
	seq_printf(m, "43:40 SPECRES           %lx\n", FIELD_GET(GENMASK(43, 40), v));
	seq_printf(m, "39:36 SB                %lx\n", FIELD_GET(GENMASK(39, 36), v));
	seq_printf(m, "35:32 FRINTTS           %lx\n", FIELD_GET(GENMASK(35, 32), v));
	seq_printf(m, "31:28 GPI               %lx\n", FIELD_GET(GENMASK(31, 28), v));
	seq_printf(m, "27:24 GPA               %lx\n", FIELD_GET(GENMASK(27, 24), v));
	seq_printf(m, "23:20 LRCPC             %lx\n", FIELD_GET(GENMASK(23, 20), v));
	seq_printf(m, "19:16 FCMA              %lx\n", FIELD_GET(GENMASK(19, 16), v));
	seq_printf(m, "15:12 JSCVT             %lx\n", FIELD_GET(GENMASK(15, 12), v));
	seq_printf(m, "11:08 API               %lx\n", FIELD_GET(GENMASK(11,  8), v));
	seq_printf(m, "07:04 APA               %lx\n", FIELD_GET(GENMASK( 7,  4), v));
	seq_printf(m, "03:00 DPB               %lx\n", FIELD_GET(GENMASK( 3,  0), v));
	seq_puts  (m, "\n");

	/* ID_AA64ISAR2_EL1  3 0 0 6 2: AArch64 Instruction Set Attribute Register 2 */
	v = read_sysreg_s(SYS_ID_AA64ISAR2_EL1);
	seq_printf(m, "ID_AA64ISAR2_EL1:       %08x_%08x\n",
		   (unsigned int)(v >> 32), (unsigned int)(v & 0xffffffff));
	seq_puts  (m, "----------------------------------------------\n");
	seq_printf(m, "63:60 ATS1A             %lx\n", FIELD_GET(GENMASK(63, 60), v));
	seq_printf(m, "59:56 LUT               %lx\n", FIELD_GET(GENMASK(59, 56), v));
	seq_printf(m, "55:52 CSSC              %lx\n", FIELD_GET(GENMASK(55, 52), v));
	seq_printf(m, "51:48 RPRFM             %lx\n", FIELD_GET(GENMASK(51, 48), v));
	seq_printf(m, "47:44 Res0              %lx\n", FIELD_GET(GENMASK(47, 44), v));
	seq_printf(m, "43:40 PRFMSLC           %lx\n", FIELD_GET(GENMASK(43, 40), v));
	seq_printf(m, "39:36 SYSINSTR_128      %lx\n", FIELD_GET(GENMASK(39, 36), v));
	seq_printf(m, "35:32 SYSREG_128        %lx\n", FIELD_GET(GENMASK(35, 32), v));
	seq_printf(m, "31:28 CLRBHB            %lx\n", FIELD_GET(GENMASK(31, 28), v));
	seq_printf(m, "27:24 PAC_frac          %lx\n", FIELD_GET(GENMASK(27, 24), v));
	seq_printf(m, "23:20 BC                %lx\n", FIELD_GET(GENMASK(23, 20), v));
	seq_printf(m, "19:16 MOPS              %lx\n", FIELD_GET(GENMASK(19, 16), v));
	seq_printf(m, "15:12 APA3              %lx\n", FIELD_GET(GENMASK(15, 12), v));
	seq_printf(m, "11:08 GPA3              %lx\n", FIELD_GET(GENMASK(11,  8), v));
	seq_printf(m, "07:04 RPRES             %lx\n", FIELD_GET(GENMASK( 7,  4), v));
	seq_printf(m, "03:00 WFxT              %lx\n", FIELD_GET(GENMASK( 3,  0), v));
	seq_puts  (m, "\n");

	/* ID_AA64ISAR3_EL1  3 0 0 6 3: AArch64 Instruction Set Attribute Register 3 */
	v = read_sysreg_s(SYS_ID_AA64ISAR3_EL1);
	seq_printf(m, "ID_AA64ISAR3_EL1:       %08x_%08x\n",
		   (unsigned int)(v >> 32), (unsigned int)(v & 0xffffffff));
	seq_puts  (m, "----------------------------------------------\n");
	seq_printf(m, "63:16 Res0              %lx\n", FIELD_GET(GENMASK(63, 16), v));
	seq_printf(m, "15:12 PACM              %lx\n", FIELD_GET(GENMASK(15, 12), v));
	seq_printf(m, "11:08 TLBIW             %lx\n", FIELD_GET(GENMASK(11,  8), v));
	seq_printf(m, "07:04 FAMINMAX          %lx\n", FIELD_GET(GENMASK( 7,  4), v));
	seq_printf(m, "03:00 CPA               %lx\n", FIELD_GET(GENMASK( 3,  0), v));
	seq_puts  (m, "\n");

	/* ID_AA64MMFR0_EL1  3 0 0 7 0: AArch64 Memory Model Feature Register 0 */
	v = read_sysreg_s(SYS_ID_AA64MMFR0_EL1);
	seq_printf(m, "ID_AA64MMFR0_EL1:       %08x_%08x\n",
		   (unsigned int)(v >> 32), (unsigned int)(v & 0xffffffff));
	seq_puts  (m, "----------------------------------------------\n");
	seq_printf(m, "63:60 ECV               %lx\n", FIELD_GET(GENMASK(63, 60), v));
	seq_printf(m, "59:56 FGT               %lx\n", FIELD_GET(GENMASK(59, 56), v));
	seq_printf(m, "55:48 Res0              %lx\n", FIELD_GET(GENMASK(55, 52), v));
	seq_printf(m, "47:44 EXS               %lx\n", FIELD_GET(GENMASK(47, 44), v));
	seq_printf(m, "43:40 TGRAN4_2          %lx\n", FIELD_GET(GENMASK(43, 40), v));
	seq_printf(m, "39:36 TGRAN64_2         %lx\n", FIELD_GET(GENMASK(39, 36), v));
	seq_printf(m, "35:32 TGRAN16_2         %lx\n", FIELD_GET(GENMASK(35, 32), v));
	seq_printf(m, "31:28 TGRAN4            %lx\n", FIELD_GET(GENMASK(31, 28), v));
	seq_printf(m, "27:24 TGRAN64           %lx\n", FIELD_GET(GENMASK(27, 24), v));
	seq_printf(m, "23:20 TGRAN16           %lx\n", FIELD_GET(GENMASK(23, 20), v));
	seq_printf(m, "19:16 BIGENDEL0         %lx\n", FIELD_GET(GENMASK(19, 16), v));
	seq_printf(m, "15:12 SNSMEM            %lx\n", FIELD_GET(GENMASK(15, 12), v));
	seq_printf(m, "11:08 BIGEND            %lx\n", FIELD_GET(GENMASK(11,  8), v));
	seq_printf(m, "07:04 ASIDBITS          %lx\n", FIELD_GET(GENMASK( 7,  4), v));
	seq_printf(m, "03:00 PARANGE           %lx\n", FIELD_GET(GENMASK( 3,  0), v));
	seq_puts  (m, "\n");

	/* ID_AA64MMFR1_EL1  3 0 0 7 1: AArch64 Memory Model Feature Register 1 */
	v = read_sysreg_s(SYS_ID_AA64MMFR1_EL1);
	seq_printf(m, "ID_AA64MMFR1_EL1:       %08x_%08x\n",
		   (unsigned int)(v >> 32), (unsigned int)(v & 0xffffffff));
	seq_puts  (m, "----------------------------------------------\n");
	seq_printf(m, "63:60 ECBHB             %lx\n", FIELD_GET(GENMASK(63, 60), v));
	seq_printf(m, "59:56 CMOW              %lx\n", FIELD_GET(GENMASK(59, 56), v));
	seq_printf(m, "55:52 TIDCP1            %lx\n", FIELD_GET(GENMASK(55, 52), v));
	seq_printf(m, "51:48 nTLBPA            %lx\n", FIELD_GET(GENMASK(51, 48), v));
	seq_printf(m, "47:44 AFP               %lx\n", FIELD_GET(GENMASK(47, 44), v));
	seq_printf(m, "43:40 HCX               %lx\n", FIELD_GET(GENMASK(43, 40), v));
	seq_printf(m, "39:36 ETS               %lx\n", FIELD_GET(GENMASK(39, 36), v));
	seq_printf(m, "35:32 TWED              %lx\n", FIELD_GET(GENMASK(35, 32), v));
	seq_printf(m, "31:28 XNX               %lx\n", FIELD_GET(GENMASK(31, 28), v));
	seq_printf(m, "27:24 SpecSEI           %lx\n", FIELD_GET(GENMASK(27, 24), v));
	seq_printf(m, "23:20 PAN               %lx\n", FIELD_GET(GENMASK(23, 20), v));
	seq_printf(m, "19:16 LO                %lx\n", FIELD_GET(GENMASK(19, 16), v));
	seq_printf(m, "15:12 HPDS              %lx\n", FIELD_GET(GENMASK(15, 12), v));
	seq_printf(m, "11:08 VH                %lx\n", FIELD_GET(GENMASK(11,  8), v));
	seq_printf(m, "07:04 VMIDBits          %lx\n", FIELD_GET(GENMASK( 7,  4), v));
	seq_printf(m, "03:00 HAFDBS            %lx\n", FIELD_GET(GENMASK( 3,  0), v));
	seq_puts  (m, "\n");

	/* ID_AA64MMFR2_EL1  3 0 0 7 2: AArch64 Memory Model Feature Register 2 */
	v = read_sysreg_s(SYS_ID_AA64MMFR2_EL1);
	seq_printf(m, "ID_AA64MMFR2_EL1:       %08x_%08x\n",
		   (unsigned int)(v >> 32), (unsigned int)(v & 0xffffffff));
	seq_puts  (m, "----------------------------------------------\n");
	seq_printf(m, "63:60 E0PD              %lx\n", FIELD_GET(GENMASK(63, 60), v));
	seq_printf(m, "59:56 EVT               %lx\n", FIELD_GET(GENMASK(59, 56), v));
	seq_printf(m, "55:52 BBM               %lx\n", FIELD_GET(GENMASK(55, 52), v));
	seq_printf(m, "51:48 TTL               %lx\n", FIELD_GET(GENMASK(51, 48), v));
	seq_printf(m, "47:44 Res0              %lx\n", FIELD_GET(GENMASK(47, 44), v));
	seq_printf(m, "43:40 FWB               %lx\n", FIELD_GET(GENMASK(43, 40), v));
	seq_printf(m, "39:36 IDS               %lx\n", FIELD_GET(GENMASK(39, 36), v));
	seq_printf(m, "35:32 AT                %lx\n", FIELD_GET(GENMASK(35, 32), v));
	seq_printf(m, "31:28 ST                %lx\n", FIELD_GET(GENMASK(31, 28), v));
	seq_printf(m, "27:24 NV                %lx\n", FIELD_GET(GENMASK(27, 24), v));
	seq_printf(m, "23:20 CCIDX             %lx\n", FIELD_GET(GENMASK(23, 20), v));
	seq_printf(m, "19:16 VARange           %lx\n", FIELD_GET(GENMASK(19, 16), v));
	seq_printf(m, "15:12 IESB              %lx\n", FIELD_GET(GENMASK(15, 12), v));
	seq_printf(m, "11:08 LSM               %lx\n", FIELD_GET(GENMASK(11,  8), v));
	seq_printf(m, "07:04 UAO               %lx\n", FIELD_GET(GENMASK( 7,  4), v));
	seq_printf(m, "03:00 CnP               %lx\n", FIELD_GET(GENMASK( 3,  0), v));
	seq_puts  (m, "\n");

	/* ID_AA64MMFR3_EL1  3 0 0 7 3: AArch64 Memory Model Feature Register 3 */
	v = read_sysreg_s(SYS_ID_AA64MMFR3_EL1);
	seq_printf(m, "ID_AA64MMFR3_EL1:       %08x_%08x\n",
		   (unsigned int)(v >> 32), (unsigned int)(v & 0xffffffff));
	seq_puts  (m, "----------------------------------------------\n");
	seq_printf(m, "63:60 Spec_FPACC        %lx\n", FIELD_GET(GENMASK(63, 60), v));
	seq_printf(m, "59:56 ADERR             %lx\n", FIELD_GET(GENMASK(59, 56), v));
	seq_printf(m, "55:52 SDERR             %lx\n", FIELD_GET(GENMASK(55, 52), v));
	seq_printf(m, "51:48 Res0              %lx\n", FIELD_GET(GENMASK(51, 48), v));
	seq_printf(m, "47:44 ANERR             %lx\n", FIELD_GET(GENMASK(47, 44), v));
	seq_printf(m, "43:40 SNERR             %lx\n", FIELD_GET(GENMASK(43, 40), v));
	seq_printf(m, "39:36 D128_2            %lx\n", FIELD_GET(GENMASK(39, 36), v));
	seq_printf(m, "35:32 D128              %lx\n", FIELD_GET(GENMASK(35, 32), v));
	seq_printf(m, "31:28 MEC               %lx\n", FIELD_GET(GENMASK(31, 28), v));
	seq_printf(m, "27:24 AIE               %lx\n", FIELD_GET(GENMASK(27, 24), v));
	seq_printf(m, "23:20 S2POE             %lx\n", FIELD_GET(GENMASK(23, 20), v));
	seq_printf(m, "19:16 S1POE             %lx\n", FIELD_GET(GENMASK(19, 16), v));
	seq_printf(m, "15:12 S2PIE             %lx\n", FIELD_GET(GENMASK(15, 12), v));
	seq_printf(m, "11:08 S1PIE             %lx\n", FIELD_GET(GENMASK(11,  8), v));
	seq_printf(m, "07:04 SCTLRX            %lx\n", FIELD_GET(GENMASK( 7,  4), v));
	seq_printf(m, "03:00 TCRX              %lx\n", FIELD_GET(GENMASK( 3,  0), v));
	seq_puts  (m, "\n");

	/* ID_AA64MMFR4_EL1  3 0 0 7 4: AArch64 Memory Model Feature Register 4 */
	v = read_sysreg_s(SYS_ID_AA64MMFR4_EL1);
	seq_printf(m, "ID_AA64MMFR4_EL1:       %08x_%08x\n",
		   (unsigned int)(v >> 32), (unsigned int)(v & 0xffffffff));
	seq_puts  (m, "----------------------------------------------\n");
	seq_printf(m, "63:40 Res0              %lx\n", FIELD_GET(GENMASK(63, 40), v));
	seq_printf(m, "39:36 E3DSE             %lx\n", FIELD_GET(GENMASK(39, 36), v));
	seq_printf(m, "35:28 Res1              %lx\n", FIELD_GET(GENMASK(35, 28), v));
	seq_printf(m, "27:24 E2H0              %lx\n", FIELD_GET(GENMASK(27, 24), v));
	seq_printf(m, "23:20 NV_frac           %lx\n", FIELD_GET(GENMASK(23, 20), v));
	seq_printf(m, "19:16 FGWTE3            %lx\n", FIELD_GET(GENMASK(19, 16), v));
	seq_printf(m, "15:12 HACDBS            %lx\n", FIELD_GET(GENMASK(15, 12), v));
	seq_printf(m, "11:08 ASID2             %lx\n", FIELD_GET(GENMASK(11,  8), v));
	seq_printf(m, "07:04 EIESB             %lx\n", FIELD_GET(GENMASK( 7,  4), v));
	seq_printf(m, "03:00 Res2              %lx\n", FIELD_GET(GENMASK( 3,  0), v));
	seq_puts  (m, "\n");
}

static void dump_show_cache_register(struct seq_file *m)
{
	unsigned long val, val1;
	unsigned int type, i;
	bool has_ccidx;

	seq_puts(m, "\n");

	/* CTR_EL0: Cache Type Register */
	val = read_sysreg(ctr_el0);
	seq_printf(m, "CTR_EL0:         0x%016lx\n", val);
	seq_puts  (m, "-----------------------------------\n");
	seq_printf(m, "37:32 TminLine   0x%lx\n",    FIELD_GET(GENMASK(37, 32), val));
	seq_printf(m, "   29 DIC        0x%lx\n",    FIELD_GET(GENMASK(29, 29), val));
	seq_printf(m, "   28 IDC        0x%lx\n",    FIELD_GET(GENMASK(28, 28), val));
	seq_printf(m, "27:24 CWG        0x%lx\n",    FIELD_GET(GENMASK(27, 24), val));
	seq_printf(m, "23:20 ERG        0x%lx\n",    FIELD_GET(GENMASK(23, 20), val));
	seq_printf(m, "19:16 DminLine   0x%lx\n",    FIELD_GET(GENMASK(19, 16), val));
	seq_printf(m, "15:14 L1Ip       0x%lx\n",    FIELD_GET(GENMASK(15, 14), val));
	seq_printf(m, "03:00 IminLine   0x%lx\n",    FIELD_GET(GENMASK(3, 0), val));
	seq_puts  (m, "\n");

	/* CLIDR_EL1: Cache Level ID Register */
	val = read_sysreg(clidr_el1);
	seq_printf(m, "CLIDR_EL1:       0x%016lx\n", val);
	seq_puts  (m, "-----------------------------------\n");
	seq_printf(m, "46:45 Ttype7     0x%lx\n",    FIELD_GET(GENMASK(46, 45), val));
	seq_printf(m, "44:43 Ttype6     0x%lx\n",    FIELD_GET(GENMASK(44, 43), val));
	seq_printf(m, "42:41 Ttype5     0x%lx\n",    FIELD_GET(GENMASK(42, 41), val));
	seq_printf(m, "40:39 Ttype4     0x%lx\n",    FIELD_GET(GENMASK(40, 39), val));
	seq_printf(m, "38:37 Ttype3     0x%lx\n",    FIELD_GET(GENMASK(38, 37), val));
	seq_printf(m, "36:35 Ttype2     0x%lx\n",    FIELD_GET(GENMASK(36, 35), val));
	seq_printf(m, "34:33 Ttype1     0x%lx\n",    FIELD_GET(GENMASK(34, 33), val));
	seq_printf(m, "32:30 ICB        0x%lx\n",    FIELD_GET(GENMASK(32, 30), val));
	seq_printf(m, "29:27 LoUU       0x%lx\n",    FIELD_GET(GENMASK(29, 27), val));
	seq_printf(m, "26:24 LoC        0x%lx\n",    FIELD_GET(GENMASK(26, 24), val));
	seq_printf(m, "23:21 LoUIS      0x%lx\n",    FIELD_GET(GENMASK(23, 21), val));
	seq_printf(m, "20:18 Ctype7     0x%lx\n",    FIELD_GET(GENMASK(20, 18), val));
	seq_printf(m, "17:15 Ctype6     0x%lx\n",    FIELD_GET(GENMASK(17, 15), val));
	seq_printf(m, "14:12 Ctype5     0x%lx\n",    FIELD_GET(GENMASK(14, 12), val));
	seq_printf(m, "11:09 Ctype4     0x%lx\n",    FIELD_GET(GENMASK(11, 9), val));
	seq_printf(m, "08:06 Ctype3     0x%lx\n",    FIELD_GET(GENMASK(8, 6), val));
	seq_printf(m, "05:03 Ctype2     0x%lx\n",    FIELD_GET(GENMASK(5, 3), val));
	seq_printf(m, "02:00 Ctype1     0x%lx\n",    FIELD_GET(GENMASK(2, 0), val));
	seq_puts  (m, "\n");

	/* FEAT_CCIDX determines CCSIDR_EL1's format */
	val1 = read_sysreg_s(SYS_ID_AA64MMFR2_EL1);
	has_ccidx = FIELD_GET(GENMASK(23, 20), val1) == 0x1 ? true : false;

	for (i = 1; i < 7; i++) {
		type = (unsigned int)((val >> ((i - 1) * 3)) & 0x7);
		if (!(type >= 0x1 && type <= 0x4))
			continue;

		/* CSSELR_EL1: Cache Size Selection Register */
		val1 = FIELD_PREP(GENMASK(3, 1), i - 1);
		write_sysreg(val1, csselr_el1);

		/* CCSIDR_EL1: Current Cache Size ID Register */
		val1 = read_sysreg(ccsidr_el1);
		seq_printf(m, "CCSIDR_EL1_%d:    0x%016lx\n", i, val1);
		seq_puts  (m, "-----------------------------------\n");
		if (!has_ccidx) {
			seq_printf(m, "NumSets:          0x%lx\n",
				   FIELD_GET(GENMASK(27, 13), val1) + 1);
			seq_printf(m, "Associate:        0x%lx\n",
				   FIELD_GET(GENMASK(12, 3), val1) + 1);
			seq_printf(m, "LineSize:         0x%lx bytes\n",
				   1UL << (FIELD_GET(GENMASK(2, 0), val1) + 4));
		} else {
			seq_printf(m, "NumSets:          0x%lx\n",
				   FIELD_GET(GENMASK(55, 32), val1) + 1);
			seq_printf(m, "Associate:        0x%lx\n",
				   FIELD_GET(GENMASK(23, 3), val1) + 1);
			seq_printf(m, "LineSize:         0x%lx bytes\n",
				   1UL << (FIELD_GET(GENMASK(2, 0), val1) + 4));
		}

		seq_puts(m, "\n");
	}

	seq_puts(m, "\n");
}

static unsigned int dump_show_mpam_acpi_table(struct seq_file *m,
					      unsigned long *bases,
					      unsigned int max_entries)
{
	struct acpi_table_header *header = NULL;
	struct acpi_mpam_msc_node *msc = NULL;
	struct acpi_mpam_resource_node *res = NULL;
	char *offset, *end;
	unsigned int i, entry = 0;

	seq_puts(m, "\n");
	acpi_get_table(ACPI_SIG_MPAM, 0, &header);
	if (!header) {
		seq_puts(m, "ACPI_SIG_MPAM not found\n");
		return entry;
	}

	seq_puts  (m, "ACPI MPAM Table Header\n");
	seq_puts  (m, "\n");
	seq_printf(m, "  signature              %c%c%c%c\n",
		   header->signature[3], header->signature[2],
		   header->signature[1], header->signature[0]);
	seq_printf(m, "  length                 %x\n", header->length);
	seq_printf(m, "  revision               %x\n", header->revision);
	seq_printf(m, "  checksum               %x\n", header->checksum);
	seq_printf(m, "  oem_id                 %s\n", header->oem_id);
	seq_printf(m, "  oem_table_id           %s\n", header->oem_table_id);
	seq_printf(m, "  oem_revision           %x\n", header->oem_revision);
	seq_printf(m, "  asl_compiler_id        %s\n", header->asl_compiler_id);
	seq_printf(m, "  asl_compiler_revision  %x\n", header->asl_compiler_revision);
	seq_puts  (m, "\n");

	offset = (char *)header;
	end = offset + header->length;
	offset += sizeof(*header);
	while (offset < end && (end - offset) >= sizeof(*msc)) {
		msc = (struct acpi_mpam_msc_node *)offset;
		if (entry < max_entries) {
			bases[entry] = msc->base_address;
			entry++;
		}

		seq_puts  (m, "ACPI MPAM MSC Node\n");
		seq_puts  (m, "\n");
		seq_printf(m, "  length                        %x\n",
			   msc->length);
		seq_printf(m, "  interface_type                %x\n",
			   msc->interface_type);
		seq_printf(m, "  reserved                      %x\n",
			   msc->reserved);
		seq_printf(m, "  identifier                    %x\n",
			   msc->identifier);
		seq_printf(m, "  base_address                  %llx\n",
			   msc->base_address);
		seq_printf(m, "  mmio_size                     %x\n",
			   msc->mmio_size);
		seq_printf(m, "  overflow_interrupt            %x\n",
			   msc->overflow_interrupt);
		seq_printf(m, "  overflow_interrupt_flags      %x\n",
			   msc->overflow_interrupt_flags);
		seq_printf(m, "  reserved1                     %x\n",
			   msc->reserved1);
		seq_printf(m, "  overflow_interrupt_affinity   %x\n",
			   msc->overflow_interrupt_affinity);
		seq_printf(m, "  error_intrrupt                %x\n",
			   msc->error_interrupt);
		seq_printf(m, "  error_interrupt_flags         %x\n",
			   msc->error_interrupt_flags);
		seq_printf(m, "  reserved2                     %x\n",
			   msc->reserved2);
		seq_printf(m, "  error_interrupt_affinity      %x\n",
			   msc->error_interrupt_affinity);
		seq_printf(m, "  max_nrdy_usec                 %x\n",
			   msc->max_nrdy_usec);
		seq_printf(m, "  hardware_id_linked_device     %llx\n",
			   msc->hardware_id_linked_device);
		seq_printf(m, "  instance_id_linked_device     %x\n",
			   msc->instance_id_linked_device);
		seq_printf(m, "  num_resource_nodes            %x\n",
			   msc->num_resource_nodes);
		seq_puts  (m, "\n");

		/* Dump resource nodes */
		res = (struct acpi_mpam_resource_node *)(offset + sizeof(*msc));
		for (i = 0; i < msc->num_resource_nodes; i++, res++) {
			seq_printf(m, "ACPI MPAM Resource Node [%d]\n", i);
			seq_puts  (m, "\n");
			seq_printf(m, "  identifier                    %x\n",
				   res->identifier);
			seq_printf(m, "  ris_index                     %x\n",
				   res->ris_index);
			seq_printf(m, "  num_functional_deps           %x\n",
				   res->num_functional_deps);
			switch (res->locator_type) {
			case ACPI_MPAM_LOCATION_TYPE_PROCESSOR_CACHE:
				seq_printf(m, "  locator_type                  %s\n",
					   "processor_cache");
				seq_printf(m, "  cache_reference               %llx\n",
					   res->locator.cache_locator.cache_reference);
				break;
			case ACPI_MPAM_LOCATION_TYPE_MEMORY:
				seq_printf(m, "  locator_type                  %s\n",
					   "memory");
				seq_printf(m, "  proximity_domain              %llx\n",
					   res->locator.memory_locator.proximity_domain);
				break;
			case ACPI_MPAM_LOCATION_TYPE_SMMU:
				seq_printf(m, "  locator_type                  %s\n",
					   "smmu");
				seq_printf(m, "  smmu_interface                %llx\n",
					   res->locator.smmu_locator.smmu_interface);
				break;
			case ACPI_MPAM_LOCATION_TYPE_MEMORY_CACHE:
				seq_printf(m, "  locator_type                  %s\n",
					   "memory_cache");
				seq_printf(m, "  level                         %x\n",
					   res->locator.mem_cache_locator.level);
				seq_printf(m, "  reference                     %x\n",
					   res->locator.mem_cache_locator.reference);
				break;
			case ACPI_MPAM_LOCATION_TYPE_ACPI_DEVICE:
				seq_printf(m, "  locator_type                  %s\n",
					   "acpi_device");
				seq_printf(m, "  acpi_hw_id                    %llx\n",
					   res->locator.acpi_locator.acpi_hw_id);
				seq_printf(m, "  acpi_unique_id                %x\n",
					   res->locator.acpi_locator.acpi_unique_id);
				break;
			case ACPI_MPAM_LOCATION_TYPE_INTERCONNECT:
				seq_printf(m, "  locator_type                  %s\n",
					   "interconnect");
				seq_printf(m, "  inter_connect_desc_tbl_off    %llx\n",
					   res->locator.interconnect_ifc_locator.inter_connect_desc_tbl_off);
				break;
			case ACPI_MPAM_LOCATION_TYPE_UNKNOWN:
			default:
				seq_printf(m, "  locator_type                  %s\n",
					   "unknown");
				seq_printf(m, "  descriptor1                   %llx\n",
					   res->locator.generic_locator.descriptor1);
				seq_printf(m, "  descriptor2                   %x\n",
					   res->locator.generic_locator.descriptor2);
			}

			seq_puts  (m, "\n");
		}

		offset += msc->length;
	}

	if (!entry)
		seq_puts(m, "No MSC node found\n");

	return entry;
}

/* MPAM hardware feature bits */
#define MPAMF_IDR_HAS_NFU			BIT(43)
#define MPAMF_IDR_HAS_ENDIS			BIT(42)
#define MPAMF_IDR_HAS_SP4			BIT(41)
#define MPAMF_IDR_HAS_ERR_MSI			BIT(40)
#define MPAMF_IDR_HAS_ESR			BIT(39)
#define MPAMF_IDR_HAS_EXTD_ESR			BIT(38)
#define MPAMF_IDR_NO_IMPL_MSMON			BIT(37)
#define MPAMF_IDR_NO_IMPL_PART			BIT(36)
#define MPAMF_IDR_HAS_RIS			BIT(32)
#define MPAMF_IDR_HAS_PARTID_NRW		BIT(31)
#define MPAMF_IDR_HAS_MSMON			BIT(30)
#define MPAMF_IDR_HAS_IMPL_IDR			BIT(29)
#define MPAMF_IDR_EXT				BIT(28)
#define MPAMF_IDR_HAS_PRI_PART			BIT(27)
#define MPAMF_IDR_HAS_MBW_PART			BIT(26)
#define MPAMF_IDR_HAS_CPOR_PART			BIT(25)
#define MPAMF_IDR_HAS_CCAP_PART			BIT(24)
#define MPAMF_CCAP_IDR_NO_CMAX			BIT(30)
#define MPAMF_CCAP_IDR_HAS_CMIN			BIT(29)
#define MPAMF_CCAP_IDR_HAS_CASSOC		BIT(28)
#define MPAMF_MBW_IDR_HAS_PROP			BIT(13)
#define MPAMF_MBW_IDR_HAS_PBM			BIT(12)
#define MPAMF_MBW_IDR_HAS_MAX			BIT(11)
#define MPAMF_MBW_IDR_HAS_MIN			BIT(10)
#define MPAMF_PRI_IDR_HAS_DSPRI			BIT(16)
#define MPAMF_MSMON_IDR_HAS_LOCAL_CAPT_EVENT	BIT(31)
#define MPAMF_MSMON_IDR_HAS_OFLW_MSI		BIT(29)
#define MPAMF_MSMON_IDR_HAS_OFLOW_SR		BIT(28)
#define MPAMF_MSMON_IDR_MSMON_MBWU		BIT(17)
#define MPAMF_MSMON_IDR_MSMON_CSU		BIT(16)
#define MPAMF_CSUMON_IDR_HAS_CAPTURE		BIT(31)
#define MPAMF_CSUMON_IDR_HAS_OFSR		BIT(26)
#define MPAMF_MBWUMON_IDR_HAS_CAPTURE		BIT(31)
#define MPAMF_MBWUMON_IDR_HAS_LONG		BIT(30)
#define MPAMF_MBWUMON_IDR_HAS_OFSR		BIT(26)

static void dump_show_mpam_hw_register(struct seq_file *m,
				       unsigned long phys,
				       unsigned int idx)
{
	void __iomem *base;
	unsigned long idr;
	unsigned int cpor_idr, ccap_idr, mbw_idr, pri_idr;
	unsigned int msmon_idr, msmon_csu_idr, msmon_mbwu_idr;
	unsigned int count, i;

	base = ioremap(phys, 0x4000);
	if (!base) {
		seq_puts(m, "Unable to map IO region\n");
		return;
	}

	seq_printf(m, "Hardware Registers [index=%02d]\n", idx);
	seq_puts  (m, "\n");
	idr = readl(base + 0x0000);
	if (idr & MPAMF_IDR_EXT)
		idr = readq(base + 0x0000);
	seq_printf(m, "MPAMF_IDR                   %016lx\n", idr);
	seq_puts  (m, "--------------------------------------------\n");
	seq_printf(m, "63:60 Res0                  %lx\n", FIELD_GET(GENMASK(63, 60), idr));
	seq_printf(m, "59:56 RIS_MAX               %lx\n", FIELD_GET(GENMASK(59, 56), idr));
	seq_printf(m, "55:44 Res1                  %lx\n", FIELD_GET(GENMASK(55, 44), idr));
	seq_printf(m, "   43 HAS_NFU               %lx\n", FIELD_GET(GENMASK(43, 43), idr));
	seq_printf(m, "   42 HAS_ENDIS             %lx\n", FIELD_GET(GENMASK(42, 42), idr));
	seq_printf(m, "   41 SP4                   %lx\n", FIELD_GET(GENMASK(41, 41), idr));
	seq_printf(m, "   40 HAS_ERR_MSI           %lx\n", FIELD_GET(GENMASK(40, 40), idr));
	seq_printf(m, "   39 HAS_ESR               %lx\n", FIELD_GET(GENMASK(39, 39), idr));
	seq_printf(m, "   38 HAS_EXTD_ESR          %lx\n", FIELD_GET(GENMASK(38, 38), idr));
	seq_printf(m, "   37 NO_IMPL_MSMON         %lx\n", FIELD_GET(GENMASK(37, 37), idr));
	seq_printf(m, "   36 NO_IMPL_PART          %lx\n", FIELD_GET(GENMASK(36, 36), idr));
	seq_printf(m, "35:33 Res2                  %lx\n", FIELD_GET(GENMASK(35, 33), idr));
	seq_printf(m, "   32 HAS_RIS               %lx\n", FIELD_GET(GENMASK(32, 32), idr));
	seq_printf(m, "   31 HAS_PARTID_NRW        %lx\n", FIELD_GET(GENMASK(31, 31), idr));
	seq_printf(m, "   30 HAS_MSMON             %lx\n", FIELD_GET(GENMASK(30, 30), idr));
	seq_printf(m, "   29 HAS_IMPL_IDR          %lx\n", FIELD_GET(GENMASK(29, 29), idr));
	seq_printf(m, "   28 EXT                   %lx\n", FIELD_GET(GENMASK(28, 28), idr));
	seq_printf(m, "   27 HAS_PRI_PART          %lx\n", FIELD_GET(GENMASK(27, 27), idr));
	seq_printf(m, "   26 HAS_MBW_PART          %lx\n", FIELD_GET(GENMASK(26, 26), idr));
	seq_printf(m, "   25 HAS_CPOR_PART         %lx\n", FIELD_GET(GENMASK(25, 25), idr));
	seq_printf(m, "   24 HAS_CCAP_PART         %lx\n", FIELD_GET(GENMASK(24, 24), idr));
	seq_printf(m, "23:16 PMG_MAX               %lx\n", FIELD_GET(GENMASK(23, 16), idr));
	seq_printf(m, "15:00 PARTID_MAX            %lx\n", FIELD_GET(GENMASK(15,  0), idr));
	seq_puts  (m, "--------------------------------------------\n");

	seq_printf(m, "MPAMF_SIDR                  %08x\n", readl(base + 0x0008));
	seq_printf(m, "MPAM_IIDR                   %08x\n", readl(base + 0x0018));
	seq_printf(m, "MPAM_AIDR                   %08x\n", readl(base + 0x0020));
	if (idr & MPAMF_IDR_HAS_IMPL_IDR) {
		seq_printf(m, "MPAMF_IMPL_IDR              %08x\n",
			   readl(base + 0x0028));
	}

	seq_printf(m, "MPAMCFG_PART_SEL            %08x\n", readl(base + 0x0100));

	/* MPAMF_IDR_HAS_CCAP_PART */
	if (idr & MPAMF_IDR_HAS_CCAP_PART) {
		ccap_idr = readl(base + 0x0038);
		seq_printf(m, "MPAMF_CCAP_IDR              %08x\n", ccap_idr);

		if (!(ccap_idr & MPAMF_CCAP_IDR_NO_CMAX)) {
			seq_printf(m, "MPAMCFG_CMAX                %08x\n",
				   readl(base + 0x0108));
		}

		if (ccap_idr & MPAMF_CCAP_IDR_HAS_CMIN) {
			seq_printf(m, "MPAMCFG_CMIN                %08x\n",
				   readl(base + 0x0110));
		}

		if (ccap_idr & MPAMF_CCAP_IDR_HAS_CASSOC) {
			seq_printf(m, "MPAMCFG_CASSOC              %08x\n",
				   readl(base + 0x0118));
		}
	}

	/* MPAMF_IDR_HAS_CPOR_PART */
	if (idr & MPAMF_IDR_HAS_CPOR_PART) {
		cpor_idr = readl(base + 0x0030);
		seq_printf(m, "MPAMF_CPOR_IDR              %08x\n", cpor_idr);

		count = (cpor_idr & 0xFFFF) / 32;
		for (i = 0; i <= count; i++) {
			seq_printf(m, "MAPMCFG_CPBM_%04d           %08x\n",
				   i, readl(base + 0x1000 + i * 4));
		}
	}

	/* MPAMF_IDR_HAS_MBW_PART */
	if (idr & MPAMF_IDR_HAS_MBW_PART) {
		mbw_idr = readl(base + 0x0040);
		seq_printf(m, "MAPMF_MBW_IDR               %08x\n", mbw_idr);

		if (mbw_idr & MPAMF_MBW_IDR_HAS_PBM) {
			count = ((mbw_idr & 0x1fff0000) >> 16) / 32;
			for (i = 0; i <= count; i++) {
				seq_printf(m, "MPAMCFG_MBW_PBM_%04d        %08x\n",
					   i, readl(base + 0x2000 + i * 4));
			}
		}


		if (mbw_idr & MPAMF_MBW_IDR_HAS_PROP) {
			seq_printf(m, "MPAMCFG_MBW_PROP            %08x\n",
				   readl(base + 0x0500));
		}

		if (mbw_idr & MPAMF_MBW_IDR_HAS_MAX) {
			seq_printf(m, "MPAMCFG_MBW_MAX             %08x\n",
				   readl(base + 0x0208));
		}

		if (mbw_idr & MPAMF_MBW_IDR_HAS_MIN) {
			seq_printf(m, "MPAMCFG_MBW_MIN             %08x\n",
				   readl(base + 0x200));
		}
	}

	/* MPAMF_IDR_HAS_PRI_PART */
	if (idr & MPAMF_IDR_HAS_PRI_PART) {
		pri_idr = readl(base + 0x0048);
		seq_printf(m, "MPAMF_PRI_IDR               %08x\n",
			   pri_idr);
		seq_printf(m, "MPAM_PRI                    %08x\n",
			   readl(base + 0x0400));
	}

	/* MPAMF_IDR_HAS_PARTID_NRW */
	if (idr & MPAMF_IDR_HAS_PARTID_NRW) {
		seq_printf(m, "MPAMF_PARTID_NRW_IDR        %08x\n",
			   readl(base + 0x0050));
		seq_printf(m, "MPAMCFG_INTPARTID           %08x\n",
			   readl(base + 0x0600));
	}

	/* MPAMF_IDR_HAS_ENDIS */
	if (idr & MPAMF_IDR_HAS_ENDIS) {
		seq_printf(m, "MPAMCFG_EN                  %08x\n",
			   readl(base + 0x0300));
		seq_printf(m, "MPAMCFG_DIS                 %08x\n",
			   readl(base + 0x0310));
		seq_printf(m, "MPAMCFG_EN_FLAGS            %08x\n",
			   readl(base + 0x0320));
	}

	/* MPAMF_IDR_HAS_ESR */
	if (idr & MPAMF_IDR_HAS_ESR) {
		seq_printf(m, "MPAMF_ECR                   %08x\n",
			   readl(base + 0x00F0));
		if (idr & MPAMF_IDR_HAS_EXTD_ESR) {
			seq_printf(m, "MPAMF_ESR                   %016llx\n",
				   readq(base + 0x00f8));
		} else {
			seq_printf(m, "MPAMF_ESR                   %08x\n",
				   readl(base + 0x00f8));
		}
	}

	/* MPAMF_IDR_HAS_ERR_MSI */
	if (idr & MPAMF_IDR_HAS_ERR_MSI) {
		seq_printf(m, "MPAMF_ERR_MSI_MPAM          %08x\n",
			   readl(base + 0x00dc));
		seq_printf(m, "MPAMF_ERR_MSI_ADDR_L        %08x\n",
			   readl(base + 0x00e0));
		seq_printf(m, "MPAMF_ERR_MSI_ADDR_H        %08x\n",
			   readl(base + 0x00e4));
		seq_printf(m, "MPAMF_ERR_MSI_DATA          %08x\n",
			   readl(base + 0x00e8));
		seq_printf(m, "MPAMF_ERR_MSI_ATTR          %08x\n",
			   readl(base + 0x00ec));
	}

	/* MPAMF_IDR_HAS_MSMON */
	if (idr & MPAMF_IDR_HAS_MSMON) {
		msmon_idr = readl(base + 0x80);
		seq_printf(m, "MPAMF_MSMON_IDR             %08x\n",
			   msmon_idr);
		seq_printf(m, "MSMON_CFG_MON_SEL           %08x\n",
			   readl(base + 0x0800));

		if (msmon_idr & MPAMF_MSMON_IDR_HAS_LOCAL_CAPT_EVENT) {
			seq_printf(m, "MSMON_CAP_EVENT             %08x\n",
				   readl(base + 0x0808));
		}

		if (msmon_idr & MPAMF_MSMON_IDR_MSMON_CSU) {
			msmon_csu_idr = readl(base + 0x0088);
			seq_printf(m, "MPAMF_CSUMON_IDR            %08x\n",
				   msmon_csu_idr); 
			seq_printf(m, "MSMON_CFG_CSU_FLT           %08x\n",
				   readl(base + 0x0810));
			seq_printf(m, "MSMON_CFG_CSU_CTL           %08x\n",
				   readl(base + 0x0818));
			seq_printf(m, "MSMON_CSU                   %08x\n",
				   readl(base + 0x0840));

			if (msmon_csu_idr & MPAMF_CSUMON_IDR_HAS_CAPTURE) {
				seq_printf(m, "MSMON_CSU_CAPTURE           %08x\n",
					   readl(base + 0x0848));
			}

			if (msmon_csu_idr & MPAMF_CSUMON_IDR_HAS_OFSR) {
				seq_printf(m, "MSMON_CSU_OFSR              %08x\n",
					   readl(base + 0x0858));
			}
		}

		if (msmon_idr & MPAMF_MSMON_IDR_MSMON_MBWU) {
			msmon_mbwu_idr = readl(base + 0x0090);
			seq_printf(m, "MPAMF_MBWUMON_IDR           %08x\n",
				   msmon_mbwu_idr);
			seq_printf(m, "MSMON_CFG_MBWU_FLT          %08x\n",
				   readl(base + 0x0820));
			seq_printf(m, "MSMON_CFG_MBWU_CTL          %08x\n",
				   readl(base + 0x0828));
			seq_printf(m, "MSMON_MBWU                  %08x\n",
				   readl(base + 0x0860));

			if (msmon_mbwu_idr & MPAMF_MBWUMON_IDR_HAS_CAPTURE) {
				seq_printf(m, "MSMON_MBWU_CAPTURE          %08x\n",
					   readl(base + 0x0868));
			}

			seq_printf(m, "MSMON_MBWU_L                %08x\n",
				   readl(base + 0x0880));

			if ((msmon_mbwu_idr & MPAMF_MBWUMON_IDR_HAS_CAPTURE) &&
			    (msmon_mbwu_idr & MPAMF_MBWUMON_IDR_HAS_LONG)) {
				seq_printf(m, "MSMON_MBWU_L_CAPTURE        %08x\n",
					   readl(base + 0x0890));
			}

			if (msmon_mbwu_idr & MPAMF_MBWUMON_IDR_HAS_OFSR) {
				seq_printf(m, "MSMON_MBWU_OFSR             %08x\n",
					   readl(base + 0x0898));
			}
		}

		if (msmon_idr & MPAMF_MSMON_IDR_HAS_OFLW_MSI) {
			seq_printf(m, "MSMON_OFLOW_MSI_MPAM        %08x\n",
				   readl(base + 0x08dc));
			seq_printf(m, "MSMON_OFLOW_MSI_ADDR_L      %08x\n",
				   readl(base + 0x08e0));
			seq_printf(m, "MSMON_OFLOW_MSI_ADDR_H      %08x\n",
				   readl(base + 0x08e4));
			seq_printf(m, "MSMON_OFLOW_MSI_DATA        %08x\n",
				   readl(base + 0x08e8));
			seq_printf(m, "MSMON_OFLOW_MSI_ATTR        %08x\n",
				   readl(base + 0x08ec));
		}

		if (msmon_idr & MPAMF_MSMON_IDR_HAS_OFLOW_SR) {
			seq_printf(m, "MSMON_OFLOW_SR              %08x\n",
				   readl(base + 0x08f0));
		}
	}

	seq_puts  (m, "\n");
	iounmap(base);
}

#define SYS_MPAM0_EL1		sys_reg(3, 0, 10, 5, 1)
#define SYS_MPAM1_EL1		sys_reg(3, 0, 10, 5, 0)
#define SYS_MPAM2_EL2		sys_reg(3, 4, 10, 5, 0)
#define SYS_MPAM3_EL3		sys_reg(3, 6, 10, 5, 0)
#define SYS_MPAMHCR_EL2		sys_reg(3, 4, 10, 4, 0)
#define SYS_MAPMIDR_EL1		sys_reg(3, 0, 10, 4, 4)
#define SYS_MPAMSM_EL1		sys_reg(3, 0, 10, 5, 3)
#ifndef SYS_MPAMVPM0_EL2
#define SYS_MPAMVPM0_EL2	sys_reg(3, 4, 10, 6, 0)
#define SYS_MPAMVPM1_EL2	sys_reg(3, 4, 10, 6, 1)
#define SYS_MPAMVPM2_EL2	sys_reg(3, 4, 10, 6, 2)
#define SYS_MPAMVPM3_EL2	sys_reg(3, 4, 10, 6, 3)
#define SYS_MPAMVPM4_EL2	sys_reg(3, 4, 10, 6, 4)
#define SYS_MPAMVPM5_EL2	sys_reg(3, 4, 10, 6, 5)
#define SYS_MPAMVPM6_EL2	sys_reg(3, 4, 10, 6, 6)
#define SYS_MPAMVPM7_EL2	sys_reg(3, 4, 10, 6, 7)
#endif
#define SYS_MPAMVPMV_EL2	sys_reg(3, 4, 10, 4, 1)

static void dump_show_mpam_cpu_register(struct seq_file *m)
{
	unsigned long major_version, minor_version, v;
	bool has_sme;

	/* Bail if MPAM feature isn't supported */
	v = read_sysreg_s(SYS_ID_AA64PFR0_EL1);
	major_version = FIELD_GET(GENMASK(43, 40), v);
	v = read_sysreg_s(SYS_ID_AA64PFR1_EL1);
	minor_version = FIELD_GET(GENMASK(19, 16), v);
	has_sme = (FIELD_GET(GENMASK(27, 24), v) != 0) ? true : false;
	if (major_version == 0 && minor_version == 0) {
		seq_puts(m, "MPAM feature isn't available\n");
		return;
	}

	seq_puts  (m, "\n");
	seq_printf(m, "MPAM version %ld.%ld  SME: %s\n",
		   major_version, minor_version, has_sme ? "supported" : "unsupported");
	seq_puts  (m, "\n");

	/* MPAM0_EL1  3 0 10 5 1: MPAM0 Register (EL1) */
	v = read_sysreg_s(SYS_MPAM0_EL1);
	seq_printf(m, "MPAM0_EL1:              %08x_%08x\n",
		   (unsigned int)(v >> 32), (unsigned int)(v & 0xffffffff));
	seq_puts  (m, "----------------------------------------------\n");
	seq_printf(m, "63:48 Res0              %lx\n", FIELD_GET(GENMASK(63, 48), v));
	seq_printf(m, "47:40 PMG_D             %lx\n", FIELD_GET(GENMASK(47, 40), v));
	seq_printf(m, "39:32 PMG_I             %lx\n", FIELD_GET(GENMASK(39, 32), v));
	seq_printf(m, "31:16 PARTID_D          %lx\n", FIELD_GET(GENMASK(31, 16), v));
	seq_printf(m, "15:00 PARTID_I          %lx\n", FIELD_GET(GENMASK(15,  0), v));
	seq_puts  (m, "\n");

	/* MPAM1_EL1  3 0 10 5 0: MPAM1 Register (EL1) */
	v = read_sysreg_s(SYS_MPAM1_EL1);
	seq_printf(m, "MPAM1_EL1:              %08x_%08x\n",
		   (unsigned int)(v >> 32), (unsigned int)(v & 0xffffffff));
	seq_puts  (m, "----------------------------------------------\n");
	seq_printf(m, "   63 MPAMEN            %lx\n", FIELD_GET(GENMASK(63, 63), v));
	seq_printf(m, "62:61 Res0              %lx\n", FIELD_GET(GENMASK(62, 61), v));
	seq_printf(m, "   60 FORCED_NS         %lx\n", FIELD_GET(GENMASK(60, 60), v));
	seq_printf(m, "59:55 Res1              %lx\n", FIELD_GET(GENMASK(59, 55), v));
	seq_printf(m, "   54 ALTSP_FRCD        %lx\n", FIELD_GET(GENMASK(54, 54), v));
	seq_printf(m, "53:48 Res2              %lx\n", FIELD_GET(GENMASK(53, 48), v));
	seq_printf(m, "47:40 PMG_D             %lx\n", FIELD_GET(GENMASK(47, 40), v));
	seq_printf(m, "39:32 PMG_I             %lx\n", FIELD_GET(GENMASK(39, 32), v));
	seq_printf(m, "31:16 PARTID_D          %lx\n", FIELD_GET(GENMASK(31, 16), v));
	seq_printf(m, "15:00 PARTID_I          %lx\n", FIELD_GET(GENMASK(15,  0), v));
	seq_puts  (m, "\n");

	/* MPAM2_EL1  3 4 10 5 0: MPAM2 Register (EL2) */
	v = read_sysreg_s(SYS_MPAM2_EL2);
	seq_printf(m, "MPAM2_EL2:              %08x_%08x\n",
		   (unsigned int)(v >> 32), (unsigned int)(v & 0xffffffff));
	seq_puts  (m, "----------------------------------------------\n");
	seq_printf(m, "   63 MPAMEN            %lx\n", FIELD_GET(GENMASK(63, 63), v));
	seq_printf(m, "62:59 Res0              %lx\n", FIELD_GET(GENMASK(62, 59), v));
	seq_printf(m, "   58 TIDR              %lx\n", FIELD_GET(GENMASK(58, 58), v));
	seq_printf(m, "   57 Res1              %lx\n", FIELD_GET(GENMASK(57, 57), v));
	seq_printf(m, "   56 ALTSP_HFC         %lx\n", FIELD_GET(GENMASK(56, 56), v));
	seq_printf(m, "   55 ALTSP_EL2         %lx\n", FIELD_GET(GENMASK(55, 55), v));
	seq_printf(m, "   54 ALTSP_FRCD        %lx\n", FIELD_GET(GENMASK(54, 54), v));
	seq_printf(m, "53:51 Res2              %lx\n", FIELD_GET(GENMASK(53, 51), v));
	seq_printf(m, "   50 EnMPAMSM          %lx\n", FIELD_GET(GENMASK(50, 50), v));
	seq_printf(m, "   49 TRAPMPAM0EL1      %lx\n", FIELD_GET(GENMASK(49, 49), v));
	seq_printf(m, "   48 TRAPMPAM1EL1      %lx\n", FIELD_GET(GENMASK(48, 48), v));
	seq_printf(m, "47:40 PMG_D             %lx\n", FIELD_GET(GENMASK(47, 40), v));
	seq_printf(m, "39:32 PMG_I             %lx\n", FIELD_GET(GENMASK(39, 32), v));
	seq_printf(m, "31:16 PARTID_D          %lx\n", FIELD_GET(GENMASK(31, 16), v));
	seq_printf(m, "15:00 PARTID_I          %lx\n", FIELD_GET(GENMASK(15,  0), v));
	seq_puts  (m, "\n");

	/* MPAM3_EL3  3 6 10 5 0: MPAM3 Regiter (EL3) */
	/* MPAMHCR_EL2  3 4 10 4 0: MPAM Hypervisor Control Register (EL2) */
	v = read_sysreg_s(SYS_MPAMHCR_EL2);
	seq_printf(m, "MPAMHCR_EL2:            %08x_%08x\n",
		   (unsigned int)(v >> 32), (unsigned int)(v & 0xffffffff));
	seq_puts  (m, "----------------------------------------------\n");
	seq_printf(m, "63:32 Res0              %lx\n", FIELD_GET(GENMASK(63, 32), v));
	seq_printf(m, "   31 TRAP_MPAMIDR_EL1  %lx\n", FIELD_GET(GENMASK(31, 31), v));
	seq_printf(m, "30:09 Res1              %lx\n", FIELD_GET(GENMASK(30,  9), v));
	seq_printf(m, "   08 GSTAPP_PLK        %lx\n", FIELD_GET(GENMASK( 8,  8), v));
	seq_printf(m, "07:02 Res2              %lx\n", FIELD_GET(GENMASK( 7,  2), v));
	seq_printf(m, "   01 EL1_VPMEN         %lx\n", FIELD_GET(GENMASK( 1,  1), v));
	seq_printf(m, "   00 EL0_VPMEN         %lx\n", FIELD_GET(GENMASK( 0,  0), v));
	seq_puts  (m, "\n");

	/* MAPMIDR_EL1  3 0 10 4 4: MPAM ID Register (EL1) */
	v = read_sysreg_s(SYS_MAPMIDR_EL1);
	seq_printf(m, "MAPMIDR_EL1:            %08x_%08x\n",
		   (unsigned int)(v >> 32), (unsigned int)(v & 0xffffffff));
	seq_puts  (m, "----------------------------------------------\n");
	seq_printf(m, "63:62 Res0              %lx\n", FIELD_GET(GENMASK(63, 62), v));
	seq_printf(m, "   61 HAS_SDEFLT        %lx\n", FIELD_GET(GENMASK(61, 61), v));
	seq_printf(m, "   60 HAS_FORCE_NS      %lx\n", FIELD_GET(GENMASK(60, 60), v));
	seq_printf(m, "   59 SP4               %lx\n", FIELD_GET(GENMASK(59, 59), v));
	seq_printf(m, "   58 HAS_TIDR          %lx\n", FIELD_GET(GENMASK(58, 58), v));
	seq_printf(m, "   57 HAS_ALTSP         %lx\n", FIELD_GET(GENMASK(57, 57), v));
	seq_printf(m, "56:40 Res1              %lx\n", FIELD_GET(GENMASK(56, 40), v));
	seq_printf(m, "39:32 PMG_MAX           %lx\n", FIELD_GET(GENMASK(39, 32), v));
	seq_printf(m, "31:21 Res2              %lx\n", FIELD_GET(GENMASK(31, 21), v));
	seq_printf(m, "20:18 VPMR_MAX          %lx\n", FIELD_GET(GENMASK(20, 18), v));
	seq_printf(m, "   17 HAS_HCR           %lx\n", FIELD_GET(GENMASK(17, 17), v));
	seq_printf(m, "   16 Res3              %lx\n", FIELD_GET(GENMASK(16, 16), v));
	seq_printf(m, "15:00 PARTID_MAX        %lx\n", FIELD_GET(GENMASK(15,  0), v));
	seq_puts  (m, "\n");

	/* ID_MPAMSM_EL1  3 0 10 5 3: MPAM Streaming Mode Register (EL1)
	 *
	 * Access to the register has undefined behavior if SME isn't supported
	 */
	if (has_sme) {
		v = read_sysreg_s(SYS_MPAMSM_EL1);
		seq_printf(m, "MPAMSM_EL1:            %08x_%08x\n",
			   (unsigned int)(v >> 32), (unsigned int)(v & 0xffffffff));
		seq_puts  (m, "----------------------------------------------\n");
		seq_printf(m, "63:48 Res0              %lx\n", FIELD_GET(GENMASK(63, 48), v));
		seq_printf(m, "47:40 PMG_D             %lx\n", FIELD_GET(GENMASK(47, 40), v));
		seq_printf(m, "39:32 Res1              %lx\n", FIELD_GET(GENMASK(39, 32), v));
		seq_printf(m, "31:16 PARTID_D          %lx\n", FIELD_GET(GENMASK(31, 16), v));
		seq_printf(m, "15:00 Res2              %lx\n", FIELD_GET(GENMASK(15,  0), v));
		seq_puts  (m, "\n");
	}

	/*
	 * MPAMVPM0_EL2  3 4 10 6 0: MPAM Virtual PARTID Mapping Register 0
	 * MPAMVPM1_EL2  3 4 10 6 1: MPAM Virtual PARTID Mapping Register 1
	 * MPAMVPM2_EL2  3 4 10 6 2: MPAM Virtual PARTID Mapping Register 2
	 * MPAMVPM3_EL2  3 4 10 6 3: MPAM Virtual PARTID Mapping Register 3
         * MPAMVPM4_EL2  3 4 10 6 4: MPAM Virtual PARTID Mapping Register 4
         * MPAMVPM5_EL2  3 4 10 6 5: MPAM Virtual PARTID Mapping Register 5
	 * MPAMVPM6_EL2  3 4 10 6 6: MPAM Virtual PARTID Mapping Register 6
         * MPAMVPM7_EL2  3 4 10 6 7: MPAM Virtual PARTID Mapping Register 7
         * MPAMVPMV_EL2  3 4 10 4 1: MPAM Virtual PARTID Mapping Valid Register
	 */
	v = read_sysreg_s(SYS_MPAMVPM7_EL2);
	seq_printf(m, "MPAMVPM7_EL2:           %08x_%08x\n",
		   (unsigned int)(v >> 32), (unsigned int)(v & 0xffffffff));
	seq_puts  (m, "----------------------------------------------\n");
	seq_printf(m, "63:48 PhyPARTID31       %lx\n", FIELD_GET(GENMASK(63, 48), v));
	seq_printf(m, "47:32 PhyPARTID30       %lx\n", FIELD_GET(GENMASK(47, 32), v));
	seq_printf(m, "31:16 PhyPARTID29       %lx\n", FIELD_GET(GENMASK(31, 16), v));
	seq_printf(m, "15:00 PhyPARTID28       %lx\n", FIELD_GET(GENMASK(15,  0), v));
	seq_puts  (m, "\n");

	v = read_sysreg_s(SYS_MPAMVPM6_EL2);
	seq_printf(m, "MPAMVPM6_EL2:           %08x_%08x\n",
		   (unsigned int)(v >> 32), (unsigned int)(v & 0xffffffff));
	seq_puts  (m, "----------------------------------------------\n");
	seq_printf(m, "63:48 PhyPARTID27       %lx\n", FIELD_GET(GENMASK(63, 48), v));
	seq_printf(m, "47:32 PhyPARTID26       %lx\n", FIELD_GET(GENMASK(47, 32), v));
	seq_printf(m, "31:16 PhyPARTID25       %lx\n", FIELD_GET(GENMASK(31, 16), v));
	seq_printf(m, "15:00 PhyPARTID24       %lx\n", FIELD_GET(GENMASK(15,  0), v));
	seq_puts  (m, "\n");

	v = read_sysreg_s(SYS_MPAMVPM5_EL2);
	seq_printf(m, "MPAMVPM5_EL2:           %08x_%08x\n",
		   (unsigned int)(v >> 32), (unsigned int)(v & 0xffffffff));
	seq_puts  (m, "----------------------------------------------\n");
	seq_printf(m, "63:48 PhyPARTID23       %lx\n", FIELD_GET(GENMASK(63, 48), v));
	seq_printf(m, "47:32 PhyPARTID22       %lx\n", FIELD_GET(GENMASK(47, 32), v));
	seq_printf(m, "31:16 PhyPARTID21       %lx\n", FIELD_GET(GENMASK(31, 16), v));
	seq_printf(m, "15:00 PhyPARTID20       %lx\n", FIELD_GET(GENMASK(15,  0), v));
	seq_puts  (m, "\n");

	v = read_sysreg_s(SYS_MPAMVPM4_EL2);
	seq_printf(m, "MPAMVPM4_EL2:           %08x_%08x\n",
		   (unsigned int)(v >> 32), (unsigned int)(v & 0xffffffff));
	seq_puts  (m, "----------------------------------------------\n");
	seq_printf(m, "63:48 PhyPARTID19       %lx\n", FIELD_GET(GENMASK(63, 48), v));
	seq_printf(m, "47:32 PhyPARTID18       %lx\n", FIELD_GET(GENMASK(47, 32), v));
	seq_printf(m, "31:16 PhyPARTID17       %lx\n", FIELD_GET(GENMASK(31, 16), v));
	seq_printf(m, "15:00 PhyPARTID16       %lx\n", FIELD_GET(GENMASK(15,  0), v));
	seq_puts  (m, "\n");

	v = read_sysreg_s(SYS_MPAMVPM3_EL2);
	seq_printf(m, "MPAMVPM3_EL2:           %08x_%08x\n",
		   (unsigned int)(v >> 32), (unsigned int)(v & 0xffffffff));
	seq_puts  (m, "----------------------------------------------\n");
	seq_printf(m, "63:48 PhyPARTID15       %lx\n", FIELD_GET(GENMASK(63, 48), v));
	seq_printf(m, "47:32 PhyPARTID14       %lx\n", FIELD_GET(GENMASK(47, 32), v));
	seq_printf(m, "31:16 PhyPARTID13       %lx\n", FIELD_GET(GENMASK(31, 16), v));
	seq_printf(m, "15:00 PhyPARTID12       %lx\n", FIELD_GET(GENMASK(15,  0), v));
	seq_puts  (m, "\n");

	v = read_sysreg_s(SYS_MPAMVPM2_EL2);
	seq_printf(m, "MPAMVPM7_EL2:           %08x_%08x\n",
		   (unsigned int)(v >> 32), (unsigned int)(v & 0xffffffff));
	seq_puts  (m, "----------------------------------------------\n");
	seq_printf(m, "63:48 PhyPARTID11       %lx\n", FIELD_GET(GENMASK(63, 48), v));
	seq_printf(m, "47:32 PhyPARTID10       %lx\n", FIELD_GET(GENMASK(47, 32), v));
	seq_printf(m, "31:16 PhyPARTID09       %lx\n", FIELD_GET(GENMASK(31, 16), v));
	seq_printf(m, "15:00 PhyPARTID08       %lx\n", FIELD_GET(GENMASK(15,  0), v));
	seq_puts  (m, "\n");

	v = read_sysreg_s(SYS_MPAMVPM1_EL2);
	seq_printf(m, "MPAMVPM1_EL2:           %08x_%08x\n",
		   (unsigned int)(v >> 32), (unsigned int)(v & 0xffffffff));
	seq_puts  (m, "----------------------------------------------\n");
	seq_printf(m, "63:48 PhyPARTID07       %lx\n", FIELD_GET(GENMASK(63, 48), v));
	seq_printf(m, "47:32 PhyPARTID06       %lx\n", FIELD_GET(GENMASK(47, 32), v));
	seq_printf(m, "31:16 PhyPARTID05       %lx\n", FIELD_GET(GENMASK(31, 16), v));
	seq_printf(m, "15:00 PhyPARTID04       %lx\n", FIELD_GET(GENMASK(15,  0), v));
	seq_puts  (m, "\n");

	v = read_sysreg_s(SYS_MPAMVPM0_EL2);
	seq_printf(m, "MPAMVPM0_EL2:           %08x_%08x\n",
		   (unsigned int)(v >> 32), (unsigned int)(v & 0xffffffff));
	seq_puts  (m, "----------------------------------------------\n");
	seq_printf(m, "63:48 PhyPARTID03       %lx\n", FIELD_GET(GENMASK(63, 48), v));
	seq_printf(m, "47:32 PhyPARTID02       %lx\n", FIELD_GET(GENMASK(47, 32), v));
	seq_printf(m, "31:16 PhyPARTID01       %lx\n", FIELD_GET(GENMASK(31, 16), v));
	seq_printf(m, "15:00 PhyPARTID00       %lx\n", FIELD_GET(GENMASK(15,  0), v));
	seq_puts  (m, "\n");

	v = read_sysreg_s(SYS_MPAMVPMV_EL2);
	seq_printf(m, "MPAMVPMV_EL2:           %08x_%08x\n",
		   (unsigned int)(v >> 32), (unsigned int)(v & 0xffffffff));
	seq_puts  (m, "----------------------------------------------\n");
	seq_printf(m, "63:32 Res0              %lx\n", FIELD_GET(GENMASK(63, 48), v));
	seq_printf(m, "   31 VPM_v31           %lx\n", FIELD_GET(GENMASK(31, 31), v));
	seq_printf(m, "   30 VPM_v30           %lx\n", FIELD_GET(GENMASK(30, 30), v));
	seq_printf(m, "   29 VPM_v29           %lx\n", FIELD_GET(GENMASK(29, 29), v));
	seq_printf(m, "   28 VPM_v28           %lx\n", FIELD_GET(GENMASK(28, 28), v));
	seq_printf(m, "   27 VPM_v27           %lx\n", FIELD_GET(GENMASK(27, 27), v));
	seq_printf(m, "   26 VPM_v26           %lx\n", FIELD_GET(GENMASK(26, 26), v));
	seq_printf(m, "   25 VPM_v25           %lx\n", FIELD_GET(GENMASK(25, 25), v));
	seq_printf(m, "   24 VPM_v24           %lx\n", FIELD_GET(GENMASK(24, 24), v));
	seq_printf(m, "   23 VPM_v23           %lx\n", FIELD_GET(GENMASK(23, 23), v));
	seq_printf(m, "   22 VPM_v22           %lx\n", FIELD_GET(GENMASK(22, 22), v));
	seq_printf(m, "   21 VPM_v21           %lx\n", FIELD_GET(GENMASK(21, 21), v));
	seq_printf(m, "   20 VPM_v20           %lx\n", FIELD_GET(GENMASK(20, 20), v));
	seq_printf(m, "   19 VPM_v19           %lx\n", FIELD_GET(GENMASK(19, 19), v));
	seq_printf(m, "   18 VPM_v18           %lx\n", FIELD_GET(GENMASK(18, 18), v));
	seq_printf(m, "   17 VPM_v17           %lx\n", FIELD_GET(GENMASK(17, 17), v));
	seq_printf(m, "   16 VPM_v16           %lx\n", FIELD_GET(GENMASK(16, 16), v));
	seq_printf(m, "   15 VPM_v15           %lx\n", FIELD_GET(GENMASK(15, 15), v));
	seq_printf(m, "   14 VPM_v14           %lx\n", FIELD_GET(GENMASK(14, 14), v));
	seq_printf(m, "   13 VPM_v13           %lx\n", FIELD_GET(GENMASK(13, 13), v));
	seq_printf(m, "   12 VPM_v12           %lx\n", FIELD_GET(GENMASK(12, 12), v));
	seq_printf(m, "   11 VPM_v11           %lx\n", FIELD_GET(GENMASK(11, 11), v));
	seq_printf(m, "   10 VPM_v10           %lx\n", FIELD_GET(GENMASK(10, 10), v));
	seq_printf(m, "   09 VPM_v09           %lx\n", FIELD_GET(GENMASK( 9,  9), v));
	seq_printf(m, "   08 VPM_v08           %lx\n", FIELD_GET(GENMASK( 8,  8), v));
	seq_printf(m, "   07 VPM_v07           %lx\n", FIELD_GET(GENMASK( 7,  7), v));
	seq_printf(m, "   07 VPM_v06           %lx\n", FIELD_GET(GENMASK( 6,  6), v));
	seq_printf(m, "   05 VPM_v05           %lx\n", FIELD_GET(GENMASK( 5,  5), v));
	seq_printf(m, "   04 VPM_v04           %lx\n", FIELD_GET(GENMASK( 4,  4), v));
	seq_printf(m, "   03 VPM_v03           %lx\n", FIELD_GET(GENMASK( 3,  3), v));
	seq_printf(m, "   02 VPM_v02           %lx\n", FIELD_GET(GENMASK( 2,  2), v));
	seq_printf(m, "   01 VPM_v01           %lx\n", FIELD_GET(GENMASK( 1,  1), v));
	seq_printf(m, "   00 VPM_v00           %lx\n", FIELD_GET(GENMASK( 0,  0), v));
	seq_puts  (m, "\n");
}

static void dump_show_mpam_register(struct seq_file *m)
{
	unsigned long bases[64];
	unsigned int i, count;

	dump_show_mpam_cpu_register(m);

	count = dump_show_mpam_acpi_table(m, bases, 64);
	for (i = 0; i < count; i++)
		dump_show_mpam_hw_register(m, bases[i], i);
}

static void dump_show_process(struct seq_file *m)
{
	struct task_struct *p;

	seq_puts(m, "\n");
	seq_puts(m, "Available processes\n");
	seq_puts(m, "\n");

	for_each_process(p)
		seq_printf(m, "pid: %d  comm: %s\n", p->pid, p->comm);

	seq_puts(m, "\n");
}

static void dump_show_mm(struct seq_file *m)
{
	struct task_struct *p, *task = NULL;
	struct mm_struct *mm;

	for_each_process(p) {
		if (!strcmp(p->comm, "test")) {
			task = p;
			break;
		}
	}

	if (!task) {
		seq_puts(m, "\n");
		seq_puts(m, "No available process\n");
		seq_puts(m, "\n");
		return;
	}

	/* Process */
	seq_puts(m, "\n");
	seq_printf(m, "pid: %d  comm: %s\n", task->pid, task->comm);
	seq_puts(m, "\n");

	/* mm_struct */
	mm = task->mm;
	seq_puts(m, "-------------------- mm_struct --------------------\n");
	seq_printf(m, "mm_count:                %d\n",    atomic_read(&mm->mm_count));
	seq_puts(  m, "mm_mt:                   -\n");
	seq_printf(m, "mmap_base:               0x%lx\n", mm->mmap_base);
	seq_printf(m, "mmap_legacy_base:        0x%lx\n", mm->mmap_legacy_base);
#ifdef CONFIG_HAVE_ARCH_COMPAT_MMAP_BASES
	seq_printf(m, "mmap_compat_base:        0x%lx\n", mm->mmap_compat_base);
	seq_printf(m, "mmap_compat_legacy_base: 0x%lx\n", mm->mmap_compat_legacy_base);
#endif
	seq_printf(m, "task_size:               0x%lx\n", mm->task_size);
	seq_printf(m, "pgd:                     0x%p\n",  mm->pgd);
#ifdef CONFIG_MEMBARRIER
	seq_printf(m, "membarrier_state:        %d\n",    atomic_read(&mm->membarrier_state));
#endif
	seq_printf(m, "mm_users:                %d\n",    atomic_read(&mm->mm_users));
#ifdef CONFIG_SCHED_MM_CID
	seq_printf(m, "pcpu_cid:                0x%p\n",  mm->pcpu_cid);
	seq_printf(m, "mm_cid_next_scan:        0x%lx\n", mm->mm_cid_next_scan);
#endif
#ifdef CONFIG_MMU
	seq_printf(m, "pgtables_bytes:          0x%lx\n",
		   atomic_long_read(&mm->pgtables_bytes));
#endif
	seq_printf(m, "map_count:               %d\n",    mm->map_count);
	seq_printf(m, "hiwater_rss:             0x%lx\n", mm->hiwater_rss);
	seq_printf(m, "hiwater_vm:              0x%lx\n", mm->hiwater_vm);
	seq_printf(m, "total_vm:                0x%lx\n", mm->total_vm);
	seq_printf(m, "locked_vm:               0x%lx\n", mm->locked_vm);
	seq_printf(m, "pinned_vm:               0x%llx\n", atomic64_read(&mm->pinned_vm));
	seq_printf(m, "data_vm:                 0x%lx\n", mm->data_vm);
	seq_printf(m, "exec_vm:                 0x%lx\n", mm->exec_vm);
	seq_printf(m, "stack_vm:                0x%lx\n", mm->stack_vm);
	seq_printf(m, "def_flags:               0x%lx\n", mm->def_flags);
	seq_printf(m, "start_code:              0x%lx\n", mm->start_code);
	seq_printf(m, "end_code:                0x%lx\n", mm->end_code);
	seq_printf(m, "start_data:              0x%lx\n", mm->start_data);
	seq_printf(m, "end_data:                0x%lx\n", mm->end_data);
	seq_printf(m, "start_brk:               0x%lx\n", mm->start_brk);
	seq_printf(m, "brk:                     0x%lx\n", mm->brk);
	seq_printf(m, "start_stack:             0x%lx\n", mm->start_stack);
	seq_printf(m, "arg_start:               0x%lx\n", mm->arg_start);
	seq_printf(m, "arg_end:                 0x%lx\n", mm->arg_end);
	seq_printf(m, "env_start:               0x%lx\n", mm->env_start);
	seq_printf(m, "env_end:                 0x%lx\n", mm->env_end);
	seq_puts(  m, "saved_auxv:              -\n");
	seq_printf(m, "rss_stat[FILE]:          0x%lx\n", get_mm_counter(mm, MM_FILEPAGES));
	seq_printf(m, "rss_stat[ANON]:          0x%lx\n", get_mm_counter(mm, MM_ANONPAGES));
	seq_printf(m, "rss_stat[SWAP]:          0x%lx\n", get_mm_counter(mm, MM_SWAPENTS));
	seq_printf(m, "rss_stat[SHMEM]:         0x%lx\n", get_mm_counter(mm, MM_SHMEMPAGES));
	seq_printf(m, "binfmt:                  0x%p\n",  mm->binfmt);
	seq_puts(  m, "context:                 -\n");
	seq_printf(m, "flags:                   0x%lx\n", mm->flags);
#ifdef CONFIG_MEMCG
	seq_printf(m, "owner:                   0x%p\n",  mm->owner);
#endif
	seq_printf(m, "user_ns:                 0x%p\n",  mm->user_ns);
	seq_printf(m, "exe_file:                0x%p\n",  mm->exe_file);
#ifdef CONFIG_MMU_NOTIFIER
	seq_printf(m, "notifier_subscriptions:  0x%p\n",  mm->notifier_subscriptions);
#endif
#ifdef CONFIG_NUMA_BALANCING
	seq_printf(m, "numa_next_scan:          0x%lx\n", mm->numa_next_scan);
	seq_printf(m, "numa_scan_offset:        0x%lx\n", mm->numa_scan_offset);
	seq_printf(m, "numa_scan_seq:           0x%x\n",  mm->numa_scan_seq);
#endif
	seq_printf(m, "tlb_flush_pending:       %d\n",    atomic_read(&mm->tlb_flush_pending));
#ifdef CONFIG_ARCH_WANT_BATCHED_UNMAP_TLB_FLUSH
	seq_printf(m, "tlb_flush_batched:       %d\n",    atomic_read(&mm->tlb_flush_batched));
#endif
#ifdef CONFIG_HUGETLB_PAGE
	seq_printf(m, "hugetlb_usage:           0x%lx\n",
		   atomic_long_read(&mm->hugetlb_usage));
#endif
	seq_puts(  m, "async_put_work:          -\n");
#ifdef CONFIG_IOMMU_MM_DATA
	seq_printf(m, "iommu_mm:                0x%p\n",  mm->iommu_mm);
#endif
#ifdef CONFIG_KSM
	seq_printf(m, "kvm_merging_pages:       0x%lx\n", mm->ksm_merging_pages);
	seq_printf(m, "ksm_rmap_items:          0x%lx\n", mm->ksm_rmap_items);
	seq_printf(m, "ksm_zero_pages:          0x%lx\n", atomic_long_read(&mm->ksm_zero_pages));
#endif
	seq_puts(  m, "\n");
}

static void dump_show_mm_mt(struct seq_file *m)
{
	struct task_struct *p, *task = NULL;
	struct mm_struct *mm;

	for_each_process(p) {
		if (!strcmp(p->comm, "test")) {
			task = p;
			break;
		}
	}

	if (!task) {
		seq_puts(m, "\n");
		seq_puts(m, "No available process\n");
		seq_puts(m, "\n");
		return;
	}

	/* Process */
	seq_puts(m, "\n");
	seq_printf(m, "pid: %d  comm: %s\n", task->pid, task->comm);
	seq_puts(m, "\n");

	/* mm_struct */
	mm = task->mm;
	seq_puts(m, "-------------------- mm_struct::mm_mt --------------------\n");
	seq_puts(m, "\n");
}

static int dump_show(struct seq_file *m, void *v)
{
	switch (dump_option) {
	case DUMP_OPT_HELP:
		dump_show_help(m);
		break;
	case DUMP_OPT_REGISTER:
		dump_show_register(m);
		break;
	case DUMP_OPT_FEATURE_REGISTER:
		dump_show_feature_register(m);
		break;
	case DUMP_OPT_CACHE_REGISTER:
		dump_show_cache_register(m);
		break;
	case DUMP_OPT_MPAM_REGISTER:
		dump_show_mpam_register(m);
		break;
	case DUMP_OPT_PROCESS:
		dump_show_process(m);
		break;
	case DUMP_OPT_MM:
		dump_show_mm(m);
		break;
	case DUMP_OPT_MM_MT:
		dump_show_mm_mt(m);
		break;
	default:
		seq_puts(m, "\n");
		seq_printf(m, "Unsupported option %d\n", dump_option);
		seq_puts(m, "\n");
	}

	return 0;
}

static int dump_open(struct inode *inode, struct file *file)
{
	return single_open(file, dump_show, pde_data(inode));
}

static ssize_t dump_write(struct file *file, const char __user *buf,
			  size_t count, loff_t *pos)
{
	int i;
	char option[64];

	memset(option, 0, sizeof(option));
	if (count < 1 ||
	    count > sizeof(option) - 1 ||
	    copy_from_user(option, buf, count))
		return -EFAULT;

	option[count - 1] = '\0';
	for (i = 0; i < ARRAY_SIZE(dump_options); i++) {
		if (!strcmp(option, dump_options[i])) {
			dump_option = i;
			break;
		}
	}

	if (i >= ARRAY_SIZE(dump_options))
		return -EINVAL;

	return count;
}

static const struct proc_ops dump_fops = {
	.proc_open    = dump_open,
	.proc_read    = seq_read,
	.proc_write   = dump_write,
	.proc_lseek   = seq_lseek,
	.proc_release = single_release,
};

static int __init dump_init(void)
{
	pde = proc_create("dump", 0444, NULL, &dump_fops);
	if (!pde)
		return -ENOMEM;

	return 0;
}

static void __exit dump_exit(void)
{
	proc_remove(pde);
}

module_init(dump_init);
module_exit(dump_exit);

MODULE_VERSION(DRIVER_VERSION);
MODULE_LICENSE("GPL v2");
MODULE_AUTHOR(DRIVER_AUTHOR);
MODULE_DESCRIPTION(DRIVER_DESC);
