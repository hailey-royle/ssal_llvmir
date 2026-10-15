.global _start
_start:
	mov rdi, [rsp] // argc
	mov rsi, rsp
	add rsi, 8     // argv
	call start
	mov rdi, rax   // exit with !return code
	mov rax, 60
	syscall
	jmp exit   // should never reach

.global exit
exit:
	mov rax, 60
	syscall
	jmp exit   // should never reach

.global write
write:
	mov rax, 1
	syscall
	ret

