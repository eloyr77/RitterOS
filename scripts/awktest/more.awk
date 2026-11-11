# a comment
function swap(arr, i, j,   t) {
    t = arr[i]; arr[i] = arr[j]
    arr[j] = t
}
function inc(x) { x++; return x }
BEGIN {
    a[1] = "first"; a[2] = "second"
    swap(a, 1, 2)
    print a[1], a[2]
    y = 5; print inc(y), y
    s = "long string" \
        " continued"
    print s
    if (y > 3)
        print "big"
    else
        print "small"
    print (y > 3 ? (y > 4 ? "very" : "quite") : "not")
}
