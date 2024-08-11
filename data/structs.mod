p1 := {2, 3};
assert(p1.1 == 3);

ts := [{[1]}, {[2]}];
assert(ts[0].0[0] == 1);

ts2 := {ts[0], ts[1]};
assert(ts[1].0[0] == ts2.1.0[0]);

function free_test(x: Int) {
    p1 := {1, 2};
    p2 := {3, 4};
    p3 := p1;
}
free_test(0);

x1 := p1.0;
x2 := {5, 6}.1;
assert((x1 == 2) & (x2 == 6));

triple := {1, {2, 3}};
pair := triple.1;
second := (triple.1).0;
third := triple.1.1;
assert(second == 2);
assert(third == 3);

pair2 := {1, {2, 3}, 4}.1;
assert(pair2.1 == 3);

p2 := {x: 2, y: 3};
y := p2.y;
assert(y == 3);

ps := [p2, {x: 4, y: 5}];

ps2 := ps ++ [{x: 5, y: 6}];
assert(ps2[1].y == 5);

thing := {x: 1, y: [2, 3], z: [{4, 5}, {6, 7}]};
assert(thing.z[0].1 == 5);

function my_function(x: Int, y: Int, z: Int) := x + y + z;

function_result := my_function(thing.x, thing.y[0], thing.z[1].0);
assert(function_result == 9);

function f(p1: {Int, Int}, p2: {x: Int, y: Int}) -> Int {
    dx := p2.x - p1.1;
    dy := p2.y - p1.0;
    return dx * dx + dy * dy;
}

p3 := ps2[1];

q := f(p1, p3);
assert(q == 10);

substruct := {1, {2, 3}, [4]}.1;
assert(substruct.0 == 2);
