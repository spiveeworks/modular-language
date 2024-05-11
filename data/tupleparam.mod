function f(point: {Int, Int}) -> Int {
    x := point.0;
    return x + point.1;
}

p := {1, 2};

first_call := f(p);

q := {8, 8};
p_copy := p;

second_call := f({3, 4});
