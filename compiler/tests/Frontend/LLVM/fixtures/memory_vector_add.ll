target datalayout = "e-p:64:64"

define void @vector_add(i32* noalias %A, i32* noalias %B, i32* noalias %C, i32 %n) {
entry:
  br label %loop

loop:
  %i = phi i32 [ 0, %entry ], [ %latch, %loop ]
  %a.addr = getelementptr i32, i32* %A, i32 %i
  %b.addr = getelementptr i32, i32* %B, i32 %i
  %c.addr = getelementptr i32, i32* %C, i32 %i
  %a = load i32, i32* %a.addr, align 4
  %b = load i32, i32* %b.addr, align 4
  %sum = add i32 %a, %b
  store i32 %sum, i32* %c.addr, align 4
  %inc = add i32 %i, 1
  %latch = add i32 %inc, 0
  %done = icmp ult i32 %inc, %n
  br i1 %done, label %loop, label %exit

exit:
  ret void
}
