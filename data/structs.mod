p1 := {2, 3};

ts := [{[1]}, {[2]}];

ts2 := {ts[0], ts[1]};

function free_test(x: Int) {
    p1 := {1, 2};
    p2 := {3, 4};
    p3 := p1;
}
free_test(0);

x1 := p1.0;
x2 := {5, 6}.1;

triple := {1, {2, 3}};
pair := triple.1;
second := (triple.1).0;
third := triple.1.1;

pair2 := {1, {2, 3}}.1;

p2 := {x: 2, y: 3};
y := p2.y;

ps := [p2, {x: 4, y: 5}];

ps2 := ps ++ [{x: 5, y: 6}];

function f(p1: tuple{Int, Int}, p2: record{x: Int, y: Int}) -> Int {
    dx := p2.x - p1.1;
    dy := p2.y - p1.0;
    return dx * dx + dy * dy;
}

p3 := ps2[1];

q := f(p1, p3);

