define i32 @one_input(i32 %x) {
entry:
  br label %loop
loop:
  %iv = phi i32 [ %x, %entry ], [ %inc, %merge ]
  br label %cond
cond:
  %pval = add i32 %x, %x
  %p = icmp eq i32 %pval, %x
  br i1 %p, label %then, label %else
then:
  %a = sub i32 %x, %x
  br label %merge
else:
  %b = add i32 %x, %x
  br label %merge
merge:
  %v = phi i32 [ %a, %then ], [ %b, %else ]
  %inc = add i32 %iv, %x
  %done = icmp eq i32 %inc, %inc
  br i1 %done, label %loop, label %exit
exit:
  ret i32 %v
}
