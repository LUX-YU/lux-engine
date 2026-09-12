E 0000000000000000 <public: bool __cdecl lux::simulation::script::detail::BoundedClassStorage::release(struct lux::simulation::script::detail::BoundedClassStorage::Ticket)>:
       0: 40 53                        	pushq	%rbx
       2: 48 83 ec 40                  	subq	$0x40, %rsp
       6: 4c 8b d2                     	movq	%rdx, %r10
       9: 4c 8b c9                     	movq	%rcx, %r9
       c: 48 39 0a                     	cmpq	%rcx, (%rdx)
       f: 0f 85 9d 00 00 00            	jne	0xb2 <public: bool __cdecl lux::simulation::script::detail::BoundedClassStorage::release(struct lux::simulation::script::detail::BoundedClassStorage::Ticket)+0xb2>
      15: 44 8b 5a 10                  	movl	0x10(%rdx), %r11d
      19: 48 b8 67 66 66 66 66 66 66 66	movabsq	$0x6666666666666667, %rax # imm = 0x6666666666666667
      23: 48 8b 49 28                  	movq	0x28(%rcx), %rcx
      27: 4d 8b 41 30                  	movq	0x30(%r9), %r8
      2b: 4c 2b c1                     	subq	%rcx, %r8
      2e: 49 f7 e8                     	imulq	%r8
      31: 48 c1 fa 04                  	sarq	$0x4, %rdx
      35: 48 8b c2                     	movq	%rdx, %rax
      38: 48 c1 e8 3f                  	shrq	$0x3f, %rax
      3c: 48 03 d0                     	addq	%rax, %rdx
      3f: 4c 3b da                     	cmpq	%rdx, %r11
      42: 73 6e                        	jae	0xb2 <public: bool __cdecl lux::simulation::script::detail::BoundedClassStorage::release(struct lux::simulation::script::detail::BoundedClassStorage::Ticket)+0xb2>
      44: 41 8b 5a 14                  	movl	0x14(%r10), %ebx
      48: 4b 8d 04 9b                  	leaq	(%r11,%r11,4), %rax
      4c: 3b 5c c1 10                  	cmpl	0x10(%rcx,%rax,8), %ebx
      50: 4c 8d 04 c1                  	leaq	(%rcx,%rax,8), %r8
      54: 73 5c                        	jae	0xb2 <public: bool __cdecl lux::simulation::script::detail::BoundedClassStorage::release(struct lux::simulation::script::detail::BoundedClassStorage::Ticket)+0xb2>
      56: 49 8b 41 10                  	movq	0x10(%r9), %rax
      5a: 8b d3                        	movl	%ebx, %edx
      5c: 41 8b 48 08                  	movl	0x8(%r8), %ecx
      60: 48 c1 e1 05                  	shlq	$0x5, %rcx
      64: 44 89 5c 24 28               	movl	%r11d, 0x28(%rsp)
      69: 89 5c 24 2c                  	movl	%ebx, 0x2c(%rsp)
      6d: 48 0f af 54 01 10            	imulq	0x10(%rcx,%rax), %rdx
      73: 49 8b 42 08                  	movq	0x8(%r10), %rax
      77: 49 03 10                     	addq	(%r8), %rdx
      7a: 49 03 11                     	addq	(%r9), %rdx
      7d: 48 89 44 24 30               	movq	%rax, 0x30(%rsp)
      82: 41 8b 40 0c                  	movl	0xc(%r8), %eax
      86: 03 c3                        	addl	%ebx, %eax
      88: 48 89 54 24 20               	movq	%rdx, 0x20(%rsp)
      8d: 48 8d 54 24 20               	leaq	0x20(%rsp), %rdx
      92: 48 8d 0c 40                  	leaq	(%rax,%rax,2), %rcx
      96: 49 8b 41 40                  	movq	0x40(%r9), %rax
      9a: 48 8b 4c c8 08               	movq	0x8(%rax,%rcx,8), %rcx
      9f: 48 89 4c 24 38               	movq	%rcx, 0x38(%rsp)
      a4: 49 8b c9                     	movq	%r9, %rcx
      a7: e8 00 00 00 00               	callq	0xac <public: bool __cdecl lux::simulation::script::detail::BoundedClassStorage::release(struct lux::simulation::script::detail::BoundedClassStorage::Ticket)+0xac>
		00000000000000a8:  IMAGE_REL_AMD64_REL32	?release@BoundedClassStorage@detail@script@simulation@lux@@QEAA_NUAllocation@12345@@Z
      ac: 48 83 c4 40                  	addq	$0x40, %rsp
      b0: 5b                           	popq	%rbx
      b1: c3                           	retq
      b2: 32 c0                        	xorb	%al, %al
      b4: 48 83 c4 40                  	addq	$0x40, %rsp
      b8: 5b                           	popq	%rbx
      b9: c3                           	retq



E 0000000000000000 <private: class tl::expected<struct lux::simulation::script::detail::ScriptExecution::AwaitableRecord *, enum lux::simulation::script::EScriptAwaitableCreateError> __cdecl lux::simulation::script::detail::ScriptExecution::admitAwaitable(struct lux::simulation::script::detail::ScriptExecution::ExecutionInstance &, class std::optional<struct lux::simulation::script::PreparedResumeType>, bool)>:
       0: 40 55                        	pushq	%rbp
       2: 56                           	pushq	%rsi
       3: 41 57                        	pushq	%r15
       5: 48 83 ec 30                  	subq	$0x30, %rsp
       9: 80 b9 b8 01 00 00 00         	cmpb	$0x0, 0x1b8(%rcx)
      10: 4d 8b f8                     	movq	%r8, %r15
      13: 48 8b f2                     	movq	%rdx, %rsi
      16: 48 8b e9                     	movq	%rcx, %rbp
      19: 74 13                        	je	0x2e <private: class tl::expected<struct lux::simulation::script::detail::ScriptExecution::AwaitableRecord *, enum lux::simulation::script::EScriptAwaitableCreateError> __cdecl lux::simulation::script::detail::ScriptExecution::admitAwaitable(struct lux::simulation::script::detail::ScriptExecution::ExecutionInstance &, class std::optional<struct lux::simulation::script::PreparedResumeType>, bool)+0x2e>
      1b: c6 02 05                     	movb	$0x5, (%rdx)
      1e: 48 8b c2                     	movq	%rdx, %rax
      21: c6 42 08 00                  	movb	$0x0, 0x8(%rdx)
      25: 48 83 c4 30                  	addq	$0x30, %rsp
      29: 41 5f                        	popq	%r15
      2b: 5e                           	popq	%rsi
      2c: 5d                           	popq	%rbp
      2d: c3                           	retq
      2e: 48 8b 41 48                  	movq	0x48(%rcx), %rax
      32: 48 89 7c 24 60               	movq	%rdi, 0x60(%rsp)
      37: 48 8d b9 18 01 00 00         	leaq	0x118(%rcx), %rdi
      3e: 48 39 47 20                  	cmpq	%rax, 0x20(%rdi)
      42: 72 18                        	jb	0x5c <private: class tl::expected<struct lux::simulation::script::detail::ScriptExecution::AwaitableRecord *, enum lux::simulation::script::EScriptAwaitableCreateError> __cdecl lux::simulation::script::detail::ScriptExecution::admitAwaitable(struct lux::simulation::script::detail::ScriptExecution::ExecutionInstance &, class std::optional<struct lux::simulation::script::PreparedResumeType>, bool)+0x5c>
      44: 48 8b 7c 24 60               	movq	0x60(%rsp), %rdi
      49: 48 8b c6                     	movq	%rsi, %rax
      4c: c6 02 03                     	movb	$0x3, (%rdx)
      4f: c6 42 08 00                  	movb	$0x0, 0x8(%rdx)
      53: 48 83 c4 30                  	addq	$0x30, %rsp
      57: 41 5f                        	popq	%r15
      59: 5e                           	popq	%rsi
      5a: 5d                           	popq	%rbp
      5b: c3                           	retq
      5c: 8b 57 18                     	movl	0x18(%rdi), %edx
      5f: 4c 89 64 24 28               	movq	%r12, 0x28(%rsp)
      64: 4c 89 74 24 20               	movq	%r14, 0x20(%rsp)
      69: 83 fa ff                     	cmpl	$-0x1, %edx
      6c: 0f 84 b7 01 00 00            	je	0x229 <private: class tl::expected<struct lux::simulation::script::detail::ScriptExecution::AwaitableRecord *, enum lux::simulation::script::EScriptAwaitableCreateError> __cdecl lux::simulation::script::detail::ScriptExecution::admitAwaitable(struct lux::simulation::script::detail::ScriptExecution::ExecutionInstance &, class std::optional<struct lux::simulation::script::PreparedResumeType>, bool)+0x229>
      72: 41 0f 10 01                  	movups	(%r9), %xmm0
      76: 44 0f b6 64 24 70            	movzbl	0x70(%rsp), %r12d
      7c: 41 0f 10 49 10               	movups	0x10(%r9), %xmm1
      81: 4c 69 f2 f0 00 00 00         	imulq	$0xf0, %rdx, %r14
      88: 4c 03 37                     	addq	(%rdi), %r14
      8b: 41 8b 46 18                  	movl	0x18(%r14), %eax
      8f: 89 47 18                     	movl	%eax, 0x18(%rdi)
      92: 8d 42 01                     	leal	0x1(%rdx), %eax
      95: 41 89 06                     	movl	%eax, (%r14)
      98: 49 8b 00                     	movq	(%r8), %rax
      9b: 41 0f 11 46 28               	movups	%xmm0, 0x28(%r14)
      a0: 49 89 46 08                  	movq	%rax, 0x8(%r14)
      a4: 41 0f 11 4e 38               	movups	%xmm1, 0x38(%r14)
      a9: 4d 89 7e 10                  	movq	%r15, 0x10(%r14)
      ad: 41 c6 46 24 00               	movb	$0x0, 0x24(%r14)
      b2: 45 88 a6 a5 00 00 00         	movb	%r12b, 0xa5(%r14)
      b9: 48 ff 47 20                  	incq	0x20(%rdi)
      bd: 4d 85 f6                     	testq	%r14, %r14
      c0: 0f 84 63 01 00 00            	je	0x229 <private: class tl::expected<struct lux::simulation::script::detail::ScriptExecution::AwaitableRecord *, enum lux::simulation::script::EScriptAwaitableCreateError> __cdecl lux::simulation::script::detail::ScriptExecution::admitAwaitable(struct lux::simulation::script::detail::ScriptExecution::ExecutionInstance &, class std::optional<struct lux::simulation::script::PreparedResumeType>, bool)+0x229>
      c6: 48 89 5c 24 58               	movq	%rbx, 0x58(%rsp)
      cb: 49 8b 1e                     	movq	(%r14), %rbx
      ce: 45 84 e4                     	testb	%r12b, %r12b
      d1: 75 63                        	jne	0x136 <private: class tl::expected<struct lux::simulation::script::detail::ScriptExecution::AwaitableRecord *, enum lux::simulation::script::EScriptAwaitableCreateError> __cdecl lux::simulation::script::detail::ScriptExecution::admitAwaitable(struct lux::simulation::script::detail::ScriptExecution::ExecutionInstance &, class std::optional<struct lux::simulation::script::PreparedResumeType>, bool)+0x136>
      d3: 45 38 66 40                  	cmpb	%r12b, 0x40(%r14)
      d7: 74 5d                        	je	0x136 <private: class tl::expected<struct lux::simulation::script::detail::ScriptExecution::AwaitableRecord *, enum lux::simulation::script::EScriptAwaitableCreateError> __cdecl lux::simulation::script::detail::ScriptExecution::admitAwaitable(struct lux::simulation::script::detail::ScriptExecution::ExecutionInstance &, class std::optional<struct lux::simulation::script::PreparedResumeType>, bool)+0x136>
      d9: 41 0f 10 46 28               	movups	0x28(%r14), %xmm0
      de: 49 8d 4e 60                  	leaq	0x60(%r14), %rcx
      e2: 45 8b 46 34                  	movl	0x34(%r14), %r8d
      e6: f2 41 0f 10 4e 38            	movsd	0x38(%r14), %xmm1
      ec: 41 8b 56 30                  	movl	0x30(%r14), %edx
      f0: 41 0f 11 46 48               	movups	%xmm0, 0x48(%r14)
      f5: f2 41 0f 11 4e 58            	movsd	%xmm1, 0x58(%r14)
      fb: e8 00 00 00 00               	callq	0x100 <private: class tl::expected<struct lux::simulation::script::detail::ScriptExecution::AwaitableRecord *, enum lux::simulation::script::EScriptAwaitableCreateError> __cdecl lux::simulation::script::detail::ScriptExecution::admitAwaitable(struct lux::simulation::script::detail::ScriptExecution::ExecutionInstance &, class std::optional<struct lux::simulation::script::PreparedResumeType>, bool)+0x100>
		00000000000000fc:  IMAGE_REL_AMD64_REL32	?resize@ScriptOwnedBytes@script@simulation@lux@@QEAA_N_K0@Z
     100: 84 c0                        	testb	%al, %al
     102: 75 32                        	jne	0x136 <private: class tl::expected<struct lux::simulation::script::detail::ScriptExecution::AwaitableRecord *, enum lux::simulation::script::EScriptAwaitableCreateError> __cdecl lux::simulation::script::detail::ScriptExecution::admitAwaitable(struct lux::simulation::script::detail::ScriptExecution::ExecutionInstance &, class std::optional<struct lux::simulation::script::PreparedResumeType>, bool)+0x136>
     104: 49 8b d6                     	movq	%r14, %rdx
     107: 48 8b cf                     	movq	%rdi, %rcx
     10a: e8 00 00 00 00               	callq	0x10f <private: class tl::expected<struct lux::simulation::script::detail::ScriptExecution::AwaitableRecord *, enum lux::simulation::script::EScriptAwaitableCreateError> __cdecl lux::simulation::script::detail::ScriptExecution::admitAwaitable(struct lux::simulation::script::detail::ScriptExecution::ExecutionInstance &, class std::optional<struct lux::simulation::script::PreparedResumeType>, bool)+0x10f>
		000000000000010b:  IMAGE_REL_AMD64_REL32	?releaseResolved@AwaitableStorage@ScriptExecution@detail@script@simulation@lux@@QEAAXAEAUAwaitableRecord@23456@@Z
     10f: c6 06 04                     	movb	$0x4, (%rsi)
     112: 44 88 66 08                  	movb	%r12b, 0x8(%rsi)
     116: 48 8b 5c 24 58               	movq	0x58(%rsp), %rbx
     11b: 4c 8b 64 24 28               	movq	0x28(%rsp), %r12
     120: 48 8b c6                     	movq	%rsi, %rax
     123: 4c 8b 74 24 20               	movq	0x20(%rsp), %r14
     128: 48 8b 7c 24 60               	movq	0x60(%rsp), %rdi
     12d: 48 83 c4 30                  	addq	$0x30, %rsp
     131: 41 5f                        	popq	%r15
     133: 5e                           	popq	%rsi
     134: 5d                           	popq	%rbp
     135: c3                           	retq
     136: 49 8b 47 20                  	movq	0x20(%r15), %rax
     13a: 49 89 86 b8 00 00 00         	movq	%rax, 0xb8(%r14)
     141: 48 85 c0                     	testq	%rax, %rax
     144: 74 07                        	je	0x14d <private: class tl::expected<struct lux::simulation::script::detail::ScriptExecution::AwaitableRecord *, enum lux::simulation::script::EScriptAwaitableCreateError> __cdecl lux::simulation::script::detail::ScriptExecution::admitAwaitable(struct lux::simulation::script::detail::ScriptExecution::ExecutionInstance &, class std::optional<struct lux::simulation::script::PreparedResumeType>, bool)+0x14d>
     146: 4c 89 b0 b0 00 00 00         	movq	%r14, 0xb0(%rax)
     14d: 4d 89 77 20                  	movq	%r14, 0x20(%r15)
     151: 45 84 e4                     	testb	%r12b, %r12b
     154: 0f 84 bc 00 00 00            	je	0x216 <private: class tl::expected<struct lux::simulation::script::detail::ScriptExecution::AwaitableRecord *, enum lux::simulation::script::EScriptAwaitableCreateError> __cdecl lux::simulation::script::detail::ScriptExecution::admitAwaitable(struct lux::simulation::script::detail::ScriptExecution::ExecutionInstance &, class std::optional<struct lux::simulation::script::PreparedResumeType>, bool)+0x216>
     15a: 48 8b 45 20                  	movq	0x20(%rbp), %rax
     15e: 4c 8b 00                     	movq	(%rax), %r8
     161: 85 db                        	testl	%ebx, %ebx
     163: 0f 84 b9 00 00 00            	je	0x222 <private: class tl::expected<struct lux::simulation::script::detail::ScriptExecution::AwaitableRecord *, enum lux::simulation::script::EScriptAwaitableCreateError> __cdecl lux::simulation::script::detail::ScriptExecution::admitAwaitable(struct lux::simulation::script::detail::ScriptExecution::ExecutionInstance &, class std::optional<struct lux::simulation::script::PreparedResumeType>, bool)+0x222>
     169: 48 8b c3                     	movq	%rbx, %rax
     16c: 48 c1 e8 20                  	shrq	$0x20, %rax
     170: 85 c0                        	testl	%eax, %eax
     172: 0f 84 aa 00 00 00            	je	0x222 <private: class tl::expected<struct lux::simulation::script::detail::ScriptExecution::AwaitableRecord *, enum lux::simulation::script::EScriptAwaitableCreateError> __cdecl lux::simulation::script::detail::ScriptExecution::admitAwaitable(struct lux::simulation::script::detail::ScriptExecution::ExecutionInstance &, class std::optional<struct lux::simulation::script::PreparedResumeType>, bool)+0x222>
     178: 8b c3                        	movl	%ebx, %eax
     17a: 49 3b 40 18                  	cmpq	0x18(%r8), %rax
     17e: 0f 87 9e 00 00 00            	ja	0x222 <private: class tl::expected<struct lux::simulation::script::detail::ScriptExecution::AwaitableRecord *, enum lux::simulation::script::EScriptAwaitableCreateError> __cdecl lux::simulation::script::detail::ScriptExecution::admitAwaitable(struct lux::simulation::script::detail::ScriptExecution::ExecutionInstance &, class std::optional<struct lux::simulation::script::PreparedResumeType>, bool)+0x222>
     184: 8d 43 ff                     	leal	-0x1(%rbx), %eax
     187: 33 ed                        	xorl	%ebp, %ebp
     189: 48 8d 14 40                  	leaq	(%rax,%rax,2), %rdx
     18d: 49 8b 40 08                  	movq	0x8(%r8), %rax
     191: 4c 8d 3c d0                  	leaq	(%rax,%rdx,8), %r15
     195: 41 38 6e 40                  	cmpb	%bpl, 0x40(%r14)
     199: 74 06                        	je	0x1a1 <private: class tl::expected<struct lux::simulation::script::detail::ScriptExecution::AwaitableRecord *, enum lux::simulation::script::EScriptAwaitableCreateError> __cdecl lux::simulation::script::detail::ScriptExecution::admitAwaitable(struct lux::simulation::script::detail::ScriptExecution::ExecutionInstance &, class std::optional<struct lux::simulation::script::PreparedResumeType>, bool)+0x1a1>
     19b: 49 8b 46 28                  	movq	0x28(%r14), %rax
     19f: eb 03                        	jmp	0x1a4 <private: class tl::expected<struct lux::simulation::script::detail::ScriptExecution::AwaitableRecord *, enum lux::simulation::script::EScriptAwaitableCreateError> __cdecl lux::simulation::script::detail::ScriptExecution::admitAwaitable(struct lux::simulation::script::detail::ScriptExecution::ExecutionInstance &, class std::optional<struct lux::simulation::script::PreparedResumeType>, bool)+0x1a4>
     1a1: 48 8b c5                     	movq	%rbp, %rax
     1a4: 48 8d 4c 24 50               	leaq	0x50(%rsp), %rcx
     1a9: 48 89 44 24 50               	movq	%rax, 0x50(%rsp)
     1ae: e8 00 00 00 00               	callq	0x1b3 <private: class tl::expected<struct lux::simulation::script::detail::ScriptExecution::AwaitableRecord *, enum lux::simulation::script::EScriptAwaitableCreateError> __cdecl lux::simulation::script::detail::ScriptExecution::admitAwaitable(struct lux::simulation::script::detail::ScriptExecution::ExecutionInstance &, class std::optional<struct lux::simulation::script::PreparedResumeType>, bool)+0x1b3>
		00000000000001af:  IMAGE_REL_AMD64_REL32	??$_Atomic_reinterpret_as@_J_K@std@@YA_JAEB_K@Z
     1b3: 49 8d 4f 08                  	leaq	0x8(%r15), %rcx
     1b7: 48 8b f8                     	movq	%rax, %rdi
     1ba: e8 00 00 00 00               	callq	0x1bf <private: class tl::expected<struct lux::simulation::script::detail::ScriptExecution::AwaitableRecord *, enum lux::simulation::script::EScriptAwaitableCreateError> __cdecl lux::simulation::script::detail::ScriptExecution::admitAwaitable(struct lux::simulation::script::detail::ScriptExecution::ExecutionInstance &, class std::optional<struct lux::simulation::script::PreparedResumeType>, bool)+0x1bf>
		00000000000001bb:  IMAGE_REL_AMD64_REL32	??$_Atomic_address_as@_JU?$_Atomic_padded@_K@std@@@std@@YAPEC_JAEAU?$_Atomic_padded@_K@0@@Z
     1bf: 48 89 38                     	movq	%rdi, (%rax)
     1c2: 41 38 6e 40                  	cmpb	%bpl, 0x40(%r14)
     1c6: 74 04                        	je	0x1cc <private: class tl::expected<struct lux::simulation::script::detail::ScriptExecution::AwaitableRecord *, enum lux::simulation::script::EScriptAwaitableCreateError> __cdecl lux::simulation::script::detail::ScriptExecution::admitAwaitable(struct lux::simulation::script::detail::ScriptExecution::ExecutionInstance &, class std::optional<struct lux::simulation::script::PreparedResumeType>, bool)+0x1cc>
     1c8: 41 8b 6e 30                  	movl	0x30(%r14), %ebp
     1cc: 48 8d 4c 24 50               	leaq	0x50(%rsp), %rcx
     1d1: 89 6c 24 50                  	movl	%ebp, 0x50(%rsp)
     1d5: e8 00 00 00 00               	callq	0x1da <private: class tl::expected<struct lux::simulation::script::detail::ScriptExecution::AwaitableRecord *, enum lux::simulation::script::EScriptAwaitableCreateError> __cdecl lux::simulation::script::detail::ScriptExecution::admitAwaitable(struct lux::simulation::script::detail::ScriptExecution::ExecutionInstance &, class std::optional<struct lux::simulation::script::PreparedResumeType>, bool)+0x1da>
		00000000000001d6:  IMAGE_REL_AMD64_REL32	??$_Atomic_reinterpret_as@HI@std@@YAHAEBI@Z
     1da: 49 8d 4f 10                  	leaq	0x10(%r15), %rcx
     1de: 8b f8                        	movl	%eax, %edi
     1e0: e8 00 00 00 00               	callq	0x1e5 <private: class tl::expected<struct lux::simulation::script::detail::ScriptExecution::AwaitableRecord *, enum lux::simulation::script::EScriptAwaitableCreateError> __cdecl lux::simulation::script::detail::ScriptExecution::admitAwaitable(struct lux::simulation::script::detail::ScriptExecution::ExecutionInstance &, class std::optional<struct lux::simulation::script::PreparedResumeType>, bool)+0x1e5>
		00000000000001e1:  IMAGE_REL_AMD64_REL32	??$_Atomic_address_as@HU?$_Atomic_padded@I@std@@@std@@YAPECHAEAU?$_Atomic_padded@I@0@@Z
     1e5: 48 8d 4c 24 50               	leaq	0x50(%rsp), %rcx
     1ea: 89 38                        	movl	%edi, (%rax)
     1ec: 48 b8 01 00 00 00 ff ff ff ff	movabsq	$-0xffffffff, %rax      # imm = 0xFFFFFFFF00000001
     1f6: 48 23 d8                     	andq	%rax, %rbx
     1f9: 90                           	nop
     1fa: 48 83 cb 01                  	orq	$0x1, %rbx
     1fe: 48 89 5c 24 50               	movq	%rbx, 0x50(%rsp)
     203: e8 00 00 00 00               	callq	0x208 <private: class tl::expected<struct lux::simulation::script::detail::ScriptExecution::AwaitableRecord *, enum lux::simulation::script::EScriptAwaitableCreateError> __cdecl lux::simulation::script::detail::ScriptExecution::admitAwaitable(struct lux::simulation::script::detail::ScriptExecution::ExecutionInstance &, class std::optional<struct lux::simulation::script::PreparedResumeType>, bool)+0x208>
		0000000000000204:  IMAGE_REL_AMD64_REL32	??$_Atomic_reinterpret_as@_J_K@std@@YA_JAEB_K@Z
     208: 49 8b cf                     	movq	%r15, %rcx
     20b: 48 8b d8                     	movq	%rax, %rbx
     20e: e8 00 00 00 00               	callq	0x213 <private: class tl::expected<struct lux::simulation::script::detail::ScriptExecution::AwaitableRecord *, enum lux::simulation::script::EScriptAwaitableCreateError> __cdecl lux::simulation::script::detail::ScriptExecution::admitAwaitable(struct lux::simulation::script::detail::ScriptExecution::ExecutionInstance &, class std::optional<struct lux::simulation::script::PreparedResumeType>, bool)+0x213>
		000000000000020f:  IMAGE_REL_AMD64_REL32	??$_Atomic_address_as@_JU?$_Atomic_padded@_K@std@@@std@@YAPEC_JAEAU?$_Atomic_padded@_K@0@@Z
     213: 48 89 18                     	movq	%rbx, (%rax)
     216: 4c 89 36                     	movq	%r14, (%rsi)
     219: c6 46 08 01                  	movb	$0x1, 0x8(%rsi)
     21d: e9 f4 fe ff ff               	jmp	0x116 <private: class tl::expected<struct lux::simulation::script::detail::ScriptExecution::AwaitableRecord *, enum lux::simulation::script::EScriptAwaitableCreateError> __cdecl lux::simulation::script::detail::ScriptExecution::admitAwaitable(struct lux::simulation::script::detail::ScriptExecution::ExecutionInstance &, class std::optional<struct lux::simulation::script::PreparedResumeType>, bool)+0x116>
     222: ff 15 00 00 00 00            	callq	*(%rip)                 # 0x228 <private: class tl::expected<struct lux::simulation::script::detail::ScriptExecution::AwaitableRecord *, enum lux::simulation::script::EScriptAwaitableCreateError> __cdecl lux::simulation::script::detail::ScriptExecution::admitAwaitable(struct lux::simulation::script::detail::ScriptExecution::ExecutionInstance &, class std::optional<struct lux::simulation::script::PreparedResumeType>, bool)+0x228>
		0000000000000224:  IMAGE_REL_AMD64_REL32	__imp_terminate
     228: cc                           	int3
     229: c6 06 04                     	movb	$0x4, (%rsi)
     22c: c6 46 08 00                  	movb	$0x0, 0x8(%rsi)
     230: e9 e6 fe ff ff               	jmp	0x11b <private: class tl::expected<struct lux::simulation::script::detail::ScriptExecution::AwaitableRecord *, enum lux::simulation::script::EScriptAwaitableCreateError> __cdecl lux::simulation::script::detail::ScriptExecution::admitAwaitable(struct lux::simulation::script::detail::ScriptExecution::ExecutionInstance &, class std::optional<struct lux::simulation::script::PreparedResumeType>, bool)+0x11b>



E 0000000000000000 <public: void __cdecl lux::simulation::script::detail::ScriptExecution::AwaitableStorage::releaseResolved(struct lux::simulation::script::detail::ScriptExecution::AwaitableRecord &)>:
       0: 4c 8b dc                     	movq	%rsp, %r11
       3: 49 89 5b 10                  	movq	%rbx, 0x10(%r11)
       7: 49 89 6b 18                  	movq	%rbp, 0x18(%r11)
       b: 49 89 73 20                  	movq	%rsi, 0x20(%r11)
       f: 57                           	pushq	%rdi
      10: 48 81 ec 80 00 00 00         	subq	$0x80, %rsp
      17: 8b 32                        	movl	(%rdx), %esi
      19: 33 ed                        	xorl	%ebp, %ebp
      1b: 89 6c 24 32                  	movl	%ebp, 0x32(%rsp)
      1f: 8b c5                        	movl	%ebp, %eax
      21: 66 89 6c 24 36               	movw	%bp, 0x36(%rsp)
      26: 48 8b f9                     	movq	%rcx, %rdi
      29: 48 89 42 1c                  	movq	%rax, 0x1c(%rdx)
      2d: 48 8d 4a 48                  	leaq	0x48(%rdx), %rcx
      31: 48 8b da                     	movq	%rdx, %rbx
      34: 89 2a                        	movl	%ebp, (%rdx)
      36: 49 89 6b 98                  	movq	%rbp, -0x68(%r11)
      3a: 49 8d 53 98                  	leaq	-0x68(%r11), %rdx
      3e: 49 89 6b a0                  	movq	%rbp, -0x60(%r11)
      42: ff ce                        	decl	%esi
      44: 66 c7 44 24 30 00 01         	movw	$0x100, 0x30(%rsp)      # imm = 0x100
      4b: 48 89 ac 24 90 00 00 00      	movq	%rbp, 0x90(%rsp)
      53: 49 89 6b d0                  	movq	%rbp, -0x30(%r11)
      57: 49 89 6b d8                  	movq	%rbp, -0x28(%r11)
      5b: 49 89 6b e0                  	movq	%rbp, -0x20(%r11)
      5f: 49 c7 43 e8 08 00 00 00      	movq	$0x8, -0x18(%r11)
      67: e8 00 00 00 00               	callq	0x6c <public: void __cdecl lux::simulation::script::detail::ScriptExecution::AwaitableStorage::releaseResolved(struct lux::simulation::script::detail::ScriptExecution::AwaitableRecord &)+0x6c>
		0000000000000068:  IMAGE_REL_AMD64_REL32	??4ScriptOwnedResumeValue@script@simulation@lux@@QEAAAEAU0123@$$QEAU0123@@Z
      6c: 48 8b 4c 24 58               	movq	0x58(%rsp), %rcx
      71: 48 85 c9                     	testq	%rcx, %rcx
      74: 74 0a                        	je	0x80 <public: void __cdecl lux::simulation::script::detail::ScriptExecution::AwaitableStorage::releaseResolved(struct lux::simulation::script::detail::ScriptExecution::AwaitableRecord &)+0x80>
      76: 48 8b 54 24 70               	movq	0x70(%rsp), %rdx
      7b: e8 00 00 00 00               	callq	0x80 <public: void __cdecl lux::simulation::script::detail::ScriptExecution::AwaitableStorage::releaseResolved(struct lux::simulation::script::detail::ScriptExecution::AwaitableRecord &)+0x80>
		000000000000007c:  IMAGE_REL_AMD64_REL32	??3@YAXPEAXW4align_val_t@std@@@Z
      80: 89 ab a0 00 00 00            	movl	%ebp, 0xa0(%rbx)
      86: 40 88 ab a4 00 00 00         	movb	%bpl, 0xa4(%rbx)
      8d: 40 88 ab a6 00 00 00         	movb	%bpl, 0xa6(%rbx)
      94: 48 ff 4f 20                  	decq	0x20(%rdi)
      98: 8b 43 04                     	movl	0x4(%rbx), %eax
      9b: 83 f8 ff                     	cmpl	$-0x1, %eax
      9e: 74 10                        	je	0xb0 <public: void __cdecl lux::simulation::script::detail::ScriptExecution::AwaitableStorage::releaseResolved(struct lux::simulation::script::detail::ScriptExecution::AwaitableRecord &)+0xb0>
      a0: ff c0                        	incl	%eax
      a2: 89 43 04                     	movl	%eax, 0x4(%rbx)
      a5: 8b 47 18                     	movl	0x18(%rdi), %eax
      a8: 89 43 18                     	movl	%eax, 0x18(%rbx)
      ab: 89 77 18                     	movl	%esi, 0x18(%rdi)
      ae: eb 03                        	jmp	0xb3 <public: void __cdecl lux::simulation::script::detail::ScriptExecution::AwaitableStorage::releaseResolved(struct lux::simulation::script::detail::ScriptExecution::AwaitableRecord &)+0xb3>
      b0: 89 6b 04                     	movl	%ebp, 0x4(%rbx)
      b3: 4c 8d 9c 24 80 00 00 00      	leaq	0x80(%rsp), %r11
      bb: 49 8b 5b 18                  	movq	0x18(%r11), %rbx
      bf: 49 8b 6b 20                  	movq	0x20(%r11), %rbp
      c3: 49 8b 73 28                  	movq	0x28(%r11), %rsi
      c7: 49 8b e3                     	movq	%r11, %rsp
      ca: 5f                           	popq	%rdi
      cb: c3                           	retq



F 0000000000000000 <public: bool __cdecl lux::simulation::script::detail::BoundedClassStorage::release(struct lux::simulation::script::detail::BoundedClassStorage::Ticket)>:
       0: 48 83 ec 38                  	subq	$0x38, %rsp
       4: 4c 8b ca                     	movq	%rdx, %r9
       7: 4c 8b d1                     	movq	%rcx, %r10
       a: 48 39 0a                     	cmpq	%rcx, (%rdx)
       d: 75 72                        	jne	0x81 <public: bool __cdecl lux::simulation::script::detail::BoundedClassStorage::release(struct lux::simulation::script::detail::BoundedClassStorage::Ticket)+0x81>
       f: 44 8b 5a 10                  	movl	0x10(%rdx), %r11d
      13: 48 b8 67 66 66 66 66 66 66 66	movabsq	$0x6666666666666667, %rax # imm = 0x6666666666666667
      1d: 48 8b 49 28                  	movq	0x28(%rcx), %rcx
      21: 4d 8b 42 30                  	movq	0x30(%r10), %r8
      25: 4c 2b c1                     	subq	%rcx, %r8
      28: 49 f7 e8                     	imulq	%r8
      2b: 48 c1 fa 04                  	sarq	$0x4, %rdx
      2f: 48 8b c2                     	movq	%rdx, %rax
      32: 48 c1 e8 3f                  	shrq	$0x3f, %rax
      36: 48 03 d0                     	addq	%rax, %rdx
      39: 4c 3b da                     	cmpq	%rdx, %r11
      3c: 73 43                        	jae	0x81 <public: bool __cdecl lux::simulation::script::detail::BoundedClassStorage::release(struct lux::simulation::script::detail::BoundedClassStorage::Ticket)+0x81>
      3e: 45 8b 41 14                  	movl	0x14(%r9), %r8d
      42: 4b 8d 04 9b                  	leaq	(%r11,%r11,4), %rax
      46: 44 3b 44 c1 10               	cmpl	0x10(%rcx,%rax,8), %r8d
      4b: 48 8d 14 c1                  	leaq	(%rcx,%rax,8), %rdx
      4f: 73 30                        	jae	0x81 <public: bool __cdecl lux::simulation::script::detail::BoundedClassStorage::release(struct lux::simulation::script::detail::BoundedClassStorage::Ticket)+0x81>
      51: 8b 42 0c                     	movl	0xc(%rdx), %eax
      54: 4d 8b 49 08                  	movq	0x8(%r9), %r9
      58: 41 03 c0                     	addl	%r8d, %eax
      5b: 48 8d 0c 40                  	leaq	(%rax,%rax,2), %rcx
      5f: 49 8b 42 40                  	movq	0x40(%r10), %rax
      63: 48 8d 0c c8                  	leaq	(%rax,%rcx,8), %rcx
      67: 48 89 4c 24 28               	movq	%rcx, 0x28(%rsp)
      6c: 49 8b ca                     	movq	%r10, %rcx
      6f: 48 89 54 24 20               	movq	%rdx, 0x20(%rsp)
      74: 41 8b d3                     	movl	%r11d, %edx
      77: e8 00 00 00 00               	callq	0x7c <public: bool __cdecl lux::simulation::script::detail::BoundedClassStorage::release(struct lux::simulation::script::detail::BoundedClassStorage::Ticket)+0x7c>
		0000000000000078:  IMAGE_REL_AMD64_REL32	?releaseResolved@BoundedClassStorage@detail@script@simulation@lux@@AEAA_NII_KAEAUPage@12345@AEAUSlot@12345@@Z
      7c: 48 83 c4 38                  	addq	$0x38, %rsp
      80: c3                           	retq
      81: 32 c0                        	xorb	%al, %al
      83: 48 83 c4 38                  	addq	$0x38, %rsp
      87: c3                           	retq



F 0000000000000000 <private: bool __cdecl lux::simulation::script::detail::BoundedClassStorage::releaseResolved(unsigned int, unsigned int, unsigned __int64, struct lux::simulation::script::detail::BoundedClassStorage::Page &, struct lux::simulation::script::detail::BoundedClassStorage::Slot &)>:
       0: 48 89 5c 24 18               	movq	%rbx, 0x18(%rsp)
       5: 55                           	pushq	%rbp
       6: 4c 8b 5c 24 38               	movq	0x38(%rsp), %r11
       b: 41 8b e8                     	movl	%r8d, %ebp
       e: 8b da                        	movl	%edx, %ebx
      10: 4c 8b d1                     	movq	%rcx, %r10
      13: 41 80 7b 14 00               	cmpb	$0x0, 0x14(%r11)
      18: 0f 84 d3 00 00 00            	je	0xf1 <private: bool __cdecl lux::simulation::script::detail::BoundedClassStorage::releaseResolved(unsigned int, unsigned int, unsigned __int64, struct lux::simulation::script::detail::BoundedClassStorage::Page &, struct lux::simulation::script::detail::BoundedClassStorage::Slot &)+0xf1>
      1e: 4d 39 0b                     	cmpq	%r9, (%r11)
      21: 0f 85 ca 00 00 00            	jne	0xf1 <private: bool __cdecl lux::simulation::script::detail::BoundedClassStorage::releaseResolved(unsigned int, unsigned int, unsigned __int64, struct lux::simulation::script::detail::BoundedClassStorage::Page &, struct lux::simulation::script::detail::BoundedClassStorage::Slot &)+0xf1>
      27: 48 8b 54 24 30               	movq	0x30(%rsp), %rdx
      2c: 4c 8d 89 c0 00 00 00         	leaq	0xc0(%rcx), %r9
      33: 41 c6 43 14 00               	movb	$0x0, 0x14(%r11)
      38: ff 4a 20                     	decl	0x20(%rdx)
      3b: 48 ff 49 60                  	decq	0x60(%rcx)
      3f: 80 79 78 00                  	cmpb	$0x0, 0x78(%rcx)
      43: 74 26                        	je	0x6b <private: bool __cdecl lux::simulation::script::detail::BoundedClassStorage::releaseResolved(unsigned int, unsigned int, unsigned __int64, struct lux::simulation::script::detail::BoundedClassStorage::Page &, struct lux::simulation::script::detail::BoundedClassStorage::Slot &)+0x6b>
      45: 49 83 01 03                  	addq	$0x3, (%r9)
      49: 49 8b 43 08                  	movq	0x8(%r11), %rax
      4d: 48 29 81 98 00 00 00         	subq	%rax, 0x98(%rcx)
      54: 8b 4a 08                     	movl	0x8(%rdx), %ecx
      57: 49 8b 42 10                  	movq	0x10(%r10), %rax
      5b: 48 c1 e1 05                  	shlq	$0x5, %rcx
      5f: 48 8b 4c 01 10               	movq	0x10(%rcx,%rax), %rcx
      64: 49 29 8a a0 00 00 00         	subq	%rcx, 0xa0(%r10)
      6b: 45 33 c0                     	xorl	%r8d, %r8d
      6e: 4d 89 43 08                  	movq	%r8, 0x8(%r11)
      72: 83 7a 14 ff                  	cmpl	$-0x1, 0x14(%rdx)
      76: 75 66                        	jne	0xde <private: bool __cdecl lux::simulation::script::detail::BoundedClassStorage::releaseResolved(unsigned int, unsigned int, unsigned __int64, struct lux::simulation::script::detail::BoundedClassStorage::Page &, struct lux::simulation::script::detail::BoundedClassStorage::Slot &)+0xde>
      78: 49 8b 42 28                  	movq	0x28(%r10), %rax
      7c: 48 8d 0c 9b                  	leaq	(%rbx,%rbx,4), %rcx
      80: 48 89 74 24 10               	movq	%rsi, 0x10(%rsp)
      85: 8b 74 c8 08                  	movl	0x8(%rax,%rcx,8), %esi
      89: 48 c1 e6 05                  	shlq	$0x5, %rsi
      8d: 49 03 72 10                  	addq	0x10(%r10), %rsi
      91: c7 44 c8 18 ff ff ff ff      	movl	$0xffffffff, 0x18(%rax,%rcx,8) # imm = 0xFFFFFFFF
      99: 48 89 7c 24 18               	movq	%rdi, 0x18(%rsp)
      9e: 48 8d 3c c8                  	leaq	(%rax,%rcx,8), %rdi
      a2: 8b 46 18                     	movl	0x18(%rsi), %eax
      a5: 89 47 1c                     	movl	%eax, 0x1c(%rdi)
      a8: 8b 46 18                     	movl	0x18(%rsi), %eax
      ab: 83 f8 ff                     	cmpl	$-0x1, %eax
      ae: 74 0c                        	je	0xbc <private: bool __cdecl lux::simulation::script::detail::BoundedClassStorage::releaseResolved(unsigned int, unsigned int, unsigned __int64, struct lux::simulation::script::detail::BoundedClassStorage::Page &, struct lux::simulation::script::detail::BoundedClassStorage::Slot &)+0xbc>
      b0: 48 8d 0c 80                  	leaq	(%rax,%rax,4), %rcx
      b4: 49 8b 42 28                  	movq	0x28(%r10), %rax
      b8: 89 5c c8 18                  	movl	%ebx, 0x18(%rax,%rcx,8)
      bc: 89 5e 18                     	movl	%ebx, 0x18(%rsi)
      bf: 48 8b 74 24 10               	movq	0x10(%rsp), %rsi
      c4: 45 38 42 78                  	cmpb	%r8b, 0x78(%r10)
      c8: 74 0f                        	je	0xd9 <private: bool __cdecl lux::simulation::script::detail::BoundedClassStorage::releaseResolved(unsigned int, unsigned int, unsigned __int64, struct lux::simulation::script::detail::BoundedClassStorage::Page &, struct lux::simulation::script::detail::BoundedClassStorage::Slot &)+0xd9>
      ca: 83 7f 1c ff                  	cmpl	$-0x1, 0x1c(%rdi)
      ce: 41 0f 95 c0                  	setne	%r8b
      d2: 49 83 c0 02                  	addq	$0x2, %r8
      d6: 4d 01 01                     	addq	%r8, (%r9)
      d9: 48 8b 7c 24 18               	movq	0x18(%rsp), %rdi
      de: 8b 42 14                     	movl	0x14(%rdx), %eax
      e1: 41 89 43 10                  	movl	%eax, 0x10(%r11)
      e5: b0 01                        	movb	$0x1, %al
      e7: 89 6a 14                     	movl	%ebp, 0x14(%rdx)
      ea: 48 8b 5c 24 20               	movq	0x20(%rsp), %rbx
      ef: 5d                           	popq	%rbp
      f0: c3                           	retq
      f1: 48 8b 5c 24 20               	movq	0x20(%rsp), %rbx
      f6: 32 c0                        	xorb	%al, %al
      f8: 5d                           	popq	%rbp
      f9: c3                           	retq



F 0000000000000000 <private: class tl::expected<struct lux::simulation::script::detail::ScriptExecution::AwaitableRecord *, enum lux::simulation::script::EScriptAwaitableCreateError> __cdecl lux::simulation::script::detail::ScriptExecution::admitAwaitable(struct lux::simulation::script::detail::ScriptExecution::ExecutionInstance &, class std::optional<struct lux::simulation::script::PreparedResumeType> const &, bool)>:
       0: 40 55                        	pushq	%rbp
       2: 56                           	pushq	%rsi
       3: 41 57                        	pushq	%r15
       5: 48 83 ec 30                  	subq	$0x30, %rsp
       9: 80 b9 b8 01 00 00 00         	cmpb	$0x0, 0x1b8(%rcx)
      10: 4d 8b f8                     	movq	%r8, %r15
      13: 48 8b f2                     	movq	%rdx, %rsi
      16: 48 8b e9                     	movq	%rcx, %rbp
      19: 74 13                        	je	0x2e <private: class tl::expected<struct lux::simulation::script::detail::ScriptExecution::AwaitableRecord *, enum lux::simulation::script::EScriptAwaitableCreateError> __cdecl lux::simulation::script::detail::ScriptExecution::admitAwaitable(struct lux::simulation::script::detail::ScriptExecution::ExecutionInstance &, class std::optional<struct lux::simulation::script::PreparedResumeType> const &, bool)+0x2e>
      1b: c6 02 05                     	movb	$0x5, (%rdx)
      1e: 48 8b c2                     	movq	%rdx, %rax
      21: c6 42 08 00                  	movb	$0x0, 0x8(%rdx)
      25: 48 83 c4 30                  	addq	$0x30, %rsp
      29: 41 5f                        	popq	%r15
      2b: 5e                           	popq	%rsi
      2c: 5d                           	popq	%rbp
      2d: c3                           	retq
      2e: 48 8b 41 48                  	movq	0x48(%rcx), %rax
      32: 48 89 7c 24 60               	movq	%rdi, 0x60(%rsp)
      37: 48 8d b9 18 01 00 00         	leaq	0x118(%rcx), %rdi
      3e: 48 39 47 20                  	cmpq	%rax, 0x20(%rdi)
      42: 72 18                        	jb	0x5c <private: class tl::expected<struct lux::simulation::script::detail::ScriptExecution::AwaitableRecord *, enum lux::simulation::script::EScriptAwaitableCreateError> __cdecl lux::simulation::script::detail::ScriptExecution::admitAwaitable(struct lux::simulation::script::detail::ScriptExecution::ExecutionInstance &, class std::optional<struct lux::simulation::script::PreparedResumeType> const &, bool)+0x5c>
      44: 48 8b 7c 24 60               	movq	0x60(%rsp), %rdi
      49: 48 8b c6                     	movq	%rsi, %rax
      4c: c6 02 03                     	movb	$0x3, (%rdx)
      4f: c6 42 08 00                  	movb	$0x0, 0x8(%rdx)
      53: 48 83 c4 30                  	addq	$0x30, %rsp
      57: 41 5f                        	popq	%r15
      59: 5e                           	popq	%rsi
      5a: 5d                           	popq	%rbp
      5b: c3                           	retq
      5c: 8b 57 18                     	movl	0x18(%rdi), %edx
      5f: 4c 89 64 24 28               	movq	%r12, 0x28(%rsp)
      64: 4c 89 74 24 20               	movq	%r14, 0x20(%rsp)
      69: 83 fa ff                     	cmpl	$-0x1, %edx
      6c: 0f 84 b7 01 00 00            	je	0x229 <private: class tl::expected<struct lux::simulation::script::detail::ScriptExecution::AwaitableRecord *, enum lux::simulation::script::EScriptAwaitableCreateError> __cdecl lux::simulation::script::detail::ScriptExecution::admitAwaitable(struct lux::simulation::script::detail::ScriptExecution::ExecutionInstance &, class std::optional<struct lux::simulation::script::PreparedResumeType> const &, bool)+0x229>
      72: 44 0f b6 64 24 70            	movzbl	0x70(%rsp), %r12d
      78: 4c 69 f2 f0 00 00 00         	imulq	$0xf0, %rdx, %r14
      7f: 4c 03 37                     	addq	(%rdi), %r14
      82: 41 8b 46 18                  	movl	0x18(%r14), %eax
      86: 89 47 18                     	movl	%eax, 0x18(%rdi)
      89: 8d 42 01                     	leal	0x1(%rdx), %eax
      8c: 41 89 06                     	movl	%eax, (%r14)
      8f: 49 8b 00                     	movq	(%r8), %rax
      92: 49 89 46 08                  	movq	%rax, 0x8(%r14)
      96: 4d 89 7e 10                  	movq	%r15, 0x10(%r14)
      9a: 41 c6 46 24 00               	movb	$0x0, 0x24(%r14)
      9f: 41 0f 10 01                  	movups	(%r9), %xmm0
      a3: 41 0f 11 46 28               	movups	%xmm0, 0x28(%r14)
      a8: 41 0f 10 49 10               	movups	0x10(%r9), %xmm1
      ad: 45 88 a6 a4 00 00 00         	movb	%r12b, 0xa4(%r14)
      b4: 41 0f 11 4e 38               	movups	%xmm1, 0x38(%r14)
      b9: 48 ff 47 20                  	incq	0x20(%rdi)
      bd: 4d 85 f6                     	testq	%r14, %r14
      c0: 0f 84 63 01 00 00            	je	0x229 <private: class tl::expected<struct lux::simulation::script::detail::ScriptExecution::AwaitableRecord *, enum lux::simulation::script::EScriptAwaitableCreateError> __cdecl lux::simulation::script::detail::ScriptExecution::admitAwaitable(struct lux::simulation::script::detail::ScriptExecution::ExecutionInstance &, class std::optional<struct lux::simulation::script::PreparedResumeType> const &, bool)+0x229>
      c6: 48 89 5c 24 58               	movq	%rbx, 0x58(%rsp)
      cb: 49 8b 1e                     	movq	(%r14), %rbx
      ce: 45 84 e4                     	testb	%r12b, %r12b
      d1: 75 63                        	jne	0x136 <private: class tl::expected<struct lux::simulation::script::detail::ScriptExecution::AwaitableRecord *, enum lux::simulation::script::EScriptAwaitableCreateError> __cdecl lux::simulation::script::detail::ScriptExecution::admitAwaitable(struct lux::simulation::script::detail::ScriptExecution::ExecutionInstance &, class std::optional<struct lux::simulation::script::PreparedResumeType> const &, bool)+0x136>
      d3: 45 38 66 40                  	cmpb	%r12b, 0x40(%r14)
      d7: 74 5d                        	je	0x136 <private: class tl::expected<struct lux::simulation::script::detail::ScriptExecution::AwaitableRecord *, enum lux::simulation::script::EScriptAwaitableCreateError> __cdecl lux::simulation::script::detail::ScriptExecution::admitAwaitable(struct lux::simulation::script::detail::ScriptExecution::ExecutionInstance &, class std::optional<struct lux::simulation::script::PreparedResumeType> const &, bool)+0x136>
      d9: 41 0f 10 46 28               	movups	0x28(%r14), %xmm0
      de: 49 8d 4e 60                  	leaq	0x60(%r14), %rcx
      e2: 45 8b 46 34                  	movl	0x34(%r14), %r8d
      e6: f2 41 0f 10 4e 38            	movsd	0x38(%r14), %xmm1
      ec: 41 8b 56 30                  	movl	0x30(%r14), %edx
      f0: 41 0f 11 46 48               	movups	%xmm0, 0x48(%r14)
      f5: f2 41 0f 11 4e 58            	movsd	%xmm1, 0x58(%r14)
      fb: e8 00 00 00 00               	callq	0x100 <private: class tl::expected<struct lux::simulation::script::detail::ScriptExecution::AwaitableRecord *, enum lux::simulation::script::EScriptAwaitableCreateError> __cdecl lux::simulation::script::detail::ScriptExecution::admitAwaitable(struct lux::simulation::script::detail::ScriptExecution::ExecutionInstance &, class std::optional<struct lux::simulation::script::PreparedResumeType> const &, bool)+0x100>
		00000000000000fc:  IMAGE_REL_AMD64_REL32	?resize@ScriptOwnedBytes@script@simulation@lux@@QEAA_N_K0@Z
     100: 84 c0                        	testb	%al, %al
     102: 75 32                        	jne	0x136 <private: class tl::expected<struct lux::simulation::script::detail::ScriptExecution::AwaitableRecord *, enum lux::simulation::script::EScriptAwaitableCreateError> __cdecl lux::simulation::script::detail::ScriptExecution::admitAwaitable(struct lux::simulation::script::detail::ScriptExecution::ExecutionInstance &, class std::optional<struct lux::simulation::script::PreparedResumeType> const &, bool)+0x136>
     104: 49 8b d6                     	movq	%r14, %rdx
     107: 48 8b cf                     	movq	%rdi, %rcx
     10a: e8 00 00 00 00               	callq	0x10f <private: class tl::expected<struct lux::simulation::script::detail::ScriptExecution::AwaitableRecord *, enum lux::simulation::script::EScriptAwaitableCreateError> __cdecl lux::simulation::script::detail::ScriptExecution::admitAwaitable(struct lux::simulation::script::detail::ScriptExecution::ExecutionInstance &, class std::optional<struct lux::simulation::script::PreparedResumeType> const &, bool)+0x10f>
		000000000000010b:  IMAGE_REL_AMD64_REL32	?releaseResolved@AwaitableStorage@ScriptExecution@detail@script@simulation@lux@@QEAAXAEAUAwaitableRecord@23456@@Z
     10f: c6 06 04                     	movb	$0x4, (%rsi)
     112: 44 88 66 08                  	movb	%r12b, 0x8(%rsi)
     116: 48 8b 5c 24 58               	movq	0x58(%rsp), %rbx
     11b: 4c 8b 64 24 28               	movq	0x28(%rsp), %r12
     120: 48 8b c6                     	movq	%rsi, %rax
     123: 4c 8b 74 24 20               	movq	0x20(%rsp), %r14
     128: 48 8b 7c 24 60               	movq	0x60(%rsp), %rdi
     12d: 48 83 c4 30                  	addq	$0x30, %rsp
     131: 41 5f                        	popq	%r15
     133: 5e                           	popq	%rsi
     134: 5d                           	popq	%rbp
     135: c3                           	retq
     136: 49 8b 47 20                  	movq	0x20(%r15), %rax
     13a: 49 89 86 b8 00 00 00         	movq	%rax, 0xb8(%r14)
     141: 48 85 c0                     	testq	%rax, %rax
     144: 74 07                        	je	0x14d <private: class tl::expected<struct lux::simulation::script::detail::ScriptExecution::AwaitableRecord *, enum lux::simulation::script::EScriptAwaitableCreateError> __cdecl lux::simulation::script::detail::ScriptExecution::admitAwaitable(struct lux::simulation::script::detail::ScriptExecution::ExecutionInstance &, class std::optional<struct lux::simulation::script::PreparedResumeType> const &, bool)+0x14d>
     146: 4c 89 b0 b0 00 00 00         	movq	%r14, 0xb0(%rax)
     14d: 4d 89 77 20                  	movq	%r14, 0x20(%r15)
     151: 45 84 e4                     	testb	%r12b, %r12b
     154: 0f 84 bc 00 00 00            	je	0x216 <private: class tl::expected<struct lux::simulation::script::detail::ScriptExecution::AwaitableRecord *, enum lux::simulation::script::EScriptAwaitableCreateError> __cdecl lux::simulation::script::detail::ScriptExecution::admitAwaitable(struct lux::simulation::script::detail::ScriptExecution::ExecutionInstance &, class std::optional<struct lux::simulation::script::PreparedResumeType> const &, bool)+0x216>
     15a: 48 8b 45 20                  	movq	0x20(%rbp), %rax
     15e: 4c 8b 00                     	movq	(%rax), %r8
     161: 85 db                        	testl	%ebx, %ebx
     163: 0f 84 b9 00 00 00            	je	0x222 <private: class tl::expected<struct lux::simulation::script::detail::ScriptExecution::AwaitableRecord *, enum lux::simulation::script::EScriptAwaitableCreateError> __cdecl lux::simulation::script::detail::ScriptExecution::admitAwaitable(struct lux::simulation::script::detail::ScriptExecution::ExecutionInstance &, class std::optional<struct lux::simulation::script::PreparedResumeType> const &, bool)+0x222>
     169: 48 8b c3                     	movq	%rbx, %rax
     16c: 48 c1 e8 20                  	shrq	$0x20, %rax
     170: 85 c0                        	testl	%eax, %eax
     172: 0f 84 aa 00 00 00            	je	0x222 <private: class tl::expected<struct lux::simulation::script::detail::ScriptExecution::AwaitableRecord *, enum lux::simulation::script::EScriptAwaitableCreateError> __cdecl lux::simulation::script::detail::ScriptExecution::admitAwaitable(struct lux::simulation::script::detail::ScriptExecution::ExecutionInstance &, class std::optional<struct lux::simulation::script::PreparedResumeType> const &, bool)+0x222>
     178: 8b c3                        	movl	%ebx, %eax
     17a: 49 3b 40 18                  	cmpq	0x18(%r8), %rax
     17e: 0f 87 9e 00 00 00            	ja	0x222 <private: class tl::expected<struct lux::simulation::script::detail::ScriptExecution::AwaitableRecord *, enum lux::simulation::script::EScriptAwaitableCreateError> __cdecl lux::simulation::script::detail::ScriptExecution::admitAwaitable(struct lux::simulation::script::detail::ScriptExecution::ExecutionInstance &, class std::optional<struct lux::simulation::script::PreparedResumeType> const &, bool)+0x222>
     184: 8d 43 ff                     	leal	-0x1(%rbx), %eax
     187: 33 ed                        	xorl	%ebp, %ebp
     189: 48 8d 14 40                  	leaq	(%rax,%rax,2), %rdx
     18d: 49 8b 40 08                  	movq	0x8(%r8), %rax
     191: 4c 8d 3c d0                  	leaq	(%rax,%rdx,8), %r15
     195: 41 38 6e 40                  	cmpb	%bpl, 0x40(%r14)
     199: 74 06                        	je	0x1a1 <private: class tl::expected<struct lux::simulation::script::detail::ScriptExecution::AwaitableRecord *, enum lux::simulation::script::EScriptAwaitableCreateError> __cdecl lux::simulation::script::detail::ScriptExecution::admitAwaitable(struct lux::simulation::script::detail::ScriptExecution::ExecutionInstance &, class std::optional<struct lux::simulation::script::PreparedResumeType> const &, bool)+0x1a1>
     19b: 49 8b 46 28                  	movq	0x28(%r14), %rax
     19f: eb 03                        	jmp	0x1a4 <private: class tl::expected<struct lux::simulation::script::detail::ScriptExecution::AwaitableRecord *, enum lux::simulation::script::EScriptAwaitableCreateError> __cdecl lux::simulation::script::detail::ScriptExecution::admitAwaitable(struct lux::simulation::script::detail::ScriptExecution::ExecutionInstance &, class std::optional<struct lux::simulation::script::PreparedResumeType> const &, bool)+0x1a4>
     1a1: 48 8b c5                     	movq	%rbp, %rax
     1a4: 48 8d 4c 24 50               	leaq	0x50(%rsp), %rcx
     1a9: 48 89 44 24 50               	movq	%rax, 0x50(%rsp)
     1ae: e8 00 00 00 00               	callq	0x1b3 <private: class tl::expected<struct lux::simulation::script::detail::ScriptExecution::AwaitableRecord *, enum lux::simulation::script::EScriptAwaitableCreateError> __cdecl lux::simulation::script::detail::ScriptExecution::admitAwaitable(struct lux::simulation::script::detail::ScriptExecution::ExecutionInstance &, class std::optional<struct lux::simulation::script::PreparedResumeType> const &, bool)+0x1b3>
		00000000000001af:  IMAGE_REL_AMD64_REL32	??$_Atomic_reinterpret_as@_J_K@std@@YA_JAEB_K@Z
     1b3: 49 8d 4f 08                  	leaq	0x8(%r15), %rcx
     1b7: 48 8b f8                     	movq	%rax, %rdi
     1ba: e8 00 00 00 00               	callq	0x1bf <private: class tl::expected<struct lux::simulation::script::detail::ScriptExecution::AwaitableRecord *, enum lux::simulation::script::EScriptAwaitableCreateError> __cdecl lux::simulation::script::detail::ScriptExecution::admitAwaitable(struct lux::simulation::script::detail::ScriptExecution::ExecutionInstance &, class std::optional<struct lux::simulation::script::PreparedResumeType> const &, bool)+0x1bf>
		00000000000001bb:  IMAGE_REL_AMD64_REL32	??$_Atomic_address_as@_JU?$_Atomic_padded@_K@std@@@std@@YAPEC_JAEAU?$_Atomic_padded@_K@0@@Z
     1bf: 48 89 38                     	movq	%rdi, (%rax)
     1c2: 41 38 6e 40                  	cmpb	%bpl, 0x40(%r14)
     1c6: 74 04                        	je	0x1cc <private: class tl::expected<struct lux::simulation::script::detail::ScriptExecution::AwaitableRecord *, enum lux::simulation::script::EScriptAwaitableCreateError> __cdecl lux::simulation::script::detail::ScriptExecution::admitAwaitable(struct lux::simulation::script::detail::ScriptExecution::ExecutionInstance &, class std::optional<struct lux::simulation::script::PreparedResumeType> const &, bool)+0x1cc>
     1c8: 41 8b 6e 30                  	movl	0x30(%r14), %ebp
     1cc: 48 8d 4c 24 50               	leaq	0x50(%rsp), %rcx
     1d1: 89 6c 24 50                  	movl	%ebp, 0x50(%rsp)
     1d5: e8 00 00 00 00               	callq	0x1da <private: class tl::expected<struct lux::simulation::script::detail::ScriptExecution::AwaitableRecord *, enum lux::simulation::script::EScriptAwaitableCreateError> __cdecl lux::simulation::script::detail::ScriptExecution::admitAwaitable(struct lux::simulation::script::detail::ScriptExecution::ExecutionInstance &, class std::optional<struct lux::simulation::script::PreparedResumeType> const &, bool)+0x1da>
		00000000000001d6:  IMAGE_REL_AMD64_REL32	??$_Atomic_reinterpret_as@HI@std@@YAHAEBI@Z
     1da: 49 8d 4f 10                  	leaq	0x10(%r15), %rcx
     1de: 8b f8                        	movl	%eax, %edi
     1e0: e8 00 00 00 00               	callq	0x1e5 <private: class tl::expected<struct lux::simulation::script::detail::ScriptExecution::AwaitableRecord *, enum lux::simulation::script::EScriptAwaitableCreateError> __cdecl lux::simulation::script::detail::ScriptExecution::admitAwaitable(struct lux::simulation::script::detail::ScriptExecution::ExecutionInstance &, class std::optional<struct lux::simulation::script::PreparedResumeType> const &, bool)+0x1e5>
		00000000000001e1:  IMAGE_REL_AMD64_REL32	??$_Atomic_address_as@HU?$_Atomic_padded@I@std@@@std@@YAPECHAEAU?$_Atomic_padded@I@0@@Z
     1e5: 48 8d 4c 24 50               	leaq	0x50(%rsp), %rcx
     1ea: 89 38                        	movl	%edi, (%rax)
     1ec: 48 b8 01 00 00 00 ff ff ff ff	movabsq	$-0xffffffff, %rax      # imm = 0xFFFFFFFF00000001
     1f6: 48 23 d8                     	andq	%rax, %rbx
     1f9: 90                           	nop
     1fa: 48 83 cb 01                  	orq	$0x1, %rbx
     1fe: 48 89 5c 24 50               	movq	%rbx, 0x50(%rsp)
     203: e8 00 00 00 00               	callq	0x208 <private: class tl::expected<struct lux::simulation::script::detail::ScriptExecution::AwaitableRecord *, enum lux::simulation::script::EScriptAwaitableCreateError> __cdecl lux::simulation::script::detail::ScriptExecution::admitAwaitable(struct lux::simulation::script::detail::ScriptExecution::ExecutionInstance &, class std::optional<struct lux::simulation::script::PreparedResumeType> const &, bool)+0x208>
		0000000000000204:  IMAGE_REL_AMD64_REL32	??$_Atomic_reinterpret_as@_J_K@std@@YA_JAEB_K@Z
     208: 49 8b cf                     	movq	%r15, %rcx
     20b: 48 8b d8                     	movq	%rax, %rbx
     20e: e8 00 00 00 00               	callq	0x213 <private: class tl::expected<struct lux::simulation::script::detail::ScriptExecution::AwaitableRecord *, enum lux::simulation::script::EScriptAwaitableCreateError> __cdecl lux::simulation::script::detail::ScriptExecution::admitAwaitable(struct lux::simulation::script::detail::ScriptExecution::ExecutionInstance &, class std::optional<struct lux::simulation::script::PreparedResumeType> const &, bool)+0x213>
		000000000000020f:  IMAGE_REL_AMD64_REL32	??$_Atomic_address_as@_JU?$_Atomic_padded@_K@std@@@std@@YAPEC_JAEAU?$_Atomic_padded@_K@0@@Z
     213: 48 89 18                     	movq	%rbx, (%rax)
     216: 4c 89 36                     	movq	%r14, (%rsi)
     219: c6 46 08 01                  	movb	$0x1, 0x8(%rsi)
     21d: e9 f4 fe ff ff               	jmp	0x116 <private: class tl::expected<struct lux::simulation::script::detail::ScriptExecution::AwaitableRecord *, enum lux::simulation::script::EScriptAwaitableCreateError> __cdecl lux::simulation::script::detail::ScriptExecution::admitAwaitable(struct lux::simulation::script::detail::ScriptExecution::ExecutionInstance &, class std::optional<struct lux::simulation::script::PreparedResumeType> const &, bool)+0x116>
     222: ff 15 00 00 00 00            	callq	*(%rip)                 # 0x228 <private: class tl::expected<struct lux::simulation::script::detail::ScriptExecution::AwaitableRecord *, enum lux::simulation::script::EScriptAwaitableCreateError> __cdecl lux::simulation::script::detail::ScriptExecution::admitAwaitable(struct lux::simulation::script::detail::ScriptExecution::ExecutionInstance &, class std::optional<struct lux::simulation::script::PreparedResumeType> const &, bool)+0x228>
		0000000000000224:  IMAGE_REL_AMD64_REL32	__imp_terminate
     228: cc                           	int3
     229: c6 06 04                     	movb	$0x4, (%rsi)
     22c: c6 46 08 00                  	movb	$0x0, 0x8(%rsi)
     230: e9 e6 fe ff ff               	jmp	0x11b <private: class tl::expected<struct lux::simulation::script::detail::ScriptExecution::AwaitableRecord *, enum lux::simulation::script::EScriptAwaitableCreateError> __cdecl lux::simulation::script::detail::ScriptExecution::admitAwaitable(struct lux::simulation::script::detail::ScriptExecution::ExecutionInstance &, class std::optional<struct lux::simulation::script::PreparedResumeType> const &, bool)+0x11b>



F 0000000000000000 <public: void __cdecl lux::simulation::script::detail::ScriptExecution::AwaitableStorage::releaseResolved(struct lux::simulation::script::detail::ScriptExecution::AwaitableRecord &)>:
       0: 48 89 5c 24 10               	movq	%rbx, 0x10(%rsp)
       5: 48 89 6c 24 18               	movq	%rbp, 0x18(%rsp)
       a: 56                           	pushq	%rsi
       b: 57                           	pushq	%rdi
       c: 41 56                        	pushq	%r14
       e: 48 83 ec 40                  	subq	$0x40, %rsp
      12: 8b 2a                        	movl	(%rdx), %ebp
      14: 45 33 f6                     	xorl	%r14d, %r14d
      17: 41 8b c6                     	movl	%r14d, %eax
      1a: 44 89 32                     	movl	%r14d, (%rdx)
      1d: 48 89 42 1c                  	movq	%rax, 0x1c(%rdx)
      21: 48 8b f1                     	movq	%rcx, %rsi
      24: 48 8b 8a 80 00 00 00         	movq	0x80(%rdx), %rcx
      2b: ff cd                        	decl	%ebp
      2d: 4c 89 74 24 60               	movq	%r14, 0x60(%rsp)
      32: 48 8b da                     	movq	%rdx, %rbx
      35: 48 85 c9                     	testq	%rcx, %rcx
      38: 74 0c                        	je	0x46 <public: void __cdecl lux::simulation::script::detail::ScriptExecution::AwaitableStorage::releaseResolved(struct lux::simulation::script::detail::ScriptExecution::AwaitableRecord &)+0x46>
      3a: 48 8b 92 98 00 00 00         	movq	0x98(%rdx), %rdx
      41: e8 00 00 00 00               	callq	0x46 <public: void __cdecl lux::simulation::script::detail::ScriptExecution::AwaitableStorage::releaseResolved(struct lux::simulation::script::detail::ScriptExecution::AwaitableRecord &)+0x46>
		0000000000000042:  IMAGE_REL_AMD64_REL32	??3@YAXPEAXW4align_val_t@std@@@Z
      46: 4c 89 b3 80 00 00 00         	movq	%r14, 0x80(%rbx)
      4d: 4c 89 b3 90 00 00 00         	movq	%r14, 0x90(%rbx)
      54: 48 c7 83 98 00 00 00 08 00 00 00     	movq	$0x8, 0x98(%rbx)
      5f: 4c 89 b3 88 00 00 00         	movq	%r14, 0x88(%rbx)
      66: 4c 89 74 24 20               	movq	%r14, 0x20(%rsp)
      6b: 4c 89 74 24 28               	movq	%r14, 0x28(%rsp)
      70: 0f 10 44 24 20               	movups	0x20(%rsp), %xmm0
      75: 44 89 74 24 32               	movl	%r14d, 0x32(%rsp)
      7a: 66 44 89 74 24 36            	movw	%r14w, 0x36(%rsp)
      80: 0f 11 43 48                  	movups	%xmm0, 0x48(%rbx)
      84: 66 c7 44 24 30 00 01         	movw	$0x100, 0x30(%rsp)      # imm = 0x100
      8b: f2 0f 10 4c 24 30            	movsd	0x30(%rsp), %xmm1
      91: f2 0f 11 4b 58               	movsd	%xmm1, 0x58(%rbx)
      96: 44 89 b3 a0 00 00 00         	movl	%r14d, 0xa0(%rbx)
      9d: 44 88 b3 a5 00 00 00         	movb	%r14b, 0xa5(%rbx)
      a4: 48 ff 4e 20                  	decq	0x20(%rsi)
      a8: 8b 43 04                     	movl	0x4(%rbx), %eax
      ab: 83 f8 ff                     	cmpl	$-0x1, %eax
      ae: 74 10                        	je	0xc0 <public: void __cdecl lux::simulation::script::detail::ScriptExecution::AwaitableStorage::releaseResolved(struct lux::simulation::script::detail::ScriptExecution::AwaitableRecord &)+0xc0>
      b0: ff c0                        	incl	%eax
      b2: 89 43 04                     	movl	%eax, 0x4(%rbx)
      b5: 8b 46 18                     	movl	0x18(%rsi), %eax
      b8: 89 43 18                     	movl	%eax, 0x18(%rbx)
      bb: 89 6e 18                     	movl	%ebp, 0x18(%rsi)
      be: eb 04                        	jmp	0xc4 <public: void __cdecl lux::simulation::script::detail::ScriptExecution::AwaitableStorage::releaseResolved(struct lux::simulation::script::detail::ScriptExecution::AwaitableRecord &)+0xc4>
      c0: 44 89 73 04                  	movl	%r14d, 0x4(%rbx)
      c4: 48 8b 5c 24 68               	movq	0x68(%rsp), %rbx
      c9: 48 8b 6c 24 70               	movq	0x70(%rsp), %rbp
      ce: 48 83 c4 40                  	addq	$0x40, %rsp
      d2: 41 5e                        	popq	%r14
      d4: 5f                           	popq	%rdi
      d5: 5e                           	popq	%rsi
      d6: c3                           	retq

