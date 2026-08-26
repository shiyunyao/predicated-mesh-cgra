target datalayout = "e-p:64:64"

define void @memory_recurrence(i32* %A, i32 %n) {
entry:
  br label %loop

loop:
  %i = phi i32 [ 1, %entry ], [ %latch, %loop ]
  %previous = sub i32 %i, 1
  %read.addr = getelementptr i32, i32* %A, i32 %previous
  %write.addr = getelementptr i32, i32* %A, i32 %i
  %value = load i32, i32* %read.addr, align 4
  %next = add i32 %value, 1
  store i32 %next, i32* %write.addr, align 4
  %inc = add i32 %i, 1
  %latch = add i32 %inc, 0
  %done = icmp ult i32 %inc, %n
  br i1 %done, label %loop, label %exit

exit:
  ret void
}
