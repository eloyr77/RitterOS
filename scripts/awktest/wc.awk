{ for (i = 1; i <= NF; i++) count[$i]++ }
END { n = 0; for (w in count) n++; print n, NR }
