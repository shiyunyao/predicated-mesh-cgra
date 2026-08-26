define i32 @kernel(i32 %x) {
entry:
  br label %loop
loop:
  %iv = phi i32 [ 0, %entry ], [ %iv.next, %loop ]
  %iv.next = add i32 %iv, 1
  %y = add i32 %x, %iv
  %cmp = icmp ult i32 %iv.next, 4
  br i1 %cmp, label %loop, label %exit
exit:
  %result = phi i32 [ %y, %loop ]
  ret i32 %result
}
