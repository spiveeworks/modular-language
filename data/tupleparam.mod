function f(point: {Int, Int}) -> Int {
    x := point.0;
    return x + point.1;
}

function g(point: {Int, Int}) -> {Int, Int} {
    return {point.1, point.0};
}

p := {1, 2};

g_to_s := f(p);
assert(g_to_s == 3);

q := {8, 8};
p_copy := p;

l_to_s := f({3, 4});
assert(l_to_s == 7);

g_to_l := g(p);
assert(g_to_l.1 == 1);

l_to_l := g({5, 6});
assert(l_to_l.0 == 6);

function h(x: Int) -> {Int, Int} {
    return {x, x + 1};
}

s_to_l := h(5);
assert(s_to_l.0 == 5);

gs := [g];

g_to_s2 := gs[0](p);
l_to_s2 := gs[0]({7, 8});
assert(l_to_s2.1 == 7);
