RM = rm -f
CC = gcc
CFLAGS = -std=c99 -lpthread -lssl -lcrypto -O3 -lm

default: all

all: bin speedup loadbalance original_speedup

bin:
	mkdir bin

speedup: src/speedup.c
	$(CC) $< $(CFLAGS) -o bin/speedup

loadbalance: src/loadbalance.c
	$(CC) $< $(CFLAGS) -o bin/loadbalance

original_speedup: src/original_speedup.c
	$(CC) $< $(CFLAGS) -o bin/original_speedup

report: report.pdf

report.pdf: report/report.tex
	cd report && pdflatex report.tex && pdflatex report.tex

clean:
	$(RM) bin/loadbalance
	$(RM) report/*.aux report/*.log

.PHONY: speedup loadbalance report clean
