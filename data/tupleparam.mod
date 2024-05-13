function f(point: {Int, Int}) -> Int {
    x := point.0;
    return x + point.1;
}

function g(point: {Int, Int}) -> {Int, Int} {
    return {point.1, point.0};
}

p := {1, 2};

g_to_s := f(p);

q := {8, 8};
p_copy := p;

l_to_s := f({3, 4});

g_to_l := g(p);

l_to_l := g({5, 6});

function h(x: Int) -> {Int, Int} {
    return {x, x + 1};
}

s_to_l := h(5);

gs := [g];

g_to_s2 := gs[0](p);
l_to_s2 := gs[0]({7, 8});
