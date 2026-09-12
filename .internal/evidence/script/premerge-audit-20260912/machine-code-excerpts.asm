
# 0-CppStaticScriptBridge.cpp.obj.asm; source sha256=eab0d9cd01c939dfe4d47da80e0b685a532324c0cb81dcb3939f0d63aeb8eb6c
 .text$mn:

0000000000000000 <public: bool __cdecl lux::simulation::script::detail::BoundedClassStorage::release(struct lux::simulation::script::detail::BoundedClassStorage::Allocation)>:
       0: 48 89 5c 24 18               	movq	%rbx, 0x18(%rsp)
       5: 48 89 6c 24 20               	movq	%rbp, 0x20(%rsp)
       a: 41 56                        	pushq	%r14
       c: 4c 8b 1a                     	movq	(%rdx), %r11
       f: 4c 8b ca                     	movq	%rdx, %r9
      12: 4c 8b c1                     	movq	%rcx, %r8
      15: 4d 85 db                     	testq	%r11, %r11
      18: 0f 84 6d 01 00 00            	je	0x18b <public: bool __cdecl lux::simulation::script::detail::BoundedClassStorage::release(struct lux::simulation::script::detail::BoundedClassStorage::Allocation)+0x18b>
      1e: 8b 5a 08                     	movl	0x8(%rdx), %ebx
      21: 48 b8 67 66 66 66 66 66 66 66	movabsq	$0x6666666666666667, %rax # imm = 0x6666666666666667
      2b: 48 8b 69 28                  	movq	0x28(%rcx), %rbp
      2f: 48 8b 49 30                  	movq	0x30(%rcx), %rcx
      33: 48 2b cd                     	subq	%rbp, %rcx
      36: 48 f7 e9                     	imulq	%rcx
      39: 48 c1 fa 04                  	sarq	$0x4, %rdx
      3d: 48 8b c2                     	movq	%rdx, %rax
      40: 48 c1 e8 3f                  	shrq	$0x3f, %rax
      44: 48 03 d0                     	addq	%rax, %rdx
      47: 48 3b da                     	cmpq	%rdx, %rbx
      4a: 0f 83 3b 01 00 00            	jae	0x18b <public: bool __cdecl lux::simulation::script::detail::BoundedClassStorage::release(struct lux::simulation::script::detail::BoundedClassStorage::Allocation)+0x18b>
      50: 41 8b 51 0c                  	movl	0xc(%r9), %edx
      54: 48 8d 04 9b                  	leaq	(%rbx,%rbx,4), %rax
      58: 4c 8d 34 c5 00 00 00 00      	leaq	(,%rax,8), %r14
      60: 41 3b 54 2e 10               	cmpl	0x10(%r14,%rbp), %edx
      65: 0f 83 20 01 00 00            	jae	0x18b <public: bool __cdecl lux::simulation::script::detail::BoundedClassStorage::release(struct lux::simulation::script::detail::BoundedClassStorage::Allocation)+0x18b>
      6b: 41 8b 44 2e 0c               	movl	0xc(%r14,%rbp), %eax
      70: 03 c2                        	addl	%edx, %eax
      72: 48 8d 0c 40                  	leaq	(%rax,%rax,2), %rcx
      76: 49 8b 40 40                  	movq	0x40(%r8), %rax
      7a: 4c 8d 14 c8                  	leaq	(%rax,%rcx,8), %r10
      7e: 41 8b 4c 2e 08               	movl	0x8(%r14,%rbp), %ecx
      83: 48 c1 e1 05                  	shlq	$0x5, %rcx
      87: 49 03 48 10                  	addq	0x10(%r8), %rcx
      8b: 41 80 7a 14 00               	cmpb	$0x0, 0x14(%r10)
      90: 0f 84 f5 00 00 00            	je	0x18b <public: bool __cdecl lux::simulation::script::detail::BoundedClassStorage::release(struct lux::simulation::script::detail::BoundedClassStorage::Allocation)+0x18b>
      96: 49 8b 41 10                  	movq	0x10(%r9), %rax
      9a: 49 39 02                     	cmpq	%rax, (%r10)
      9d: 0f 85 e8 00 00 00            	jne	0x18b <public: bool __cdecl lux::simulation::script::detail::BoundedClassStorage::release(struct lux::simulation::script::detail::BoundedClassStorage::Allocation)+0x18b>
      a3: 49 8b 41 18                  	movq	0x18(%r9), %rax
      a7: 49 39 42 08                  	cmpq	%rax, 0x8(%r10)
      ab: 0f 85 da 00 00 00            	jne	0x18b <public: bool __cdecl lux::simulation::script::detail::BoundedClassStorage::release(struct lux::simulation::script::detail::BoundedClassStorage::Allocation)+0x18b>
      b1: 8b c2                        	movl	%edx, %eax
      b3: 48 0f af 41 10               	imulq	0x10(%rcx), %rax
      b8: 49 03 00                     	addq	(%r8), %rax
      bb: 49 03 04 2e                  	addq	(%r14,%rbp), %rax
      bf: 4c 3b d8                     	cmpq	%rax, %r11
      c2: 0f 85 c3 00 00 00            	jne	0x18b <public: bool __cdecl lux::simulation::script::detail::BoundedClassStorage::release(struct lux::simulation::script::detail::BoundedClassStorage::Allocation)+0x18b>
      c8: 41 c6 42 14 00               	movb	$0x0, 0x14(%r10)
      cd: 4d 8d 98 c0 00 00 00         	leaq	0xc0(%r8), %r11
      d4: 41 ff 4c 2e 20               	decl	0x20(%r14,%rbp)
      d9: 49 ff 48 60                  	decq	0x60(%r8)
      dd: 41 80 78 78 00               	cmpb	$0x0, 0x78(%r8)
      e2: 74 1a                        	je	0xfe <public: bool __cdecl lux::simulation::script::detail::BoundedClassStorage::release(struct lux::simulation::script::detail::BoundedClassStorage::Allocation)+0xfe>
      e4: 49 83 03 03                  	addq	$0x3, (%r11)
      e8: 49 8b 42 08                  	movq	0x8(%r10), %rax
      ec: 49 29 80 98 00 00 00         	subq	%rax, 0x98(%r8)
      f3: 48 8b 41 10                  	movq	0x10(%rcx), %rax
      f7: 49 29 80 a0 00 00 00         	subq	%rax, 0xa0(%r8)
      fe: 33 d2                        	xorl	%edx, %edx
     100: 49 89 52 08                  	movq	%rdx, 0x8(%r10)
     104: 41 83 7c 2e 14 ff            	cmpl	$-0x1, 0x14(%r14,%rbp)
     10a: 75 5e                        	jne	0x16a <public: bool __cdecl lux::simulation::script::detail::BoundedClassStorage::release(struct lux::simulation::script::detail::BoundedClassStorage::Allocation)+0x16a>
     10c: 48 89 74 24 10               	movq	%rsi, 0x10(%rsp)
     111: 48 89 7c 24 18               	movq	%rdi, 0x18(%rsp)
     116: 49 8b 78 28                  	movq	0x28(%r8), %rdi
     11a: 49 03 fe                     	addq	%r14, %rdi
     11d: 8b 77 08                     	movl	0x8(%rdi), %esi
     120: 48 c1 e6 05                  	shlq	$0x5, %rsi
     124: 49 03 70 10                  	addq	0x10(%r8), %rsi
     128: c7 47 18 ff ff ff ff         	movl	$0xffffffff, 0x18(%rdi) # imm = 0xFFFFFFFF
     12f: 8b 46 18                     	movl	0x18(%rsi), %eax
     132: 89 47 1c                     	movl	%eax, 0x1c(%rdi)
     135: 8b 46 18                     	movl	0x18(%rsi), %eax
     138: 83 f8 ff                     	cmpl	$-0x1, %eax
     13b: 74 0c                        	je	0x149 <public: bool __cdecl lux::simulation::script::detail::BoundedClassStorage::release(struct lux::simulation::script::detail::BoundedClassStorage::Allocation)+0x149>
     13d: 48 8d 0c 80                  	leaq	(%rax,%rax,4), %rcx
     141: 49 8b 40 28                  	movq	0x28(%r8), %rax
     145: 89 5c c8 18                  	movl	%ebx, 0x18(%rax,%rcx,8)
     149: 89 5e 18                     	movl	%ebx, 0x18(%rsi)
     14c: 48 8b 74 24 10               	movq	0x10(%rsp), %rsi
     151: 41 38 50 78                  	cmpb	%dl, 0x78(%r8)
     155: 74 0e                        	je	0x165 <public: bool __cdecl lux::simulation::script::detail::BoundedClassStorage::release(struct lux::simulation::script::detail::BoundedClassStorage::Allocation)+0x165>
     157: 83 7f 1c ff                  	cmpl	$-0x1, 0x1c(%rdi)
     15b: 0f 95 c2                     	setne	%dl
     15e: 48 83 c2 02                  	addq	$0x2, %rdx
     162: 49 01 13                     	addq	%rdx, (%r11)
     165: 48 8b 7c 24 18               	movq	0x18(%rsp), %rdi
     16a: 41 8b 44 2e 14               	movl	0x14(%r14,%rbp), %eax
     16f: 41 89 42 10                  	movl	%eax, 0x10(%r10)
     173: 41 8b 41 0c                  	movl	0xc(%r9), %eax
     177: 41 89 44 2e 14               	movl	%eax, 0x14(%r14,%rbp)
     17c: b0 01                        	movb	$0x1, %al
     17e: 48 8b 5c 24 20               	movq	0x20(%rsp), %rbx
     183: 48 8b 6c 24 28               	movq	0x28(%rsp), %rbp
     188: 41 5e                        	popq	%r14
     18a: c3                           	retq
     18b: 48 8b 5c 24 20               	movq	0x20(%rsp), %rbx
     190: 32 c0                        	xorb	%al, %al
     192: 48 8b 6c 24 28               	movq	0x28(%rsp), %rbp
     197: 41 5e                        	popq	%r14
     199: c3                           	retq


# 0-CppStaticScriptBridge.cpp.obj.asm; source sha256=eab0d9cd01c939dfe4d47da80e0b685a532324c0cb81dcb3939f0d63aeb8eb6c
 .text$mn:

0000000000000000 <public: bool __cdecl lux::simulation::script::detail::BoundedClassStorage::release(struct lux::simulation::script::detail::BoundedClassStorage::Ticket)>:
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


# 3-ScriptSystem.cpp.obj.asm; source sha256=d0ba78cc60983abb446a0b0467b7e25c1831b8dbc3c3b482494c20bedf1022ac
 .text$mn:

0000000000000000 <public: struct lux::simulation::script::ScriptOwnedResumeValue & __cdecl lux::simulation::script::ScriptOwnedResumeValue::operator=(struct lux::simulation::script::ScriptOwnedResumeValue &&)>:
       0: 48 89 5c 24 10               	movq	%rbx, 0x10(%rsp)
       5: 48 89 74 24 18               	movq	%rsi, 0x18(%rsp)
       a: 57                           	pushq	%rdi
       b: 48 83 ec 20                  	subq	$0x20, %rsp
       f: 0f 10 02                     	movups	(%rdx), %xmm0
      12: 48 8d 72 18                  	leaq	0x18(%rdx), %rsi
      16: 48 8b d9                     	movq	%rcx, %rbx
      19: 48 8d 79 18                  	leaq	0x18(%rcx), %rdi
      1d: 0f 11 01                     	movups	%xmm0, (%rcx)
      20: f2 0f 10 4a 10               	movsd	0x10(%rdx), %xmm1
      25: f2 0f 11 49 10               	movsd	%xmm1, 0x10(%rcx)
      2a: 48 3b fe                     	cmpq	%rsi, %rdi
      2d: 74 7d                        	je	0xac <public: struct lux::simulation::script::ScriptOwnedResumeValue & __cdecl lux::simulation::script::ScriptOwnedResumeValue::operator=(struct lux::simulation::script::ScriptOwnedResumeValue &&)+0xac>
      2f: 48 8b 4f 20                  	movq	0x20(%rdi), %rcx
      33: 4c 89 74 24 30               	movq	%r14, 0x30(%rsp)
      38: 48 85 c9                     	testq	%rcx, %rcx
      3b: 74 09                        	je	0x46 <public: struct lux::simulation::script::ScriptOwnedResumeValue & __cdecl lux::simulation::script::ScriptOwnedResumeValue::operator=(struct lux::simulation::script::ScriptOwnedResumeValue &&)+0x46>
      3d: 48 8b 57 38                  	movq	0x38(%rdi), %rdx
      41: e8 00 00 00 00               	callq	0x46 <public: struct lux::simulation::script::ScriptOwnedResumeValue & __cdecl lux::simulation::script::ScriptOwnedResumeValue::operator=(struct lux::simulation::script::ScriptOwnedResumeValue &&)+0x46>
		0000000000000042:  IMAGE_REL_AMD64_REL32	??3@YAXPEAXW4align_val_t@std@@@Z
      46: 33 c9                        	xorl	%ecx, %ecx
      48: 48 c7 47 38 08 00 00 00      	movq	$0x8, 0x38(%rdi)
      50: 48 89 4f 28                  	movq	%rcx, 0x28(%rdi)
      54: 48 89 4f 20                  	movq	%rcx, 0x20(%rdi)
      58: 48 89 4f 30                  	movq	%rcx, 0x30(%rdi)
      5c: 4c 8b 46 28                  	movq	0x28(%rsi), %r8
      60: 48 89 4e 28                  	movq	%rcx, 0x28(%rsi)
      64: 4c 89 47 28                  	movq	%r8, 0x28(%rdi)
      68: 48 8b 46 20                  	movq	0x20(%rsi), %rax
      6c: 48 85 c0                     	testq	%rax, %rax
      6f: 74 26                        	je	0x97 <public: struct lux::simulation::script::ScriptOwnedResumeValue & __cdecl lux::simulation::script::ScriptOwnedResumeValue::operator=(struct lux::simulation::script::ScriptOwnedResumeValue &&)+0x97>
      71: 48 89 4e 20                  	movq	%rcx, 0x20(%rsi)
      75: 48 89 47 20                  	movq	%rax, 0x20(%rdi)
      79: 48 8b 46 30                  	movq	0x30(%rsi), %rax
      7d: 48 89 4e 30                  	movq	%rcx, 0x30(%rsi)
      81: 48 89 47 30                  	movq	%rax, 0x30(%rdi)
      85: 48 8b 46 38                  	movq	0x38(%rsi), %rax
      89: 48 c7 46 38 08 00 00 00      	movq	$0x8, 0x38(%rsi)
      91: 48 89 47 38                  	movq	%rax, 0x38(%rdi)
      95: eb 10                        	jmp	0xa7 <public: struct lux::simulation::script::ScriptOwnedResumeValue & __cdecl lux::simulation::script::ScriptOwnedResumeValue::operator=(struct lux::simulation::script::ScriptOwnedResumeValue &&)+0xa7>
      97: 4d 85 c0                     	testq	%r8, %r8
      9a: 74 0b                        	je	0xa7 <public: struct lux::simulation::script::ScriptOwnedResumeValue & __cdecl lux::simulation::script::ScriptOwnedResumeValue::operator=(struct lux::simulation::script::ScriptOwnedResumeValue &&)+0xa7>
      9c: 48 8b d6                     	movq	%rsi, %rdx
      9f: 48 8b cf                     	movq	%rdi, %rcx
      a2: e8 00 00 00 00               	callq	0xa7 <public: struct lux::simulation::script::ScriptOwnedResumeValue & __cdecl lux::simulation::script::ScriptOwnedResumeValue::operator=(struct lux::simulation::script::ScriptOwnedResumeValue &&)+0xa7>
		00000000000000a3:  IMAGE_REL_AMD64_REL32	memcpy
      a7: 4c 8b 74 24 30               	movq	0x30(%rsp), %r14
      ac: 48 8b 74 24 40               	movq	0x40(%rsp), %rsi
      b1: 48 8b c3                     	movq	%rbx, %rax
      b4: 48 8b 5c 24 38               	movq	0x38(%rsp), %rbx
      b9: 48 83 c4 20                  	addq	$0x20, %rsp
      bd: 5f                           	popq	%rdi
      be: c3                           	retq


# 3-ScriptSystem.cpp.obj.asm; source sha256=d0ba78cc60983abb446a0b0467b7e25c1831b8dbc3c3b482494c20bedf1022ac
 .text$mn:

0000000000000000 <private: class tl::expected<struct lux::simulation::script::detail::ScriptExecution::AwaitableRecord *, enum lux::simulation::script::EScriptAwaitableCreateError> __cdecl lux::simulation::script::detail::ScriptExecution::admitAwaitable(struct lux::simulation::script::detail::ScriptExecution::ExecutionInstance &, class std::optional<struct lux::simulation::script::PreparedResumeType>, bool)>:
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


# 3-ScriptSystem.cpp.obj.asm; source sha256=d0ba78cc60983abb446a0b0467b7e25c1831b8dbc3c3b482494c20bedf1022ac
 .text$mn:

0000000000000000 <public: bool __cdecl lux::simulation::script::ScriptOwnedBytes::resize(unsigned __int64, unsigned __int64)>:
       0: 48 89 54 24 10               	movq	%rdx, 0x10(%rsp)
       5: 53                           	pushq	%rbx
       6: 55                           	pushq	%rbp
       7: 56                           	pushq	%rsi
       8: 57                           	pushq	%rdi
       9: 41 54                        	pushq	%r12
       b: 48 83 ec 20                  	subq	$0x20, %rsp
       f: 49 8b f8                     	movq	%r8, %rdi
      12: 48 8b f2                     	movq	%rdx, %rsi
      15: 48 8b d9                     	movq	%rcx, %rbx
      18: 4d 85 c0                     	testq	%r8, %r8
      1b: 0f 84 2f 01 00 00            	je	0x150 <public: bool __cdecl lux::simulation::script::ScriptOwnedBytes::resize(unsigned __int64, unsigned __int64)+0x150>
      21: 49 8d 40 ff                  	leaq	-0x1(%r8), %rax
      25: 49 85 c0                     	testq	%rax, %r8
      28: 0f 85 22 01 00 00            	jne	0x150 <public: bool __cdecl lux::simulation::script::ScriptOwnedBytes::resize(unsigned __int64, unsigned __int64)+0x150>
      2e: 48 83 fa 20                  	cmpq	$0x20, %rdx
      32: 77 4d                        	ja	0x81 <public: bool __cdecl lux::simulation::script::ScriptOwnedBytes::resize(unsigned __int64, unsigned __int64)+0x81>
      34: 49 83 f8 08                  	cmpq	$0x8, %r8
      38: 77 47                        	ja	0x81 <public: bool __cdecl lux::simulation::script::ScriptOwnedBytes::resize(unsigned __int64, unsigned __int64)+0x81>
      3a: 48 8b 41 28                  	movq	0x28(%rcx), %rax
      3e: 48 8d 79 28                  	leaq	0x28(%rcx), %rdi
      42: 48 85 c0                     	testq	%rax, %rax
      45: 74 22                        	je	0x69 <public: bool __cdecl lux::simulation::script::ScriptOwnedBytes::resize(unsigned __int64, unsigned __int64)+0x69>
      47: 48 3b d0                     	cmpq	%rax, %rdx
      4a: 4c 8d 44 24 58               	leaq	0x58(%rsp), %r8
      4f: 48 8b 41 20                  	movq	0x20(%rcx), %rax
      53: 48 8b d1                     	movq	%rcx, %rdx
      56: 4c 0f 43 c7                  	cmovaeq	%rdi, %r8
      5a: 48 85 c0                     	testq	%rax, %rax
      5d: 48 0f 45 d0                  	cmovneq	%rax, %rdx
      61: 4d 8b 00                     	movq	(%r8), %r8
      64: e8 00 00 00 00               	callq	0x69 <public: bool __cdecl lux::simulation::script::ScriptOwnedBytes::resize(unsigned __int64, unsigned __int64)+0x69>
		0000000000000065:  IMAGE_REL_AMD64_REL32	memcpy
      69: 48 8b cb                     	movq	%rbx, %rcx
      6c: e8 00 00 00 00               	callq	0x71 <public: bool __cdecl lux::simulation::script::ScriptOwnedBytes::resize(unsigned __int64, unsigned __int64)+0x71>
		000000000000006d:  IMAGE_REL_AMD64_REL32	?releaseSpill@ScriptOwnedBytes@script@simulation@lux@@AEAAXXZ
      71: b0 01                        	movb	$0x1, %al
      73: 48 89 37                     	movq	%rsi, (%rdi)
      76: 48 83 c4 20                  	addq	$0x20, %rsp
      7a: 41 5c                        	popq	%r12
      7c: 5f                           	popq	%rdi
      7d: 5e                           	popq	%rsi
      7e: 5d                           	popq	%rbp
      7f: 5b                           	popq	%rbx
      80: c3                           	retq
      81: 41 bc 08 00 00 00            	movl	$0x8, %r12d
      87: 49 3b fc                     	cmpq	%r12, %rdi
      8a: 49 0f 42 fc                  	cmovbq	%r12, %rdi
      8e: 48 83 79 20 00               	cmpq	$0x0, 0x20(%rcx)
      93: 74 1d                        	je	0xb2 <public: bool __cdecl lux::simulation::script::ScriptOwnedBytes::resize(unsigned __int64, unsigned __int64)+0xb2>
      95: 48 39 71 30                  	cmpq	%rsi, 0x30(%rcx)
      99: 72 17                        	jb	0xb2 <public: bool __cdecl lux::simulation::script::ScriptOwnedBytes::resize(unsigned __int64, unsigned __int64)+0xb2>
      9b: 48 39 79 38                  	cmpq	%rdi, 0x38(%rcx)
      9f: 72 11                        	jb	0xb2 <public: bool __cdecl lux::simulation::script::ScriptOwnedBytes::resize(unsigned __int64, unsigned __int64)+0xb2>
      a1: 48 89 71 28                  	movq	%rsi, 0x28(%rcx)
      a5: b0 01                        	movb	$0x1, %al
      a7: 48 83 c4 20                  	addq	$0x20, %rsp
      ab: 41 5c                        	popq	%r12
      ad: 5f                           	popq	%rdi
      ae: 5e                           	popq	%rsi
      af: 5d                           	popq	%rbp
      b0: 5b                           	popq	%rbx
      b1: c3                           	retq
      b2: 4c 8d 05 00 00 00 00         	leaq	(%rip), %r8             # 0xb9 <public: bool __cdecl lux::simulation::script::ScriptOwnedBytes::resize(unsigned __int64, unsigned __int64)+0xb9>
		00000000000000b5:  IMAGE_REL_AMD64_REL32	?nothrow@std@@3Unothrow_t@1@B
      b9: 48 8b d7                     	movq	%rdi, %rdx
      bc: 48 8b ce                     	movq	%rsi, %rcx
      bf: e8 00 00 00 00               	callq	0xc4 <public: bool __cdecl lux::simulation::script::ScriptOwnedBytes::resize(unsigned __int64, unsigned __int64)+0xc4>
		00000000000000c0:  IMAGE_REL_AMD64_REL32	??2@YAPEAX_KW4align_val_t@std@@AEBUnothrow_t@1@@Z
      c4: 48 8b e8                     	movq	%rax, %rbp
      c7: 48 85 c0                     	testq	%rax, %rax
      ca: 0f 84 80 00 00 00            	je	0x150 <public: bool __cdecl lux::simulation::script::ScriptOwnedBytes::resize(unsigned __int64, unsigned __int64)+0x150>
      d0: 48 8b 43 28                  	movq	0x28(%rbx), %rax
      d4: 4c 89 74 24 50               	movq	%r14, 0x50(%rsp)
      d9: 4c 8d 73 28                  	leaq	0x28(%rbx), %r14
      dd: 4c 89 7c 24 60               	movq	%r15, 0x60(%rsp)
      e2: 4d 8b fe                     	movq	%r14, %r15
      e5: 48 85 c0                     	testq	%rax, %rax
      e8: 74 29                        	je	0x113 <public: bool __cdecl lux::simulation::script::ScriptOwnedBytes::resize(unsigned __int64, unsigned __int64)+0x113>
      ea: 48 3b f0                     	cmpq	%rax, %rsi
      ed: 4c 8d 44 24 58               	leaq	0x58(%rsp), %r8
      f2: 48 8b 43 20                  	movq	0x20(%rbx), %rax
      f6: 48 8b d3                     	movq	%rbx, %rdx
      f9: 4d 0f 43 c6                  	cmovaeq	%r14, %r8
      fd: 48 8b cd                     	movq	%rbp, %rcx
     100: 48 85 c0                     	testq	%rax, %rax
     103: 48 0f 45 d0                  	cmovneq	%rax, %rdx
     107: 4d 8b 00                     	movq	(%r8), %r8
     10a: e8 00 00 00 00               	callq	0x10f <public: bool __cdecl lux::simulation::script::ScriptOwnedBytes::resize(unsigned __int64, unsigned __int64)+0x10f>
		000000000000010b:  IMAGE_REL_AMD64_REL32	memcpy
     10f: 4c 8d 7b 28                  	leaq	0x28(%rbx), %r15
     113: 48 8b 4b 20                  	movq	0x20(%rbx), %rcx
     117: 48 85 c9                     	testq	%rcx, %rcx
     11a: 74 0b                        	je	0x127 <public: bool __cdecl lux::simulation::script::ScriptOwnedBytes::resize(unsigned __int64, unsigned __int64)+0x127>
     11c: 48 8b 53 38                  	movq	0x38(%rbx), %rdx
     120: e8 00 00 00 00               	callq	0x125 <public: bool __cdecl lux::simulation::script::ScriptOwnedBytes::resize(unsigned __int64, unsigned __int64)+0x125>
		0000000000000121:  IMAGE_REL_AMD64_REL32	??3@YAXPEAXW4align_val_t@std@@@Z
     125: eb 03                        	jmp	0x12a <public: bool __cdecl lux::simulation::script::ScriptOwnedBytes::resize(unsigned __int64, unsigned __int64)+0x12a>
     127: 4d 8b fe                     	movq	%r14, %r15
     12a: 4c 8b 74 24 50               	movq	0x50(%rsp), %r14
     12f: b0 01                        	movb	$0x1, %al
     131: 49 89 37                     	movq	%rsi, (%r15)
     134: 4c 8b 7c 24 60               	movq	0x60(%rsp), %r15
     139: 48 89 6b 20                  	movq	%rbp, 0x20(%rbx)
     13d: 48 89 73 30                  	movq	%rsi, 0x30(%rbx)
     141: 48 89 7b 38                  	movq	%rdi, 0x38(%rbx)
     145: 48 83 c4 20                  	addq	$0x20, %rsp
     149: 41 5c                        	popq	%r12
     14b: 5f                           	popq	%rdi
     14c: 5e                           	popq	%rsi
     14d: 5d                           	popq	%rbp
     14e: 5b                           	popq	%rbx
     14f: c3                           	retq
     150: 32 c0                        	xorb	%al, %al
     152: 48 83 c4 20                  	addq	$0x20, %rsp
     156: 41 5c                        	popq	%r12
     158: 5f                           	popq	%rdi
     159: 5e                           	popq	%rsi
     15a: 5d                           	popq	%rbp
     15b: 5b                           	popq	%rbx
     15c: c3                           	retq


# 3-ScriptSystem.cpp.obj.asm; source sha256=d0ba78cc60983abb446a0b0467b7e25c1831b8dbc3c3b482494c20bedf1022ac
 .text$mn:

0000000000000000 <private: bool __cdecl lux::simulation::script::detail::ScriptExecution::takeAwaitable(struct lux::simulation::script::detail::ScriptExecution::AwaitableRecord &, struct lux::simulation::script::detail::ScriptExecution::ExecutionAccess, class std::optional<struct lux::simulation::script::detail::ScriptExecution::AwaitableOutcome> &)>:
       0: 48 89 74 24 18               	movq	%rsi, 0x18(%rsp)
       5: 57                           	pushq	%rdi
       6: 41 56                        	pushq	%r14
       8: 41 57                        	pushq	%r15
       a: 48 83 ec 30                  	subq	$0x30, %rsp
       e: 48 8d 42 24                  	leaq	0x24(%rdx), %rax
      12: 48 8b f2                     	movq	%rdx, %rsi
      15: 0f b6 10                     	movzbl	(%rax), %edx
      18: 49 8b f9                     	movq	%r9, %rdi
      1b: 4d 8b f0                     	movq	%r8, %r14
      1e: 4c 8b f9                     	movq	%rcx, %r15
      21: 80 fa 01                     	cmpb	$0x1, %dl
      24: 74 16                        	je	0x3c <private: bool __cdecl lux::simulation::script::detail::ScriptExecution::takeAwaitable(struct lux::simulation::script::detail::ScriptExecution::AwaitableRecord &, struct lux::simulation::script::detail::ScriptExecution::ExecutionAccess, class std::optional<struct lux::simulation::script::detail::ScriptExecution::AwaitableOutcome> &)+0x3c>
      26: 80 fa 03                     	cmpb	$0x3, %dl
      29: 74 11                        	je	0x3c <private: bool __cdecl lux::simulation::script::detail::ScriptExecution::takeAwaitable(struct lux::simulation::script::detail::ScriptExecution::AwaitableRecord &, struct lux::simulation::script::detail::ScriptExecution::ExecutionAccess, class std::optional<struct lux::simulation::script::detail::ScriptExecution::AwaitableOutcome> &)+0x3c>
      2b: 32 c0                        	xorb	%al, %al
      2d: 48 8b 74 24 60               	movq	0x60(%rsp), %rsi
      32: 48 83 c4 30                  	addq	$0x30, %rsp
      36: 41 5f                        	popq	%r15
      38: 41 5e                        	popq	%r14
      3a: 5f                           	popq	%rdi
      3b: c3                           	retq
      3c: 41 80 79 68 00               	cmpb	$0x0, 0x68(%r9)
      41: 48 89 5c 24 50               	movq	%rbx, 0x50(%rsp)
      46: 48 89 6c 24 58               	movq	%rbp, 0x58(%rsp)
      4b: 74 31                        	je	0x7e <private: bool __cdecl lux::simulation::script::detail::ScriptExecution::takeAwaitable(struct lux::simulation::script::detail::ScriptExecution::AwaitableRecord &, struct lux::simulation::script::detail::ScriptExecution::ExecutionAccess, class std::optional<struct lux::simulation::script::detail::ScriptExecution::AwaitableOutcome> &)+0x7e>
      4d: 49 8b 49 40                  	movq	0x40(%r9), %rcx
      51: 49 8d 59 58                  	leaq	0x58(%r9), %rbx
      55: 48 85 c9                     	testq	%rcx, %rcx
      58: 74 0c                        	je	0x66 <private: bool __cdecl lux::simulation::script::detail::ScriptExecution::takeAwaitable(struct lux::simulation::script::detail::ScriptExecution::AwaitableRecord &, struct lux::simulation::script::detail::ScriptExecution::ExecutionAccess, class std::optional<struct lux::simulation::script::detail::ScriptExecution::AwaitableOutcome> &)+0x66>
      5a: 48 8b 13                     	movq	(%rbx), %rdx
      5d: e8 00 00 00 00               	callq	0x62 <private: bool __cdecl lux::simulation::script::detail::ScriptExecution::takeAwaitable(struct lux::simulation::script::detail::ScriptExecution::AwaitableRecord &, struct lux::simulation::script::detail::ScriptExecution::ExecutionAccess, class std::optional<struct lux::simulation::script::detail::ScriptExecution::AwaitableOutcome> &)+0x62>
		000000000000005e:  IMAGE_REL_AMD64_REL32	??3@YAXPEAXW4align_val_t@std@@@Z
      62: 48 8d 46 24                  	leaq	0x24(%rsi), %rax
      66: 33 c9                        	xorl	%ecx, %ecx
      68: 48 c7 03 08 00 00 00         	movq	$0x8, (%rbx)
      6f: 48 89 4f 40                  	movq	%rcx, 0x40(%rdi)
      73: 48 89 4f 50                  	movq	%rcx, 0x50(%rdi)
      77: 48 89 4f 48                  	movq	%rcx, 0x48(%rdi)
      7b: 88 4f 68                     	movb	%cl, 0x68(%rdi)
      7e: 0f b6 00                     	movzbl	(%rax), %eax
      81: 48 8d 4f 08                  	leaq	0x8(%rdi), %rcx
      85: 8b 9e a0 00 00 00            	movl	0xa0(%rsi), %ebx
      8b: 48 8d 56 48                  	leaq	0x48(%rsi), %rdx
      8f: 88 07                        	movb	%al, (%rdi)
      91: e8 00 00 00 00               	callq	0x96 <private: bool __cdecl lux::simulation::script::detail::ScriptExecution::takeAwaitable(struct lux::simulation::script::detail::ScriptExecution::AwaitableRecord &, struct lux::simulation::script::detail::ScriptExecution::ExecutionAccess, class std::optional<struct lux::simulation::script::detail::ScriptExecution::AwaitableOutcome> &)+0x96>
		0000000000000092:  IMAGE_REL_AMD64_REL32	??0ScriptOwnedResumeValue@script@simulation@lux@@QEAA@$$QEAU0123@@Z
      96: 89 5f 60                     	movl	%ebx, 0x60(%rdi)
      99: 4c 8d 44 24 20               	leaq	0x20(%rsp), %r8
      9e: 41 0f 10 06                  	movups	(%r14), %xmm0
      a2: 48 8b d6                     	movq	%rsi, %rdx
      a5: c6 47 68 01                  	movb	$0x1, 0x68(%rdi)
      a9: 49 8b cf                     	movq	%r15, %rcx
      ac: 0f 29 44 24 20               	movaps	%xmm0, 0x20(%rsp)
      b1: e8 00 00 00 00               	callq	0xb6 <private: bool __cdecl lux::simulation::script::detail::ScriptExecution::takeAwaitable(struct lux::simulation::script::detail::ScriptExecution::AwaitableRecord &, struct lux::simulation::script::detail::ScriptExecution::ExecutionAccess, class std::optional<struct lux::simulation::script::detail::ScriptExecution::AwaitableOutcome> &)+0xb6>
		00000000000000b2:  IMAGE_REL_AMD64_REL32	?eraseAwaitableRecord@ScriptExecution@detail@script@simulation@lux@@AEAA_NAEAUAwaitableRecord@12345@UExecutionAccess@12345@@Z
      b6: 48 8b 6c 24 58               	movq	0x58(%rsp), %rbp
      bb: b0 01                        	movb	$0x1, %al
      bd: 48 8b 5c 24 50               	movq	0x50(%rsp), %rbx
      c2: 48 8b 74 24 60               	movq	0x60(%rsp), %rsi
      c7: 48 83 c4 30                  	addq	$0x30, %rsp
      cb: 41 5f                        	popq	%r15
      cd: 41 5e                        	popq	%r14
      cf: 5f                           	popq	%rdi
      d0: c3                           	retq
