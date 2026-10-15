.global _start
_start:
	mov rdi, [rsp] // argc
	mov rsi, rsp
	add rsi, 8     // argv
	call start
	mov rdi, rax   // exit with !return code
	mov rax, 60
	syscall
	jmp exit_syscall   // should never reach

.global exit_syscall
exit_syscall:
	mov rax, 60
	syscall
	jmp exit_syscall   // should never reach

.global write_syscall
write_syscall:
	mov rax, 1
	syscall
	ret

