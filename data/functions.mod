
function f(x: Int, y: Int) := 2 * x * y;

procedure g(x: Int, y: Int) {
    a := 3 * x;
    return a + y;
}

y := f(g(3, 1), 5);

function h(x: Int, c: Int) := x * y + c;

z := h(9, y);

