/*
 * Copyright (c) 2026 jialiang.  All rights reserved.
 *
 * io.h - i386 port I/O and CPU control declarations.
 *
 * These were formerly in arch/i386/hal/hal.h.  The implementations live in
 * arch/i386/x86.asm.  This header is architecture-private but used by drivers
 * (ata/atapi/pccon/npx) across the kernel, so it sits at arch/i386/ level.
 */
#ifndef _ARCH_I386_IO_H_
#define _ARCH_I386_IO_H_

unsigned char	hal_inb(int port);
void		hal_outb(int port, int value);
void		hal_outw(int port, unsigned value);
void		hal_insw(int port, void *addr, unsigned count);
void		hal_outsw(int port, const void *addr, unsigned count);

/* CPU control; implemented in x86.asm. */
void		cpu_load_cr3(unsigned pd);
unsigned	cpu_read_cr3(void);
void		cpu_halt(void);
void		cpu_cli(void);
void		cpu_sti(void);
void		cpu_idle(void);		/* S432: sti+hlt, one indivisible pair */
unsigned	cpu_read_esp(void);
void		cpu_write_esp(unsigned esp);
unsigned	hal_rdtsc(void);
void		cpu_breakpoint(void);
void		kgdb_return_to_user(void);

/* i386 NPX helpers used by npx.c (assembly in x86.asm). */
void		npx_init(void);
void		npx_init_finite(void);
void		npx_clear(void);
void		npx_save(void *area);
void		npx_restore(void *area);
unsigned short	npx_get_control(void);
unsigned short	npx_get_status(void);
void		npx_set_control(unsigned short control);

#endif /* !_ARCH_I386_IO_H_ */
