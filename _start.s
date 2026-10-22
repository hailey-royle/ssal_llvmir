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

/* exit procedure: code i32: */
.global exit
exit:
	mov rax, 60
	syscall

/* should never reach this point, if the syscall fails, try again */
	jmp exit

/*
  log procedure i32: buffer []i8:
  write the contents of buffer to stdout.
  procedure arguments:
    rdi - <buffer count>
    rsi - <buffer capacity>
    rdx - <buffer data>
  syscall write arguments:
    rdi - fd
    rsi - <buffer data>
    rdx - <buffer count>
    rax - 0
  the write syscall will return between -4095 and -1 if error, and the number of bytes written if not.
  log returns 0 if the number of bytes written is <buffer count> and not zero (1) otherwise.
*/
.global log
log:
	mov rax, 1
	mov rsi, rdx
	mov rdx, rdi
	mov rdi, 1
	syscall
	cmp rax, rdi
	mov rax, 0
	cmove rax, rdi
	ret

