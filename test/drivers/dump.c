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
#include <linux/mm.h>
#include <linux/sched.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>

#define DRIVER_VERSION	"0.1"
#define DRIVER_AUTHOR	"Gavin Shan, Redhat Inc"
#define DRIVER_DESC	"Dump items through procfs"

enum {
	DUMP_OPT_HELP,
	DUMP_OPT_REGISTER,
	DUMP_OPT_REGISTER_CACHE,
	DUMP_OPT_REGISTER_MPAM,
	DUMP_OPT_PROCESS,
	DUMP_OPT_MM,
	DUMP_OPT_MM_MT,
	DUMP_OPT_MAX
};

static struct proc_dir_entry *pde;
static int dump_option = DUMP_OPT_REGISTER_MPAM;
static const char * const dump_options[] = {
	"help",
	"register",
	"register_cache",
	"register_mpam",
	"process",
	"mm",
	"mm_maple_tree",
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

static void dump_show_register(struct seq_file *m)
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

static void dump_show_register_cache(struct seq_file *m)
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

#define SYS_ID_MPAM0_EL1	sys_reg(3, 0, 10, 5, 1)
#define SYS_ID_MPAM1_EL1	sys_reg(3, 0, 10, 5, 0)
#define SYS_ID_MPAM2_EL2	sys_reg(3, 4, 10, 5, 0)
#define SYS_ID_MPAM3_EL3	sys_reg(3, 6, 10, 5, 0)
#define SYS_ID_MPAMHCR_EL2	sys_reg(3, 4, 10, 4, 0)
#define SYS_ID_MAPMIDR_EL1	sys_reg(3, 0, 10, 4, 4)
#define SYS_ID_MPAMSM_EL1	sys_reg(3, 0, 10, 5, 3)
#define SYS_ID_MPAMVPM0_EL2	sys_reg(3, 4, 10, 6, 0)
#define SYS_ID_MPAMVPM1_EL2	sys_reg(3, 4, 10, 6, 1)
#define SYS_ID_MPAMVPM2_EL2	sys_reg(3, 4, 10, 6, 2)
#define SYS_ID_MPAMVPM3_EL2	sys_reg(3, 4, 10, 6, 3)
#define SYS_ID_MPAMVPM4_EL2	sys_reg(3, 4, 10, 6, 4)
#define SYS_ID_MPAMVPM5_EL2	sys_reg(3, 4, 10, 6, 5)
#define SYS_ID_MPAMVPM6_EL2	sys_reg(3, 4, 10, 6, 6)
#define SYS_ID_MPAMVPM7_EL2	sys_reg(3, 4, 10, 6, 7)
#define SYS_ID_MPAMVPMV_EL2	sys_reg(3, 4, 10, 4, 1)

static void dump_show_register_mpam(struct seq_file *m)
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

	/* ID_MPAM0_EL1  3 0 10 5 1: MPAM0 Register (EL1) */
	v = read_sysreg_s(SYS_ID_MPAM0_EL1);
	seq_printf(m, "ID_MPAM0_EL1:           %08x_%08x\n",
		   (unsigned int)(v >> 32), (unsigned int)(v & 0xffffffff));
	seq_puts  (m, "----------------------------------------------\n");
	seq_printf(m, "63:48 Res0              %lx\n", FIELD_GET(GENMASK(63, 48), v));
	seq_printf(m, "47:40 PMG_D             %lx\n", FIELD_GET(GENMASK(47, 40), v));
	seq_printf(m, "39:32 PMG_I             %lx\n", FIELD_GET(GENMASK(39, 32), v));
	seq_printf(m, "31:16 PARTID_D          %lx\n", FIELD_GET(GENMASK(31, 16), v));
	seq_printf(m, "15:00 PARTID_I          %lx\n", FIELD_GET(GENMASK(15,  0), v));
	seq_puts  (m, "\n");

	/* ID_MPAM1_EL1  3 0 10 5 0: MPAM1 Register (EL1) */
	v = read_sysreg_s(SYS_ID_MPAM1_EL1);
	seq_printf(m, "ID_MPAM1_EL1:           %08x_%08x\n",
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

	/* ID_MPAM2_EL1  3 4 10 5 0: MPAM2 Register (EL2) */
	v = read_sysreg_s(SYS_ID_MPAM2_EL2);
	seq_printf(m, "ID_MPAM2_EL1:           %08x_%08x\n",
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

	/* ID_MPAM3_EL3  3 6 10 5 0: MPAM3 Regiter (EL3) */
	/* ID_MPAMHCR_EL2  3 4 10 4 0: MPAM Hypervisor Control Register (EL2) */
	v = read_sysreg_s(SYS_ID_MPAMHCR_EL2);
	seq_printf(m, "ID_MPAMHCR_EL2:         %08x_%08x\n",
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

	/* ID_MAPMIDR_EL1  3 0 10 4 4: MPAM ID Register (EL1) */
	v = read_sysreg_s(SYS_ID_MAPMIDR_EL1);
	seq_printf(m, "ID_MAPMIDR_EL1:         %08x_%08x\n",
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
		v = read_sysreg_s(SYS_ID_MPAMSM_EL1);
		seq_printf(m, "ID_MPAMSM_EL1:         %08x_%08x\n",
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
	 * ID_MPAMVPM0_EL2  3 4 10 6 0: MPAM Virtual PARTID Mapping Register 0
	 * ID_MPAMVPM1_EL2  3 4 10 6 1: MPAM Virtual PARTID Mapping Register 1
	 * ID_MPAMVPM2_EL2  3 4 10 6 2: MPAM Virtual PARTID Mapping Register 2
	 * ID_MPAMVPM3_EL2  3 4 10 6 3: MPAM Virtual PARTID Mapping Register 3
         * ID_MPAMVPM4_EL2  3 4 10 6 4: MPAM Virtual PARTID Mapping Register 4
         * ID_MPAMVPM5_EL2  3 4 10 6 5: MPAM Virtual PARTID Mapping Register 5
	 * ID_MPAMVPM6_EL2  3 4 10 6 6: MPAM Virtual PARTID Mapping Register 6
         * ID_MPAMVPM7_EL2  3 4 10 6 7: MPAM Virtual PARTID Mapping Register 7
         * ID_MPAMVPMV_EL2  3 4 10 4 1: MPAM Virtual PARTID Mapping Valid Register
	 */
	v = read_sysreg_s(SYS_ID_MPAMVPM7_EL2);
	seq_printf(m, "ID_MPAMVPM7_EL2:        %08x_%08x\n",
		   (unsigned int)(v >> 32), (unsigned int)(v & 0xffffffff));
	seq_puts  (m, "----------------------------------------------\n");
	seq_printf(m, "63:48 PhyPARTID31       %lx\n", FIELD_GET(GENMASK(63, 48), v));
	seq_printf(m, "47:32 PhyPARTID30       %lx\n", FIELD_GET(GENMASK(47, 32), v));
	seq_printf(m, "31:16 PhyPARTID29       %lx\n", FIELD_GET(GENMASK(31, 16), v));
	seq_printf(m, "15:00 PhyPARTID28       %lx\n", FIELD_GET(GENMASK(15,  0), v));
	seq_puts  (m, "\n");

	v = read_sysreg_s(SYS_ID_MPAMVPM6_EL2);
	seq_printf(m, "ID_MPAMVPM6_EL2:        %08x_%08x\n",
		   (unsigned int)(v >> 32), (unsigned int)(v & 0xffffffff));
	seq_puts  (m, "----------------------------------------------\n");
	seq_printf(m, "63:48 PhyPARTID27       %lx\n", FIELD_GET(GENMASK(63, 48), v));
	seq_printf(m, "47:32 PhyPARTID26       %lx\n", FIELD_GET(GENMASK(47, 32), v));
	seq_printf(m, "31:16 PhyPARTID25       %lx\n", FIELD_GET(GENMASK(31, 16), v));
	seq_printf(m, "15:00 PhyPARTID24       %lx\n", FIELD_GET(GENMASK(15,  0), v));
	seq_puts  (m, "\n");

	v = read_sysreg_s(SYS_ID_MPAMVPM5_EL2);
	seq_printf(m, "ID_MPAMVPM5_EL2:        %08x_%08x\n",
		   (unsigned int)(v >> 32), (unsigned int)(v & 0xffffffff));
	seq_puts  (m, "----------------------------------------------\n");
	seq_printf(m, "63:48 PhyPARTID23       %lx\n", FIELD_GET(GENMASK(63, 48), v));
	seq_printf(m, "47:32 PhyPARTID22       %lx\n", FIELD_GET(GENMASK(47, 32), v));
	seq_printf(m, "31:16 PhyPARTID21       %lx\n", FIELD_GET(GENMASK(31, 16), v));
	seq_printf(m, "15:00 PhyPARTID20       %lx\n", FIELD_GET(GENMASK(15,  0), v));
	seq_puts  (m, "\n");

	v = read_sysreg_s(SYS_ID_MPAMVPM4_EL2);
	seq_printf(m, "ID_MPAMVPM4_EL2:        %08x_%08x\n",
		   (unsigned int)(v >> 32), (unsigned int)(v & 0xffffffff));
	seq_puts  (m, "----------------------------------------------\n");
	seq_printf(m, "63:48 PhyPARTID19       %lx\n", FIELD_GET(GENMASK(63, 48), v));
	seq_printf(m, "47:32 PhyPARTID18       %lx\n", FIELD_GET(GENMASK(47, 32), v));
	seq_printf(m, "31:16 PhyPARTID17       %lx\n", FIELD_GET(GENMASK(31, 16), v));
	seq_printf(m, "15:00 PhyPARTID16       %lx\n", FIELD_GET(GENMASK(15,  0), v));
	seq_puts  (m, "\n");

	v = read_sysreg_s(SYS_ID_MPAMVPM3_EL2);
	seq_printf(m, "ID_MPAMVPM3_EL2:        %08x_%08x\n",
		   (unsigned int)(v >> 32), (unsigned int)(v & 0xffffffff));
	seq_puts  (m, "----------------------------------------------\n");
	seq_printf(m, "63:48 PhyPARTID15       %lx\n", FIELD_GET(GENMASK(63, 48), v));
	seq_printf(m, "47:32 PhyPARTID14       %lx\n", FIELD_GET(GENMASK(47, 32), v));
	seq_printf(m, "31:16 PhyPARTID13       %lx\n", FIELD_GET(GENMASK(31, 16), v));
	seq_printf(m, "15:00 PhyPARTID12       %lx\n", FIELD_GET(GENMASK(15,  0), v));
	seq_puts  (m, "\n");

	v = read_sysreg_s(SYS_ID_MPAMVPM2_EL2);
	seq_printf(m, "ID_MPAMVPM7_EL2:        %08x_%08x\n",
		   (unsigned int)(v >> 32), (unsigned int)(v & 0xffffffff));
	seq_puts  (m, "----------------------------------------------\n");
	seq_printf(m, "63:48 PhyPARTID11       %lx\n", FIELD_GET(GENMASK(63, 48), v));
	seq_printf(m, "47:32 PhyPARTID10       %lx\n", FIELD_GET(GENMASK(47, 32), v));
	seq_printf(m, "31:16 PhyPARTID09       %lx\n", FIELD_GET(GENMASK(31, 16), v));
	seq_printf(m, "15:00 PhyPARTID08       %lx\n", FIELD_GET(GENMASK(15,  0), v));
	seq_puts  (m, "\n");

	v = read_sysreg_s(SYS_ID_MPAMVPM1_EL2);
	seq_printf(m, "ID_MPAMVPM1_EL2:        %08x_%08x\n",
		   (unsigned int)(v >> 32), (unsigned int)(v & 0xffffffff));
	seq_puts  (m, "----------------------------------------------\n");
	seq_printf(m, "63:48 PhyPARTID07       %lx\n", FIELD_GET(GENMASK(63, 48), v));
	seq_printf(m, "47:32 PhyPARTID06       %lx\n", FIELD_GET(GENMASK(47, 32), v));
	seq_printf(m, "31:16 PhyPARTID05       %lx\n", FIELD_GET(GENMASK(31, 16), v));
	seq_printf(m, "15:00 PhyPARTID04       %lx\n", FIELD_GET(GENMASK(15,  0), v));
	seq_puts  (m, "\n");

	v = read_sysreg_s(SYS_ID_MPAMVPM0_EL2);
	seq_printf(m, "ID_MPAMVPM0_EL2:        %08x_%08x\n",
		   (unsigned int)(v >> 32), (unsigned int)(v & 0xffffffff));
	seq_puts  (m, "----------------------------------------------\n");
	seq_printf(m, "63:48 PhyPARTID03       %lx\n", FIELD_GET(GENMASK(63, 48), v));
	seq_printf(m, "47:32 PhyPARTID02       %lx\n", FIELD_GET(GENMASK(47, 32), v));
	seq_printf(m, "31:16 PhyPARTID01       %lx\n", FIELD_GET(GENMASK(31, 16), v));
	seq_printf(m, "15:00 PhyPARTID00       %lx\n", FIELD_GET(GENMASK(15,  0), v));
	seq_puts  (m, "\n");

	v = read_sysreg_s(SYS_ID_MPAMVPMV_EL2);
	seq_printf(m, "ID_MPAMVPMV_EL2:        %08x_%08x\n",
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
#ifdef CONFIG_MMU
	seq_printf(m, "get_unmapped_area:       0x%p\n",  mm->get_unmapped_area);
#endif
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
	seq_printf(m, "ksm_zero_pages:          0x%lx\n", mm->ksm_zero_pages);
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
	case DUMP_OPT_REGISTER_CACHE:
		dump_show_register_cache(m);
		break;
	case DUMP_OPT_REGISTER_MPAM:
		dump_show_register_mpam(m);
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
