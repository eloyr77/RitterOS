function fact(n) { return n <= 1 ? 1 : n * fact(n - 1) }
function fill(arr, n,   i) { for (i = 1; i <= n; i++) arr[i] = i * i }
BEGIN { print fact(10); fill(sq, 5); for (i = 1; i <= 5; i++) printf "%d ", sq[i]; print "" }
