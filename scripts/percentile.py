#!/usr/bin/env python3

import sys

def main():
    N = int(sys.argv[1])
    percentile = float(sys.argv[2])
    vals = [0.0] * N

    for i in range(N):
        vals[i] = input().split(',')[-1]

    vals.sort()

    idx = int(round(percentile * N + 0.5))
    print(vals[idx - 1])

if __name__ == '__main__':
    main()
