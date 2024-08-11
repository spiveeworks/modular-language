xs := [{1, 2}, {3, 4}, {5, 6}];
p1 := xs[0];
p2 := xs[1];
p3 := xs[0];
assert(p1.0 == 1);
assert(p2.1 == 4);
assert(p3.0 == xs[0].0);
