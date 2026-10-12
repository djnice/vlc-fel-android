/* SPDX-License-Identifier: GPL-2.0-or-later */
/* the mvc3d kernel module, embedded (mvc3d_module_install); MVC3D_KO: its path, a string */
	.section .rodata.mvc3d_ko, "a"
	.balign 16
	.global mvc3d_ko_start
	.hidden mvc3d_ko_start
mvc3d_ko_start:
	.incbin MVC3D_KO
	.global mvc3d_ko_end
	.hidden mvc3d_ko_end
mvc3d_ko_end:
	.section .note.GNU-stack, "", %progbits
