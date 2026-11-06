/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 djnice <djnice@freemail.hu> */
/*
 * the mvc3d kernel modules, embedded (mvc3d_module_install); MVC3D_KO: its path
 * MVC3D_KO_SC2 (optional): the S905X4 module (mvc3d_sc2.c)
 */
	.section .rodata.mvc3d_ko, "a"
	.balign 16
	.global mvc3d_ko_start
	.hidden mvc3d_ko_start
mvc3d_ko_start:
	.incbin MVC3D_KO
	.global mvc3d_ko_end
	.hidden mvc3d_ko_end
mvc3d_ko_end:
#ifdef MVC3D_KO_SC2
	.balign 16
	.global mvc3d_sc2_ko_start
	.hidden mvc3d_sc2_ko_start
mvc3d_sc2_ko_start:
	.incbin MVC3D_KO_SC2
	.global mvc3d_sc2_ko_end
	.hidden mvc3d_sc2_ko_end
mvc3d_sc2_ko_end:
#endif
	.section .note.GNU-stack, "", %progbits
