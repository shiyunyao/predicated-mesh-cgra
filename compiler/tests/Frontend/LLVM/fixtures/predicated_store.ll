define void @predicated_store(i32 %limit, i32* %out) {
entry:
  br label %loop
loop:
  %iv = phi i32 [ 0, %entry ], [ %inc, %merge ]
  br label %cond
cond:
  %p = icmp ult i32 %iv, %limit
  br i1 %p, label %then, label %else
then:
  store i32 %iv, i32* %out
  br label %merge
else:
  br label %merge
merge:
  %inc = add i32 %iv, 1
  %done = icmp ult i32 %inc, 4
  br i1 %done, label %loop, label %exit
exit:
  ret void
}
