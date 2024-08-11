
function f(x: Int, y: Int) := 2 * x * y;

procedure g(x: Int, y: Int) -> Int {
    a := 3 * x;
    return a + y;
}

y := f(g(3, 1), 5);
assert(y == 100);

function h(x: Int, c: Int) := x * y + c;

assert(h(9, y) == 1000);

function j(x: Int) := f(x, x);

w := j(3);
assert(w == 18);

function double_array(xs: [Int]) := xs ++ xs;

doubled := double_array([1, 2, 3]);
assert(doubled[4] == 2);

