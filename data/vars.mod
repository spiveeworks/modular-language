function f(x: Int) -> Int {
    var y := x;
    y = y + 2;
    y = y * x;
    return y;
}

result := f(3);

