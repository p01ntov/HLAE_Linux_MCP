/* SysV x86-64 dlsym interposer. libc uses the caller's return address to
 * implement RTLD_NEXT; forwarding through a C++ call changes that address.
 * Tail-jump after restoring the entry stack so libc sees the real caller.
 */
#if !defined(__linux__) || !defined(__x86_64__)
#error "The live dlsym resolver requires Linux x86-64"
#endif

    .text
    .p2align 4
    .globl dlsym
    .type dlsym, @function
    .hidden camera_dlsym_override
    .hidden camera_libc_dlsym
dlsym:
    .cfi_startproc
    endbr64
    /* Entry rsp is 8 mod 16. Reserve 24 bytes to align both helper calls. */
    subq $24, %rsp
    .cfi_def_cfa_offset 32
    movq %rdi, 0(%rsp)
    movq %rsi, 8(%rsp)
    .cfi_remember_state
    call camera_dlsym_override
    testq %rax, %rax
    jne .Lcamera_dlsym_return

    call camera_libc_dlsym
    testq %rax, %rax
    je .Lcamera_dlsym_return
    movq 0(%rsp), %rdi
    movq 8(%rsp), %rsi
    addq $24, %rsp
    .cfi_def_cfa_offset 8
    jmp *%rax

.Lcamera_dlsym_return:
    .cfi_restore_state
    addq $24, %rsp
    .cfi_def_cfa_offset 8
    ret
    .cfi_endproc
    .size dlsym, .-dlsym

    .section .note.GNU-stack,"",@progbits
