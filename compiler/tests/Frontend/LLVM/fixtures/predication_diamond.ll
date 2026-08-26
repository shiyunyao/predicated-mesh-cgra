define i32 @diamond(i32 %x, i32 %y) {
entry:
  br label %loop
loop:
  %iv = phi i32 [ 0, %entry ], [ %inc, %merge ]
  br label %cond
cond:
  %p = icmp ult i32 %x, %y
  br i1 %p, label %then, label %else
then:
  %a = add i32 %x, %x
  br label %merge
else:
  %b = add i32 %y, %y
  br label %merge
merge:
  %v = phi i32 [ %a, %then ], [ %b, %else ]
  %inc = add i32 %iv, 1
  %done = icmp ult i32 %inc, 2
  br i1 %done, label %loop, label %exit
exit:
  %out = phi i32 [ %v, %merge ]
  ret i32 %out
}
