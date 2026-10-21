.global _start
_start:

/* argc or <argumnt count>: move from the stack to the first procedure argument register */
	mov rdi, [rsp]

/* <argument capacity>: is the same as <argument count>*/
	mov rsi, rdi

/* <argument data>:
  First find how many bytes will be added to the stack.
  This is the number of arguments times the size of one argument.
  The size of one argument is 16 ( count + capacity + data )
  Or: (sub rsp (mul 16 <argument count>))
  This is using only rdx so it uses a neg and add instead
*/
	mov rdx, rdi
	shl rdx, 4
	neg rdx
	add rdx, rsp

/*
  r8 is the number of arguments on left to process.
  r9 is the arugment array write location.
*/
	pop r8
	mov r9, rdx

__start_argument_loop:

/* if there are no more arguments left, call ssal start */
	cmp r8, 0
	jz __start_call_start
	dec r8

/*
  r10 is the pointer to the next argument array.
  r11 is the length of the string.
  move the next argument's data pointer to the stack.
*/
	pop r10
	xor r11, r11
	mov [r9+8], r10

/* Increment the pointer and count until the null terminating byte is found. */
__start_argument_get_argument_count:
	cmp byte ptr [r10], 0
	jz __start_argument_found_argument_count_end
	inc r11
	inc r10
	jmp __start_argument_get_argument_count

/* Move the argument's count and capacity in place, then start the next argument */
__start_argument_found_argument_count_end:
	mov [r9], r11d
	mov [r9+4], r11d
	add r9, 16
	jmp __start_argument_loop

/* Move the stack pointer the the correct place and call ssal start */
__start_call_start:
	mov rsp, rdx
	call start

/* move start's return value to rdi and fallthrough into exit */
	mov rdi, rax

/* exit the program */
.global exit
exit:
	mov rax, 60
	syscall

/* should never reach this point, if the syscall fails, try again */
	jmp exit

.global write
write:
	mov rax, 1
	syscall
	ret

