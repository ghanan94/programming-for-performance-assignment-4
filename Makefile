RM = rm -f
CC = gcc

CFLAGS = -g -std=c99 -lpthread -lssl -lcrypto -O3 -lm -I/usr/local/opt/openssl/include -L/usr/local/opt/openssl/lib

PERCENTILE_SCRIPT = scripts/percentile.py

default: all

all: bin speedup loadbalance original_speedup original_loadbalance

bin:
	mkdir bin

speedup: src/speedup.c
	$(CC) $< $(CFLAGS) -o bin/speedup

loadbalance: src/loadbalance.c
	$(CC) $< $(CFLAGS) -o bin/loadbalance

original_loadbalance: src/original_loadbalance.c
	$(CC) $< $(CFLAGS) -o bin/original_loadbalance

original_speedup: src/original_speedup.c
	$(CC) $< $(CFLAGS) -o bin/original_speedup

report: report.pdf

report.pdf: report/report.tex
	cd report && pdflatex report.tex && pdflatex report.tex

clean:
	$(RM) bin/loadbalance bin/speedup bin/original_speedup
	$(RM) report/*.aux report/*.log

run_part_1:
	bin/original_speedup -l 1 -n 2
	$(PERCENTILE_SCRIPT) 100000 0.9 < results.csv

	bin/speedup -l 1 -n 2
	$(PERCENTILE_SCRIPT) 100000 0.9 < results.csv

run_part_2:
	bin/loadbalance -b 0 -a 1 -j 10000 -l 250 -m 10000 -n 8
	$(PERCENTILE_SCRIPT) 10000 0.9 < results.csv

	bin/loadbalance -b 1 -a 1 -j 10000 -l 250 -m 10000 -n 8
	$(PERCENTILE_SCRIPT) 10000 0.9 < results.csv

	bin/loadbalance -b 0 -a 2 -j 10000 -l 250 -m 10000 -n 8
	$(PERCENTILE_SCRIPT) 10000 0.9 < results.csv

	bin/loadbalance -b 1 -a 2 -j 10000 -l 250 -m 10000 -n 8
	$(PERCENTILE_SCRIPT) 10000 0.9 < results.csv

.PHONY: original_speedup original_loadbalance speedup loadbalance report clean
