x := 3;

var y := x;
y = y + 2;
y = y * x;
assert(y == 15);

var p := {1, 2};
p = {p.1, p.0};
p_after := p;
assert(p.0 == 2);
assert(p.1 == 1);

var arr := [{1, [5, 6]}, {2, [3, 4]}];
arr[0].1 = [2];
arr[1].0 = 3;
arr[1].1[0] = 7;
arr_after := arr;
assert(arr[0].0 == 1);
assert(arr[0].1[0] == 2);
assert(arr[1].0 == 3);
assert(arr[1].1[0] == 7);
assert(arr[1].1[1] == 4);


