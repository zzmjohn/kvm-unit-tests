#include "libcflat.h"
#include "desc.h"
#include "apic-defs.h"
#include "apic.h"
#include "processor.h"
#include "vm.h"

#define xstr(s) str(s)
#define str(s) #s

static volatile int test_count;
static volatile unsigned int test_divider;

static char *fault_addr;
static ulong fault_phys;

static int g_fail;
static int g_tests;

static inline void io_delay(void)
{
}

static void report(const char *msg, int pass)
{
    ++g_tests;
    printf("%s: %s\n", msg, (pass ? "PASS" : "FAIL"));
    if (!pass)
        ++g_fail;
}

static void nmi_tss(void)
{
start:
	printf("NMI task is running\n");
	print_current_tss_info();
	test_count++;
	asm volatile ("iret");
	goto start;
}

static void de_tss(void)
{
start:
	printf("DE task is running\n");
	print_current_tss_info();
	test_divider = 10;
	test_count++;
	asm volatile ("iret");
	goto start;
}

static void of_tss(void)
{
start:
	printf("OF task is running\n");
	print_current_tss_info();
	test_count++;
	asm volatile ("iret");
	goto start;
}

static void bp_tss(void)
{
start:
	printf("BP task is running\n");
	print_current_tss_info();
	test_count++;
	asm volatile ("iret");
	goto start;
}

void do_pf_tss(ulong *error_code)
{
	printf("PF task is running %x %x\n", error_code, *(ulong*)error_code);
	print_current_tss_info();
	if (*(ulong*)error_code == 0x2) /* write access, not present */
		test_count++;
	install_pte(phys_to_virt(read_cr3()), 1, fault_addr,
		    fault_phys | PTE_PRESENT | PTE_WRITE, 0);
}

extern void pf_tss(void);

asm (
	"pf_tss: \n\t"
	"push %esp \n\t"
	"call do_pf_tss \n\t"
	"add $4, %esp \n\t"
	"iret\n\t"
	"jmp pf_tss\n\t"
    );

static void jmp_tss(void)
{
start:
	printf("JMP to task succeeded\n");
	print_current_tss_info();
	test_count++;
	asm volatile ("ljmp $" xstr(TSS_MAIN) ", $0");
	goto start;
}

static void irq_tss(void)
{
start:
	printf("IRQ task is running\n");
	print_current_tss_info();
	test_count++;
	asm volatile ("iret");
	test_count++;
	printf("IRQ task restarts after iret.\n");
	goto start;
}

int main()
{
	unsigned int res;

	setup_vm();
	setup_idt();
	setup_gdt();
	setup_tss32();

	/* test that int $2 triggers task gate */
	test_count = 0;
	set_intr_task_gate(2, nmi_tss);
	printf("Triggering nmi 2\n");
	asm volatile ("int $2");
	printf("Return from nmi %d\n", test_count);
	report("NMI int $2", test_count == 1);

	/* test that external NMI triggers task gate */
	test_count = 0;
	set_intr_task_gate(2, nmi_tss);
	printf("Triggering nmi through APIC\n");
	apic_icr_write(APIC_DEST_PHYSICAL | APIC_DM_NMI | APIC_INT_ASSERT, 0);
	io_delay();
	printf("Return from APIC nmi\n");
	report("NMI external", test_count == 1);

	/* test that external interrupt triggesr task gate */
	test_count = 0;
	printf("Trigger IRQ from APIC\n");
	set_intr_task_gate(0xf0, irq_tss);
	irq_enable();
	apic_icr_write(APIC_DEST_SELF | APIC_DEST_PHYSICAL | APIC_DM_FIXED | APIC_INT_ASSERT | 0xf0, 0);
	io_delay();
	irq_disable();
	printf("Return from APIC IRQ\n");
	report("IRQ external", test_count == 1);

	/* test that HW exception triggesr task gate */
	set_intr_task_gate(0, de_tss);
	printf("Try to devide by 0\n");
	asm volatile ("divl %3": "=a"(res)
		      : "d"(0), "a"(1500), "m"(test_divider));
	printf("Result is %d\n", res);
	report("DE exeption", res == 150);

	/* test if call HW exeption DE by int $0 triggers task gate */
	test_count = 0;
	set_intr_task_gate(0, de_tss);
	printf("Call int 0\n");
	asm volatile ("int $0");
	printf("Return from int 0\n");
	report("int $0", test_count == 1);

	/* test if HW exception OF triggers task gate */
	test_count = 0;
	set_intr_task_gate(4, of_tss);
	printf("Call into\n");
	asm volatile ("addb $127, %b0\ninto"::"a"(127));
	printf("Return from into\n");
	report("OF exeption", test_count);

	/* test if HW exception BP triggers task gate */
	test_count = 0;
	set_intr_task_gate(3, bp_tss);
	printf("Call int 3\n");
	asm volatile ("int $3");
	printf("Return from int 3\n");
	report("BP exeption", test_count == 1);

	/*
	 * test that PF triggers task gate and error code is placed on
	 * exception task's stack
	 */
	fault_addr = alloc_vpage();
	fault_phys = (ulong)virt_to_phys(alloc_page());
	test_count = 0;
	set_intr_task_gate(14, pf_tss);
	printf("Access unmapped page\n");
	*fault_addr = 0;
	printf("Return from pf tss\n");
	report("PF exeption", test_count == 1);

	/* test that calling a task by lcall works */
	test_count = 0;
	set_intr_task_gate(0, irq_tss);
	printf("Calling task by lcall\n");
	/* hlt opcode is 0xf4 I use destination IP 0xf4f4f4f4 to catch
	   incorrect instruction length calculation */
	asm volatile("lcall $" xstr(TSS_INTR) ", $0xf4f4f4f4");
	printf("Return from call\n");
	report("lcall", test_count == 1);

	/* call the same task again and check that it restarted after iret */
	test_count = 0;
	asm volatile("lcall $" xstr(TSS_INTR) ", $0xf4f4f4f4");
	report("lcall2", test_count == 2);

	/* test that calling a task by ljmp works */
	test_count = 0;
	set_intr_task_gate(0, jmp_tss);
	printf("Jumping to a task by ljmp\n");
	asm volatile ("ljmp $" xstr(TSS_INTR) ", $0xf4f4f4f4");
	printf("Jump back succeeded\n");
	report("ljmp", test_count == 1);

	printf("\nsummary: %d tests, %d failures\n", g_tests, g_fail);

	return g_fail != 0;
}
