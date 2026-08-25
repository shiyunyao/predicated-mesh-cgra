define i32 @kernel(i32 %seed, i32 %n) {
entry:
  br label %loop
loop:
  %sum = phi i32 [ %seed, %entry ], [ %next, %loop ]
  %step = add i32 %sum, 1
  %next = add i32 %step, 0
  %iv = phi i32 [ 0, %entry ], [ %inc, %loop ]
  %inc = add i32 %iv, 1
  %cmp = icmp ult i32 %inc, %n
  br i1 %cmp, label %loop, label %exit
exit:
  %result = phi i32 [ %next, %loop ]
  ret i32 %result
}
